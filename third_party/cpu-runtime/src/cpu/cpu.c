/* guest-cpu: per-thread guest CPU state and host -> guest calls. */
#include "cpu_internal.h"
#include "trace.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#if defined(__aarch64__) && defined(__APPLE__)
#include <sys/sysctl.h>
#elif defined(__aarch64__) && defined(__linux__)
#include <sys/auxv.h>
#endif

/* The guest's stacks stay below 1 TiB like the PS4's (runtime_memory.c). */
void *runtime_low_map(size_t size, int prot);

enum { GUEST_STACK = 1 << 20, RED_ZONE = 128 };
/* Return addresses of host -> guest calls: never guest code, never a host function. */
#define SENTINEL(depth) (UINT64_C(0x0000700000000000) + (uint64_t)(depth) * 16)

static _Thread_local GuestCpu *tls_cpu;
static _Thread_local uint64_t tls_thread_pointer;
static uint32_t tls_displacement;
static uint64_t cpus_created;

int guest_cpu_enabled(void) {
#if defined(__aarch64__)
    return 1;
#else
    static int enabled = -1;
    if (enabled < 0) {
        const char *env = getenv("GUEST_CPU");
        enabled = env && env[0] == 'i';
    }
    return enabled;
#endif
}

void guest_cpu_set_tls_displacement(uint32_t displacement) { tls_displacement = displacement; }

static void apply_thread_pointer(GuestCpu *cpu) {
    cpu->tcb = tls_thread_pointer;
    cpu->fs_base = tls_thread_pointer;
    /* The loader rewrote `mov rax, fs:[0]` into `mov rax, gs:[displacement]`. */
    cpu->gs_base = (uint64_t)(uintptr_t)&cpu->tcb - tls_displacement;
}

void guest_cpu_set_thread_pointer(uintptr_t tcb) {
    tls_thread_pointer = tcb;
    if (tls_cpu) apply_thread_pointer(tls_cpu);
}

#if defined(__aarch64__)
/* FEAT_AFP: FPCR.FIZ flushes denormal inputs on its own (Apple M3 and later; not M1/M2). */
static int host_has_afp(void) {
    static int afp = -1;
    if (afp < 0) {
        afp = 0;
#if defined(__aarch64__) && defined(__APPLE__)
        int v = 0;
        size_t len = sizeof(v);
        if (sysctlbyname("hw.optional.arm.FEAT_AFP", &v, &len, NULL, 0) == 0) afp = v != 0;
#elif defined(__aarch64__) && defined(__linux__) && defined(HWCAP2_AFP)
        afp = (getauxval(AT_HWCAP2) & HWCAP2_AFP) != 0;
#endif
    }
    return afp;
}
#endif

/* Guest MXCSR FTZ (bit 15, flush denormal results) and DAZ (bit 6, denormal inputs read as zero)
 * -> this host thread's FPCR, so the native float code the translator emits flushes denormals
 * as the guest asked. FPCR.FZ (bit 24) flushes inputs and outputs together (FPCR.AH = 0).
 * Exact mappings, on by default:
 *   - FTZ and DAZ: FZ.
 *   - DAZ only, with FEAT_AFP (Apple M3 and later): FIZ (bit 0) flushes inputs alone.
 * Approximate mappings, only with GUEST_CPU_FTZ=1 (neither choice is exact, so by default the thread
 * keeps the host's behaviour):
 *   - FTZ only (Bloodborne's Havok workers, inFAMOUS: MXCSR 0x9f80): FZ. The thread's results are
 *     flushed as on x86, and denormals it loads are flushed too, where x86 would use them.
 *     Without it, its denormal results are kept, which x86 flushes.
 *   - DAZ only, without FEAT_AFP (M1/M2): FZ; results that underflow are flushed too.
 * Exact FTZ needs FPCR.AH = 1, which also changes NaN results and FMIN/FMAX for the thread's
 * host code: a follow-up, switching FPCR at the guest/host boundaries. The MXCSR rounding
 * mode (RC) is not mapped either.
 * GUEST_CPU_FTZ=0 leaves FPCR alone. */
void guest_cpu_apply_host_fp(uint32_t mxcsr) {
#if defined(__aarch64__)
    static int mode = -1; /* 0: off, 1: exact mappings (default), 2: approximate ones too */
    if (mode < 0) {
        const char *env = getenv("GUEST_CPU_FTZ");
        mode = env && env[0] == '0' ? 0 : env && env[0] == '1' ? 2 : 1;
    }
    if (!mode) return;
    enum { FIZ = 1u << 0, FZ = 1u << 24 };
    const int ftz = (mxcsr & 0x8000) != 0, daz = (mxcsr & 0x40) != 0;
    uint64_t fpcr, want;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    want = fpcr & ~(uint64_t)(FZ | (host_has_afp() ? FIZ : 0));
    if ((ftz && daz) || (mode == 2 && (ftz || (daz && !host_has_afp())))) want |= FZ;
    else if (daz && host_has_afp()) want |= FIZ;
    if (want != fpcr) __asm__ volatile("msr fpcr, %0" : : "r"(want));
#elif defined(__x86_64__)
    /* The same bits exist here: the thread flushes exactly as the guest asked. */
    enum { DAZ = 1u << 6, FTZ = 1u << 15 };
    uint32_t host;
    __asm__ volatile("stmxcsr %0" : "=m"(host));
    const uint32_t want = (host & ~(uint32_t)(DAZ | FTZ)) | (mxcsr & (DAZ | FTZ));
    if (want != host) __asm__ volatile("ldmxcsr %0" : : "m"(want));
#else
    (void)mxcsr;
#endif
}

/* MXCSR and FCW given to this thread before its GuestCpu exists (guest_cpu_set_fp_control). */
static _Thread_local struct { int set; uint32_t mxcsr; uint16_t fcw; } tls_fp;
void guest_cpu_get_fp_control(uint32_t *mxcsr, uint16_t *fcw) {
    if (tls_cpu) { *mxcsr = tls_cpu->mxcsr; *fcw = tls_cpu->fcw; }
    else if (tls_fp.set) { *mxcsr = tls_fp.mxcsr; *fcw = tls_fp.fcw; }
    else { *mxcsr = 0x1f80; *fcw = 0x37f; }
}
void guest_cpu_set_fp_control(uint32_t mxcsr, uint16_t fcw) {
    tls_fp.set = 1; tls_fp.mxcsr = mxcsr; tls_fp.fcw = fcw;
    if (tls_cpu) { tls_cpu->mxcsr = mxcsr; tls_cpu->fcw = fcw; }
    guest_cpu_apply_host_fp(mxcsr);
}

static GuestWatch *watch_threads;
static _Thread_local char thread_label[32];
static uint64_t watch_target;
void guest_cpu_set_thread_label(const char *name) {
    snprintf(thread_label, sizeof(thread_label), "%s", name ? name : "unnamed");
    if (tls_cpu && tls_cpu->watch)
        memcpy(tls_cpu->watch->name, thread_label, sizeof(thread_label));
}

/* Per-thread storage is retained with the CPU for terminating diagnostics. */
static void watch_init(GuestCpu *cpu) {
    const char *env = getenv("GUEST_JIT_WATCH");
    if (!env || !*env) return;
    GuestWatch *watch = calloc(1, sizeof(*watch));
    if (!watch) abort();
    snprintf(watch->name, sizeof(watch->name), "%s", *thread_label ? thread_label : "unnamed");
    GuestWatch *head = __atomic_load_n(&watch_threads, __ATOMIC_ACQUIRE);
    do { watch->next = head; }
    while (!__atomic_compare_exchange_n(&watch_threads, &head, watch, 0,
                                        __ATOMIC_RELEASE, __ATOMIC_ACQUIRE));
    cpu->watch = watch;
    __atomic_store_n(&watch_target, strtoull(env, NULL, 0), __ATOMIC_RELAXED);
}

void guest_cpu_watch_write(uintptr_t address, size_t size) {
#if defined(__aarch64__) && defined(__APPLE__)
    if (!tls_cpu || !tls_cpu->watch) return;
    const uint64_t target = __atomic_load_n(&watch_target, __ATOMIC_RELAXED);
    if (target < address || target - address > size || size - (target - address) < 8) return;
    GuestWatch *w = tls_cpu->watch;
    const uint64_t n = w->count;
    GuestWatchRecord *r = &w->records[n % WATCH_RECORDS];
    __atomic_store_n(&r->sequence, 0, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    r->address = target;
    r->value = __atomic_load_n((const uint64_t *)(uintptr_t)target, __ATOMIC_RELAXED);
    r->rip = guest_load(tls_cpu->r[RSP], 8); /* guest return address of the host import */
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(r->tick));
    __atomic_store_n(&r->sequence, (n + 1) | (UINT64_C(1) << 63), __ATOMIC_RELEASE);
    __atomic_store_n(&w->count, n + 1, __ATOMIC_RELEASE);
#else
    (void)address; (void)size;
#endif
}

void guest_cpu_watch_dump(void) {
    for (const GuestWatch *w = __atomic_load_n(&watch_threads, __ATOMIC_ACQUIRE); w; w = w->next) {
        const uint64_t count = __atomic_load_n(&w->count, __ATOMIC_ACQUIRE);
        for (uint64_t n = count > WATCH_RECORDS ? count - WATCH_RECORDS : 0; n < count; ++n) {
            const GuestWatchRecord *r = &w->records[n % WATCH_RECORDS];
            const uint64_t seq = __atomic_load_n(&r->sequence, __ATOMIC_ACQUIRE);
            if ((seq & ~(UINT64_C(1) << 63)) != n + 1) continue;
            const uint64_t tick = __atomic_load_n(&r->tick, __ATOMIC_RELAXED);
            const uint64_t rip = __atomic_load_n(&r->rip, __ATOMIC_RELAXED);
            const uint64_t value = __atomic_load_n(&r->value, __ATOMIC_RELAXED);
            const uint64_t address = __atomic_load_n(&r->address, __ATOMIC_RELAXED);
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if (__atomic_load_n(&r->sequence, __ATOMIC_ACQUIRE) != seq) continue;
            char line[224];
            const int len = snprintf(line, sizeof(line),
                "  JIT watch thread %s tick %llu %s rip %#llx address %#llx value %#llx seq %llu\n",
                w->name, (unsigned long long)tick, seq >> 63 ? "store" : "load",
                (unsigned long long)rip, (unsigned long long)address, (unsigned long long)value, (unsigned long long)(n + 1));
            if (len > 0) { ssize_t written = write(2, line, (size_t)len); (void)written; }
        }
    }
}

static GuestCpu *current_cpu(void) {
    if (tls_cpu) return tls_cpu;
    GuestCpu *cpu = calloc(1, sizeof(*cpu));
    if (!cpu) abort();
    cpu->mxcsr = tls_fp.set ? tls_fp.mxcsr : 0x1f80;
    guest_cpu_apply_host_fp(cpu->mxcsr);
    cpu->fcw = tls_fp.set ? tls_fp.fcw : 0x37f;
    cpu->flags = 2;
    apply_thread_pointer(cpu);
    __atomic_add_fetch(&cpus_created, 1, __ATOMIC_RELAXED);
    watch_init(cpu);
    tls_cpu = cpu;
    return cpu;
}

GuestContext *guest_cpu_context_create(void) {
    GuestContext *context = calloc(1, sizeof(*context));
    if (!context) abort();
    return context;
}

void guest_cpu_context_switch(GuestContext *save, GuestContext *load) {
    save->cpu = save->own = tls_cpu;
    while (save->own && save->own->outer) save->own = save->own->outer;
    tls_cpu = load->cpu;
    load->cpu = load->own = NULL;
    guest_cpu_recomp_switch_frames(save, load);
    /* A context can resume on another host thread than the one it left. */
    if (tls_cpu) {
        apply_thread_pointer(tls_cpu);
        guest_cpu_apply_host_fp(tls_cpu->mxcsr);
    }
}

void guest_cpu_context_destroy(GuestContext *context) {
    if (!context) return;
    free(context->own);
    free(context->frames);
    free(context->resumes);
    free(context);
}

uint64_t guest_cpu_guest_rip(void) { return tls_cpu ? tls_cpu->rip : 0; }

uint64_t guest_cpu_import_return(void) {
    return tls_cpu && tls_cpu->r[RSP] ? guest_cpu_guest_return(guest_load(tls_cpu->r[RSP], 8)) : 0;
}

/* `stack`: the `stack_count` integer arguments after the sixth, the first at rsp + 8 at the
 * callee's entry. */
static uint64_t run_call(GuestCpu *cpu, uintptr_t fn, int count, const uint64_t *args, const uint64_t *stack,
                         int stack_count, uint64_t rsp) {
    static const int arg_regs[6] = {RDI, RSI, RDX, RCX, R8, R9};
    if (count > 6) {
        fputs("guest-cpu: more than 6 arguments to a guest call\n", stderr);
        abort();
    }
    for (int i = 0; i < count; ++i) cpu->r[arg_regs[i]] = args[i];
    if (__builtin_expect(guest_cpu_recomp_census, 0)) guest_cpu_recomp_note_entry(fn);
    const uint64_t stop = SENTINEL(cpu->depth);
    rsp &= ~UINT64_C(15);
    rsp -= (uint64_t)((stack_count + 1) & ~1) * 8; /* an even count of slots keeps rsp 16-byte aligned */
    for (int i = 0; i < stack_count; ++i) guest_store(rsp + (uint64_t)i * 8, 8, stack[i]);
    rsp -= 8;
    guest_store(rsp, 8, stop); /* the return address: rsp % 16 == 8 at the callee's entry */
    cpu->r[RSP] = rsp;
    cpu->rip = fn;
    guest_cpu_run(cpu, stop);
    return cpu->r[RAX];
}

/* A nested call (a guest -> host call calling back into the guest) runs on `inner`, a copy of the
 * calling state cpu, which stays as it was for the code that goes on with it after; only the
 * counters go on from the call's. The copy's TLS slot (gs_base) is its own tcb. A longjmp out of
 * the call moves the state out (guest_cpu_leave_nested). */
static uint64_t run_nested(GuestCpu *cpu, GuestCpu *inner, uintptr_t fn, int count, const uint64_t *args,
                           const uint64_t *stack, int stack_count) {
    if (cpu->gs_base == (uint64_t)(uintptr_t)&cpu->tcb - tls_displacement)
        inner->gs_base = (uint64_t)(uintptr_t)&inner->tcb - tls_displacement;
    inner->outer = cpu;
    inner->outer_rsp = cpu->r[RSP];
    ++inner->depth;
    tls_cpu = inner;
    const uint64_t result = run_call(inner, fn, count, args, stack, stack_count, cpu->r[RSP] - RED_ZONE - 64);
    tls_cpu = cpu;
    cpu->instructions = inner->instructions;
    cpu->recomp_pending = inner->recomp_pending;
    return result;
}

GuestCpu *guest_cpu_leave_nested(GuestCpu *cpu, uint64_t sp) {
    if (!cpu->outer || sp < cpu->outer_rsp) return cpu;
    do {
        GuestCpu *const out = cpu->outer;
        const GuestCpu kept = *out;
        *out = *cpu;
        out->gs_base = kept.gs_base;
        out->depth = kept.depth;
        out->stack_top = kept.stack_top;
        out->outer = kept.outer;
        out->outer_rsp = kept.outer_rsp;
        cpu = out;
    } while (cpu->outer && sp >= cpu->outer_rsp);
    tls_cpu = cpu;
    return cpu;
}

static uint64_t call_with_stack(uintptr_t fn, int count, const uint64_t *args, const uint64_t *stack,
                                int stack_count) {
    GuestCpu *cpu = current_cpu();
    if (!guest_cpu_is_guest_code(fn)) {
        fprintf(stderr, "guest-cpu: call of %p, not guest code\n", (void *)fn);
        abort();
    }
    if (cpu->depth) {
        GuestCpu inner = *cpu;
        return run_nested(cpu, &inner, fn, count, args, stack, stack_count);
    }
    static _Thread_local uint8_t *thread_stack;
    if (!thread_stack) {
        thread_stack = runtime_low_map(GUEST_STACK, PROT_READ | PROT_WRITE);
        if (!thread_stack) abort();
    }
    cpu->stack_top = (uint64_t)(uintptr_t)(thread_stack + GUEST_STACK);
    ++cpu->depth;
    const uint64_t result =
        run_call(cpu, fn, count, args, stack, stack_count, (uint64_t)(uintptr_t)(thread_stack + GUEST_STACK - 64));
    --cpu->depth;
    return result;
}

uint64_t guest_cpu_call(uintptr_t fn, int count, const uint64_t *args) {
    return call_with_stack(fn, count, args, NULL, 0);
}

static void call_full(uintptr_t fn, const uint64_t gpr[6], const uint8_t xmm[8][16], const uint64_t *stack,
                      int stack_count, uint64_t out_gpr[2], uint8_t out_xmm[2][16]) {
    GuestCpu *cpu = current_cpu();
    /* guest_cpu_call sets the integer arguments; the vector ones go in first (a nested call runs on
     * a state of its own, run_nested). */
    if (cpu->depth) {
        GuestCpu inner = *cpu;
        for (int i = 0; i < 8; ++i) memcpy(inner.v[i].b, xmm[i], 16);
        out_gpr[0] = run_nested(cpu, &inner, fn, 6, gpr, stack, stack_count);
        out_gpr[1] = inner.r[RDX];
        memcpy(out_xmm[0], inner.v[0].b, 16);
        memcpy(out_xmm[1], inner.v[1].b, 16);
        return;
    }
    for (int i = 0; i < 8; ++i) memcpy(cpu->v[i].b, xmm[i], 16);
    out_gpr[0] = call_with_stack(fn, 6, gpr, stack, stack_count);
    out_gpr[1] = cpu->r[RDX];
    memcpy(out_xmm[0], cpu->v[0].b, 16);
    memcpy(out_xmm[1], cpu->v[1].b, 16);
}

void guest_cpu_call_full(uintptr_t fn, const uint64_t gpr[6], const uint8_t xmm[8][16], uint64_t out_gpr[2],
                     uint8_t out_xmm[2][16]) {
    call_full(fn, gpr, xmm, NULL, 0, out_gpr, out_xmm);
}

void guest_cpu_call_full_stack(uintptr_t fn, const uint64_t gpr[6], const uint8_t xmm[8][16], const uint64_t *stack,
                               int stack_count, uint64_t out_gpr[2], uint8_t out_xmm[2][16]) {
    call_full(fn, gpr, xmm, stack, stack_count, out_gpr, out_xmm);
}

uint64_t guest_cpu_call_on_stack(uintptr_t fn, int count, const uint64_t *args, void *stack, size_t size) {
    GuestCpu *cpu = current_cpu();
    ++cpu->depth;
    cpu->stack_top = (uint64_t)(uintptr_t)stack + size;
    const uint64_t result = run_call(cpu, fn, count, args, NULL, 0, (uint64_t)(uintptr_t)stack + size - 64);
    --cpu->depth;
    return result;
}

void guest_cpu_start_on_stack(uintptr_t fn, int count, const uint64_t *args, uint64_t rsp, void *stack, size_t size) {
    static const int arg_regs[6] = {RDI, RSI, RDX, RCX, R8, R9};
    GuestCpu *cpu = current_cpu();
    if (count > 6) {
        fputs("guest-cpu: more than 6 arguments to a guest start\n", stderr);
        abort();
    }
    ++cpu->depth;
    cpu->stack_top = (uint64_t)(uintptr_t)stack + size;
    for (int i = 0; i < count; ++i) cpu->r[arg_regs[i]] = args[i];
    cpu->r[RSP] = rsp;
    cpu->rip = fn;
    guest_cpu_run(cpu, SENTINEL(cpu->depth)); /* no return address leads there */
    fputs("guest-cpu: a function started on its stack returned\n", stderr);
    abort();
}

_Noreturn void guest_cpu_fatal(GuestCpu *cpu, const GuestInsn *in, const char *what) {
    char bytes[64] = {0};
    const uint8_t *code = (const uint8_t *)cpu->rip;
    for (int i = 0; i < (in ? in->length : 8) && i < 15; ++i)
        snprintf(bytes + i * 3, sizeof(bytes) - (size_t)i * 3, "%02x ", code[i]);
    fprintf(stderr, "STOP: guest-cpu: %s: %s at guest %#" PRIx64 " (bytes %s)\n", what,
            in ? ZydisMnemonicGetString((ZydisMnemonic)in->mnemonic) : "?", cpu->rip, bytes);
    fprintf(stderr, "  rax=%#" PRIx64 " rbx=%#" PRIx64 " rcx=%#" PRIx64 " rdx=%#" PRIx64
                    " rsi=%#" PRIx64 " rdi=%#" PRIx64 " rbp=%#" PRIx64 " rsp=%#" PRIx64 "\n",
            cpu->r[RAX], cpu->r[RBX], cpu->r[RCX], cpu->r[RDX], cpu->r[RSI], cpu->r[RDI],
            cpu->r[RBP], cpu->r[RSP]);
    fflush(NULL);
    _exit(30);
}

void guest_cpu_decode_stats(uint64_t *blocks, uint64_t *insns);
void guest_cpu_jit_stats(uint64_t *blocks, uint64_t *fallbacks);
void guest_cpu_report(void) {
    uint64_t blocks, insns, translated, fallbacks;
    guest_cpu_decode_stats(&blocks, &insns);
    guest_cpu_jit_stats(&translated, &fallbacks);
    printf("guest-cpu: %" PRIu64 " threads, %" PRIu64 " blocks / %" PRIu64 " instructions decoded, %"
           PRIu64 " blocks translated, %" PRIu64 " instructions interpreted from translated code\n",
           cpus_created, blocks, insns, translated, fallbacks);

}
