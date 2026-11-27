/* guest-cpu: recompiled game functions (bloodborne-recomp's docs/RECOMPILATION.md). GUEST_RECOMP_LIB names a library of C
 * written by tools/recomp/recompile.c from the player's own eboot (src/recomp/rc.h): its functions
 * replace the translated ones. A call of one reaches the dispatcher (the JIT leaves its first
 * instruction untranslated) and guest_cpu_step runs it. GUEST_RECOMP_OFF=OFFSET,... leaves chosen ones to
 * the translator (to find a wrong one by bisection). */
#include <setjmp.h>
#include <stddef.h>
#include <Zydis/Zydis.h>
#include <string.h>
#include "trace.h"
#include "../recomp/rc.h"
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

/* The recompiled functions by address: open addressing with linear probing, at most half full
 * (every indirect call looks one up; chained buckets of 241,000 functions cost a cache miss per
 * link). Filled by guest_cpu_recomp_load before guest code runs, with room for the game's modules,
 * which guest_cpu_recomp_load_module adds as they load (the function, then its address, published
 * for the threads already running); address 0 is an empty slot. */
typedef struct { uint64_t address; RcFn fn; } Recomp;
static Recomp *table;
static uint64_t table_mask;
static int table_shift;
int guest_cpu_recomp_count;
uint64_t guest_cpu_recomp_calls;

static uint64_t slot_of(uint64_t address) { return (address * 0x9e3779b97f4a7c15ull) >> table_shift; }

RcFn guest_cpu_recomp_at(uint64_t address) {
    if (!table) return NULL;
    for (uint64_t i = slot_of(address);; i = (i + 1) & table_mask) {
        const uint64_t at = __atomic_load_n(&table[i].address, __ATOMIC_ACQUIRE);
        if (at == address) return table[i].fn;
        if (!at) return NULL;
    }
}

/* The game's modules' functions, which the table has room for beyond the executable's. */
enum { MODULE_FUNCTIONS = 32768 };
static size_t table_room;

static void reserve(size_t count) {
    int bits = 10;
    while ((UINT64_C(1) << bits) < 2 * (uint64_t)count) ++bits;
    table = calloc(UINT64_C(1) << bits, sizeof(*table));
    if (!table) abort();
    table_mask = (UINT64_C(1) << bits) - 1;
    table_shift = 64 - bits;
    table_room = count;
}

static void add(uint64_t address, RcFn fn) {
    uint64_t i = slot_of(address);
    while (table[i].address && table[i].address != address) i = (i + 1) & table_mask;
    if (!table[i].address) ++guest_cpu_recomp_count;
    table[i].fn = fn;
    __atomic_store_n(&table[i].address, address, __ATOMIC_RELEASE);
}

/* The executable's recompiled functions in use, by start (guest_cpu_recomp_code_checked,
 * guest_cpu_recomp_drop, guest_cpu_recomp_hook_entry): the version each runs (NULL: a hooked_entry
 * wrapper), what the table holds for it, its direct slot and what the slot holding NULL runs (the
 * unpatched version). `reach`: the furthest end of it and of the ones before it (functions can
 * overlap). `spanned`: the ones whose version runs code outside the function too
 * (RcFunction.spans). */
typedef struct {
    uint64_t start, end, reach;
    const RcFunction *version;
    RcFn fn;
    RcFn *direct;
    int dropped;
    RcFn self;
} Loaded;
static Loaded *loaded;
static size_t loaded_count, *spanned, spanned_count;
static uint64_t loaded_base;
static int loaded_order(const void *a, const void *b) {
    const uint64_t x = ((const Loaded *)a)->start, y = ((const Loaded *)b)->start;
    return x < y ? -1 : x > y;
}
/* How many of them start at or before `address`. */
static size_t loaded_through(uint64_t address) {
    size_t lo = 0, hi = loaded_count;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (loaded[mid].start <= address) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

int guest_cpu_is_tcb_load(const GuestInsn *in) {
    if (in->mnemonic != ZYDIS_MNEMONIC_MOV || in->count != 2) return 0;
    const GuestOp *d = &in->op[0], *s = &in->op[1];
    return d->type == OP_REG && d->kind == RK_GPR && d->size == 8 && s->type == OP_MEM && s->size == 8 &&
           s->base == 0xff && s->index == 0xff && ((s->segment == 1 && s->disp == 0) || s->segment == 2);
}

/* `live`: (offset from live_base, size) pairs, by offset, whose bytes count as zeros. */
static uint64_t hash_more(uint64_t h, uint64_t start, uint64_t size, uint64_t live_base, const uint64_t *live,
                          size_t live_count) {
#define MIX(byte) (h = (h ^ (uint8_t)(byte)) * 0x100000001b3ull)
    size_t next_live = 0;
    for (uint64_t at = start; at < start + size;) {
        GuestBlock *block = guest_cpu_block_uncached(at);
        if (!block->count) { free(block); break; }
        for (uint32_t i = 0; i < block->count && at < start + size; ++i) {
            const GuestInsn *in = &block->insn[i];
            if (guest_cpu_is_tcb_load(in)) {
                MIX(0xf5);
                MIX(in->op[0].reg);
            } else {
                for (int b = 0; b < in->length; ++b) {
                    const uint64_t a = at + (uint64_t)b - live_base;
                    while (next_live < live_count && live[2 * next_live] + live[2 * next_live + 1] <= a) ++next_live;
                    MIX(next_live < live_count && live[2 * next_live] <= a ? 0 : ((const uint8_t *)at)[b]);
                }
            }
            at += in->length;
        }
        free(block);
    }
#undef MIX
    return h;
}

uint64_t guest_cpu_recomp_hash(uint64_t start, uint64_t size) { return hash_more(0xcbf29ce484222325ull, start, size, 0, NULL, 0); }

uint64_t guest_cpu_recomp_hash_parts(uint64_t image_base, uint64_t offset, uint64_t size, const uint64_t *spans,
                                 size_t span_count, const uint64_t *live, size_t live_count) {
    uint64_t h = hash_more(0xcbf29ce484222325ull, image_base + offset, size, image_base, live, live_count);
    for (size_t i = 0; i < span_count; ++i)
        h = hash_more(h, image_base + spans[2 * i], spans[2 * i + 1], image_base, live, live_count);
    return h;
}

/* Functions whose entry the runtime hooked (bloodborne-runtime's guest code hooks): the entry
 * jumps to a stub `int3; the replaced instructions; jmp [rip + 0] back`. Such a function stays
 * recompiled when its code with the replaced instructions put back is the generated one: a
 * wrapper of its own runs the trap handlers, as the int3 would, then the recompiled function (or
 * returns at once when a handler sent the guest back to the caller). Without it a hooked function
 * ran in the translator, as did everything it called that a translated caller reaches. */
#define HOOKED_MAX 64
typedef struct { uint64_t entry, trap; RcFn fn; } Hooked;
static Hooked hooked[HOOKED_MAX];
static int hooked_count;

static void run_hooked(GuestCpu *cpu, int i) {
    const Hooked *h = &hooked[i];
    GuestRegs regs;
    memcpy(regs.r, cpu->r, sizeof(regs.r));
    regs.rip = h->trap + 1;
    if (!guest_cpu_trap(cpu, &regs)) {
        fprintf(stderr, "guest-cpu: a hooked function's trap at %#llx not handled\n", (unsigned long long)h->trap);
        abort();
    }
    memcpy(cpu->r, regs.r, sizeof(regs.r));
    if (regs.rip != h->trap + 1) { /* the handler returned from the function (rsp past the return address) */
        cpu->rip = regs.rip;
        return;
    }
    cpu->rip = h->entry;
    h->fn(cpu);
}
#define HOOKED(a, b) static void hooked_##a##b(GuestCpu *c) { run_hooked(c, (a) * 8 + (b)); }
#define HOOKED8(a) HOOKED(a, 0) HOOKED(a, 1) HOOKED(a, 2) HOOKED(a, 3) HOOKED(a, 4) HOOKED(a, 5) HOOKED(a, 6) HOOKED(a, 7)
HOOKED8(0) HOOKED8(1) HOOKED8(2) HOOKED8(3) HOOKED8(4) HOOKED8(5) HOOKED8(6) HOOKED8(7)
#define HOOKED_NAMES(a) hooked_##a##0, hooked_##a##1, hooked_##a##2, hooked_##a##3, hooked_##a##4, hooked_##a##5, hooked_##a##6, hooked_##a##7
static const RcFn hooked_fns[HOOKED_MAX] = {HOOKED_NAMES(0), HOOKED_NAMES(1), HOOKED_NAMES(2), HOOKED_NAMES(3),
                                            HOOKED_NAMES(4), HOOKED_NAMES(5), HOOKED_NAMES(6), HOOKED_NAMES(7)};

/* The wrapper for a function at `entry` whose code differs from the generated one only by such a
 * hook, or NULL. Every byte of the shape is checked. */
static RcFn hooked_entry(uint64_t entry, uint64_t size, uint64_t hash, RcFn fn) {
    const uint8_t *site = (const uint8_t *)(uintptr_t)entry;
    if (hooked_count == HOOKED_MAX || size < 5 || site[0] != 0xe9) return NULL;
    int32_t rel;
    memcpy(&rel, site + 1, 4);
    const uint64_t trap = entry + 5 + (uint64_t)(int64_t)rel;
    if (!guest_cpu_is_guest_code(trap)) return NULL;
    const uint8_t *stub = (const uint8_t *)(uintptr_t)trap;
    if (stub[0] != 0xcc) return NULL;
    static const uint8_t jmp_rip[] = {0xff, 0x25, 0, 0, 0, 0};
    for (uint64_t n = 5; n <= 32 && n <= size; ++n) { /* n: the replaced instructions' length */
        if (memcmp(stub + 1 + n, jmp_rip, sizeof(jmp_rip)) != 0) continue;
        uint64_t back;
        memcpy(&back, stub + 1 + n + sizeof(jmp_rip), 8);
        if (back != entry + n) continue;
        for (uint64_t k = 5; k < n; ++k)
            if (site[k] != 0x90) return NULL;
        const uint64_t h = hash_more(hash_more(0xcbf29ce484222325ull, trap + 1, n, 0, NULL, 0), entry + n, size - n, 0, NULL, 0);
        if (h != hash) return NULL;
        hooked[hooked_count] = (Hooked){entry, trap, fn};
        return hooked_fns[hooked_count++];
    }
    return NULL;
}

/* GUEST_RECOMP_THREADS / GUEST_RECOMP_NOT_THREADS=NAME,... (diagnostics): recompiled functions only on
 * the threads whose name contains one of the first, and none of the second (host thread names). */
static int name_matches(const char *list, const char *name) {
    if (!list) return 0;
    char copy[512];
    snprintf(copy, sizeof(copy), "%s", list);
    for (char *part = strtok(copy, ","); part; part = strtok(NULL, ","))
        if (*part && strstr(name, part)) return 1;
    return 0;
}
int guest_cpu_recomp_threads_filtered;
int guest_cpu_recomp_thread_named(void) {
    static _Thread_local int ok = -1;
    if (ok < 0) {
        const char *only = getenv("GUEST_RECOMP_THREADS"), *not = getenv("GUEST_RECOMP_NOT_THREADS");
        if (!only && !not) {
            ok = 1;
        } else {
            char name[64] = "";
            pthread_getname_np(pthread_self(), name, sizeof(name));
            ok = (!only || name_matches(only, name)) && !name_matches(not, name);
        }
    }
    return ok;
}

/* ---- the runtime the generated code calls (RcApi) ---- */

/* Calls the runtime makes in the translator get a return address of their own, never guest code
 * (FRAME_BASE + 16 * depth): guest_cpu_run stops there. A longjmp (or an exception) in the guest can
 * leave several such frames at once: the guest then returns to an outer frame's address, and
 * return_hook goes back to that frame on the host too (the C frames between are left: recompiled
 * code holds nothing). One address for all of them returned into the innermost frame instead. */
#define FRAME_BASE UINT64_C(0x0000700100000000)
/* `back`: the guest return address the frame's own one replaced on the guest stack, for code that
 * reads the caller from there (guest_cpu_guest_return). `resumes`: the depth of the calls of setjmp
 * (below) as it started, which a longjmp to the frame leaves behind. */
typedef struct { jmp_buf jump; uint64_t back; uint32_t resumes; } Frame; /* _setjmp: no signal mask */
static _Thread_local Frame *frames;
static _Thread_local uint32_t frame_depth, frame_capacity;
/* The calls of setjmp in compiled code whose function has not returned (RcResume), innermost last:
 * a guest longjmp to one goes on in its C frame (landed). */
static _Thread_local RcResume **resumes;
static _Thread_local uint32_t resume_depth, resume_capacity;

/* Statistics for the report: calls into the translator, longjmps to an outer frame, deepest. */
static uint64_t translated_runs, frame_jumps, deepest_frame, recomp_image_base, host_calls, resume_jumps, landings_left;
/* Adds to a shared counter in batches of 4096 per thread, as guest_cpu_recomp_counted does: every
 * thread adding one at a time to the same line costs as much as a short call. */
static void count_batched(uint64_t *total, uint32_t *pending) {
    if (++*pending == 4096) {
        __atomic_add_fetch(total, *pending, __ATOMIC_RELAXED);
        *pending = 0;
    }
}
static _Thread_local uint32_t pending_runs, pending_host_calls, pending_resume_jumps;
/* GUEST_RECOMP_TARGETS=1: the targets of calls into the translator (and with the guest return
 * address the call left, `back`), the host functions the import stubs reach and the hook points
 * that run, counted (sampled 1 in 64). */
enum { TARGET_SLOTS = 4096 };
typedef struct { uint64_t rip, count; } Target;
static Target targets[TARGET_SLOTS], host_targets[TARGET_SLOTS], hook_sites[TARGET_SLOTS];
typedef struct { uint64_t rip, back, count; } TargetFrom;
static TargetFrom targets_from[TARGET_SLOTS];
static int count_targets(void) {
    static int value = -1;
    if (value < 0) value = getenv("GUEST_RECOMP_TARGETS") && getenv("GUEST_RECOMP_TARGETS")[0] == '1';
    return value;
}
/* Whether a sample of 1 in 2^shift leaves an event out: a Weyl sequence of the thread's count picks
 * the ones kept. Every 2^shift-th one keeps only some of a run that repeats with a period sharing a
 * factor with 2^shift (a call thunk's six instructions came out as three, each counted twice). */
static int skipped(uint32_t *tick, unsigned shift) {
    return (uint32_t)(++*tick * 0x9e3779b9u) >> (32 - shift) != 0;
}
static void note_target_in(Target *table, uint64_t rip) {
    for (uint32_t i = 0, h = (uint32_t)((rip * 0x9e3779b97f4a7c15ull) >> 52); i < 16; ++i) {
        uint64_t *slot = &table[(h + i) % TARGET_SLOTS].rip, expected = 0;
        if (__atomic_load_n(slot, __ATOMIC_RELAXED) == rip ||
            __atomic_compare_exchange_n(slot, &expected, rip, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            __atomic_add_fetch(&table[(h + i) % TARGET_SLOTS].count, 1, __ATOMIC_RELAXED);
            return;
        }
    }
}
static void note_target(uint64_t rip, uint64_t back) {
    static _Thread_local uint32_t tick;
    if (skipped(&tick, 6) || !count_targets()) return;
    note_target_in(targets, rip);
    for (uint32_t i = 0, h = (uint32_t)(((rip ^ back * 31) * 0x9e3779b97f4a7c15ull) >> 52); i < 16; ++i) {
        TargetFrom *t = &targets_from[(h + i) % TARGET_SLOTS];
        uint64_t expected = 0;
        if ((__atomic_load_n(&t->rip, __ATOMIC_ACQUIRE) == rip && __atomic_load_n(&t->back, __ATOMIC_RELAXED) == back) ||
            (__atomic_compare_exchange_n(&t->rip, &expected, rip, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED) &&
             (__atomic_store_n(&t->back, back, __ATOMIC_RELEASE), 1))) {
            __atomic_add_fetch(&t->count, 1, __ATOMIC_RELAXED);
            return;
        }
    }
}
static void note_hook_site(uint64_t site) {
    static _Thread_local uint32_t tick;
    if (skipped(&tick, 6) || !count_targets()) return;
    note_target_in(hook_sites, site);
}
static void note_host_target(uint64_t host) {
    static _Thread_local uint32_t tick;
    if (skipped(&tick, 6) || !count_targets()) return;
    note_target_in(host_targets, host);
}
/* The instructions of hook sites and hook code that a hook point runs in the interpreter. */
static Target hook_step_sites[TARGET_SLOTS];
static void note_hook_step(uint64_t rip) {
    static _Thread_local uint32_t tick;
    if (skipped(&tick, 6) || !count_targets()) return;
    note_target_in(hook_step_sites, rip);
}
/* Every instruction the interpreter runs and every host call of guest code (trace.h), counted
 * one by one and sampled 1 in 16 by address. */
int guest_cpu_recomp_census;
static uint64_t interpreted, entries;
static Target interpreted_at[TARGET_SLOTS], entries_at[TARGET_SLOTS];
void guest_cpu_recomp_note_interpreted(uint64_t rip) {
    static _Thread_local uint32_t tick;
    __atomic_add_fetch(&interpreted, 1, __ATOMIC_RELAXED);
    if (!skipped(&tick, 4)) note_target_in(interpreted_at, rip);
}
void guest_cpu_recomp_note_entry(uint64_t fn) {
    static _Thread_local uint32_t tick;
    __atomic_add_fetch(&entries, 1, __ATOMIC_RELAXED);
    if (!skipped(&tick, 4)) note_target_in(entries_at, fn);
}
/* The ten busiest entries of a table, counts taken (each sample stands for 64). */
static void take_best(Target *table, uint64_t best[10][2]) {
    memset(best, 0, 10 * 2 * sizeof(uint64_t));
    for (int i = 0; i < TARGET_SLOTS; ++i) {
        const uint64_t c = __atomic_exchange_n(&table[i].count, 0, __ATOMIC_RELAXED);
        for (int k = 0; k < 10; ++k)
            if (c > best[k][1]) { memmove(best[k + 1], best[k], (size_t)(9 - k) * sizeof(best[0])); best[k][0] = table[i].rip; best[k][1] = c; break; }
    }
}

void guest_cpu_recomp_switch_frames(struct GuestContext *save, struct GuestContext *load) {
    save->frames = frames;
    save->frame_depth = frame_depth;
    save->frame_capacity = frame_capacity;
    save->resumes = resumes;
    save->resume_depth = resume_depth;
    save->resume_capacity = resume_capacity;
    frames = load->frames;
    frame_depth = load->frame_depth;
    frame_capacity = load->frame_capacity;
    resumes = load->resumes;
    resume_depth = load->resume_depth;
    resume_capacity = load->resume_capacity;
    load->frames = load->resumes = NULL;
    load->frame_depth = load->frame_capacity = load->resume_depth = load->resume_capacity = 0;
}

uint64_t guest_cpu_guest_return(uint64_t value) {
    if (value < FRAME_BASE || value >= FRAME_BASE + (uint64_t)frame_depth * 16 || (value & 15)) return value;
    return frames[(value - FRAME_BASE) / 16].back;
}

static void return_hook(GuestCpu *cpu) {
    const uint64_t rip = cpu->rip;
    if (rip < FRAME_BASE || rip >= FRAME_BASE + (uint64_t)frame_depth * 16 || (rip & 15)) return;
    __atomic_add_fetch(&frame_jumps, 1, __ATOMIC_RELAXED);
    guest_cpu_leave_nested(cpu, cpu->r[RSP]);
    _longjmp(frames[(rip - FRAME_BASE) / 16].jump, 1);
}

/* The translator from cpu->rip until the guest returns through `slot` (the return address the
 * caller left there, replaced by this frame's). */
static void run_translated(GuestCpu *cpu, uint64_t slot) {
    if (frame_depth == frame_capacity) {
        frame_capacity = frame_capacity ? frame_capacity * 2 : 64;
        frames = realloc(frames, frame_capacity * sizeof(*frames));
        if (!frames) abort();
    }
    const volatile uint32_t depth = frame_depth++;
    count_batched(&translated_runs, &pending_runs);
    note_target(cpu->rip, guest_load(slot, 8));
    if (depth + 1 > __atomic_load_n(&deepest_frame, __ATOMIC_RELAXED)) __atomic_store_n(&deepest_frame, depth + 1, __ATOMIC_RELAXED);
    const uint64_t stop = FRAME_BASE + (uint64_t)depth * 16;
    frames[depth].back = guest_load(slot, 8);
    frames[depth].resumes = resume_depth;
    guest_store(slot, 8, stop);
    if (_setjmp(frames[depth].jump) == 0) guest_cpu_run(cpu, stop);
    frame_depth = depth;
    resume_depth = frames[depth].resumes;
}

/* RcApi.resume: the call of setjmp at r goes on the stack of live ones (again, when it runs again),
 * past the ones of frames the guest stack has left. */
static void rc_resume(GuestCpu *cpu, RcResume *r, uint64_t next) {
    const uint64_t sp = cpu->r[RSP];
    while (resume_depth && resumes[resume_depth - 1] != r && resumes[resume_depth - 1]->rsp < sp) --resume_depth;
    if (r->at >= resume_depth || resumes[r->at] != r) {
        if (resume_depth == resume_capacity) {
            resume_capacity = resume_capacity ? resume_capacity * 2 : 64;
            resumes = realloc(resumes, resume_capacity * sizeof(*resumes));
            if (!resumes) abort();
        }
        r->at = resume_depth;
        resumes[resume_depth++] = r;
    }
    r->rip = next;
    r->rsp = sp;
    r->frames = frame_depth;
}

/* RcApi.leave: a function with calls of setjmp returns; they and any after them go. */
static void rc_leave(RcResume *r, int count) {
    for (int k = 0; k < count; ++k)
        if (r[k].at < resume_depth && resumes[r[k].at] == &r[k]) resume_depth = r[k].at;
}

/* The guest went on at cpu->rip with RSP where a call of setjmp of compiled code left it (a longjmp
 * to it): its C frame goes on there. Returns when there is none. */
static void resume_landing(GuestCpu *cpu) {
    const uint64_t sp = cpu->r[RSP], rip = cpu->rip;
    while (resume_depth && resumes[resume_depth - 1]->rsp < sp) --resume_depth;
    for (uint32_t i = resume_depth; i-- > 0 && resumes[i]->rsp == sp;) {
        RcResume *r = resumes[i];
        if (r->rip != rip) continue;
        count_batched(&resume_jumps, &pending_resume_jumps);
        frame_depth = r->frames;
        guest_cpu_leave_nested(cpu, sp);
        _longjmp(r->jump, 1);
    }
}
void guest_cpu_recomp_landed(GuestCpu *cpu) { resume_landing(cpu); }

/* A guest function the runtime called came back to another address than its return address (a
 * longjmp): to a call of setjmp of compiled code, which goes on; elsewhere the translator goes on
 * from there, in a frame no return address leads to, until the guest returns into an outer frame
 * (return_hook), the one of the translator that ran the setjmp. */
static __attribute__((noinline)) void landed(GuestCpu *cpu) {
    resume_landing(cpu);
    __atomic_add_fetch(&landings_left, 1, __ATOMIC_RELAXED);
    if (frame_depth == frame_capacity) {
        frame_capacity = frame_capacity ? frame_capacity * 2 : 64;
        frames = realloc(frames, frame_capacity * sizeof(*frames));
        if (!frames) abort();
    }
    const volatile uint32_t depth = frame_depth++;
    frames[depth].back = 0;
    frames[depth].resumes = resume_depth;
    if (_setjmp(frames[depth].jump) == 0) guest_cpu_run(cpu, FRAME_BASE + (uint64_t)depth * 16);
    guest_cpu_fatal(cpu, NULL, "a longjmp's code returned into a frame no return address leads to");
}

/* GUEST_RECOMP_TARGETS=1 also counts the instructions generated code leaves to the interpreter
 * (rc->step), by mnemonic. */
enum { STEP_MNEMONICS = 2048 };
static uint64_t step_counts[STEP_MNEMONICS];
static uint64_t rc_step(GuestCpu *cpu) {
    const GuestBlock *block = guest_cpu_block(cpu->rip);
    if (count_targets() && block->count && (unsigned)block->insn[0].mnemonic < STEP_MNEMONICS)
        __atomic_add_fetch(&step_counts[block->insn[0].mnemonic], 1, __ATOMIC_RELAXED);
    return guest_cpu_step_insn(cpu, &block->insn[0]);
}

/* GUEST_RECOMP_NODIRECT=1 (diagnostics): calls and tail calls between recompiled functions go
 * through the translator's dispatcher instead of being made directly. */
static int no_direct(void) {
    static int value = -1;
    if (value < 0) {
        const char *env = getenv("GUEST_RECOMP_NODIRECT");
        value = env && env[0] == '1';
    }
    return value;
}

/* An import stub (jmp [rip+slot]) whose slot holds a host function (the runtime's libraries) or a
 * recompiled function of a module: that function at once, with the return address on top of the
 * guest stack, without the translator (a _setjmp and its dispatch loop). 1: done, cpu->rip is
 * where it returned to. */
static int call_import(GuestCpu *cpu, uint64_t target) {
    if (!guest_cpu_is_guest_code(target)) return 0;
    const uint8_t *code = (const uint8_t *)(uintptr_t)target;
    if (code[0] != 0xff || code[1] != 0x25) return 0;
    int32_t disp;
    memcpy(&disp, code + 2, 4);
    const uint64_t host = guest_load(target + 6 + (uint64_t)(int64_t)disp, 8);
    if (!host) return 0;
    if (guest_cpu_is_guest_code(host)) {
        const RcFn fn = guest_cpu_recomp_at(host);
        if (!fn) return 0;
        guest_cpu_recomp_counted(cpu);
        cpu->rip = host;
        fn(cpu);
        return 1;
    }
    count_batched(&host_calls, &pending_host_calls);
    note_host_target(host);
    cpu->rip = host;
    guest_cpu_call_host_at_rip(cpu); /* pops the return address into rip */
    return 1;
}

int guest_cpu_recomp_call_import(GuestCpu *cpu) {
    return call_import(cpu, cpu->rip);
}

/* A native version of a game function (native.c), the same way: no translator in between (the
 * game's mutex on iOS, about 130,000 calls a second in play). 1: done, as for call_import. */
static int call_native(GuestCpu *cpu, uint64_t target) {
    if (!guest_cpu_native_count || guest_cpu_record_armed) return 0;
    const void *fn = guest_cpu_native_at(target);
    if (!fn) return 0;
    __atomic_add_fetch(&guest_cpu_native_calls, 1, __ATOMIC_RELAXED);
    cpu->rip = (uint64_t)(uintptr_t)fn;
    guest_cpu_call_host_at_rip(cpu);
    return 1;
}

/* A host function (the program's hook code calls its own: mov rax, imm64; call rax) at once, as
 * the interpreter calls one, with the return address on top of the guest stack. 1: done, as for
 * call_import. */
static int call_host_function(GuestCpu *cpu, uint64_t target) {
    if (guest_cpu_is_guest_code(target)) return 0;
    count_batched(&host_calls, &pending_host_calls);
    note_host_target(target);
    cpu->rip = target;
    guest_cpu_call_host_at_rip(cpu); /* pops the return address into rip */
    return 1;
}

/* The rest of rc_call and rc_tail, out of line: their recompiled path then needs no stack frame. */
static __attribute__((noinline)) void call_other(GuestCpu *cpu, uint64_t target, uint64_t next) {
    if (call_host_function(cpu, target) || call_import(cpu, target) || call_native(cpu, target)) {
        if (cpu->rip != next) landed(cpu); /* a longjmp */
        return;
    }
    /* The translator (and natives, imports: guest_cpu_run calls them). */
    cpu->rip = target;
    run_translated(cpu, cpu->r[RSP]);
    cpu->rip = next;
}
static __attribute__((noinline)) void tail_other(GuestCpu *cpu) {
    const uint64_t back = guest_load(cpu->r[RSP], 8);
    if (call_host_function(cpu, cpu->rip) || call_import(cpu, cpu->rip) || call_native(cpu, cpu->rip)) { /* it returns for us */
        if (cpu->rip != back) landed(cpu);
        return;
    }
    run_translated(cpu, cpu->r[RSP]);
    cpu->rip = back;
}

static void rc_call(GuestCpu *cpu, uint64_t target, uint64_t next) {
    const RcFn fn = guest_cpu_recomp_at(target);
    cpu->r[RSP] -= 8;
    guest_store(cpu->r[RSP], 8, next);
    if (!fn) {
        call_other(cpu, target, next);
        return;
    }
    guest_cpu_recomp_counted(cpu);
    cpu->rip = target;
    fn(cpu);
}

static void rc_tail(GuestCpu *cpu) {
    const RcFn fn = guest_cpu_recomp_at(cpu->rip);
    if (!fn) {
        tail_other(cpu);
        return;
    }
    guest_cpu_recomp_counted(cpu);
    fn(cpu);
}

/* rc_call and rc_tail with GUEST_RECOMP_NODIRECT or a thread filter: guest_cpu_recomp_load gives these
 * to the library only then, so the calls of a normal run (11.5 million a second at the Sickroom
 * lamp) check neither. */
static int recomp_here(void) {
    return !no_direct() && guest_cpu_recomp_thread_ok();
}
static void rc_call_checked(GuestCpu *cpu, uint64_t target, uint64_t next) {
    if (recomp_here()) {
        rc_call(cpu, target, next);
        return;
    }
    cpu->r[RSP] -= 8;
    guest_store(cpu->r[RSP], 8, next);
    call_other(cpu, target, next);
}
static void rc_tail_checked(GuestCpu *cpu) {
    if (recomp_here()) rc_tail(cpu);
    else tail_other(cpu);
}

/* A call (`next` its return address) or a tail call (`next` 0) of an import stub whose slot the
 * generated code names: the host function the slot held the last time this call ran (*host) at
 * once, without rc_call's table, the stub's decoding or the guest code ranges; a module's
 * recompiled function the same way; what rc_call and rc_tail do with the stub otherwise. */
static void rc_import(GuestCpu *cpu, uint64_t *host, uint64_t stub, uint64_t slot, uint64_t next) {
    const uint64_t target = guest_load(slot, 8);
    RcFn fn = NULL;
    if (target != __atomic_load_n(host, __ATOMIC_RELAXED)) {
        fn = guest_cpu_recomp_at(target);
        if (!fn && (!target || guest_cpu_is_guest_code(target))) {
            if (next) rc_call(cpu, stub, next);
            else {
                cpu->rip = stub;
                rc_tail(cpu);
            }
            return;
        }
        if (!fn) __atomic_store_n(host, target, __ATOMIC_RELAXED);
    }
    const uint64_t back = next ? next : guest_load(cpu->r[RSP], 8);
    if (next) {
        cpu->r[RSP] -= 8;
        guest_store(cpu->r[RSP], 8, next);
    }
    cpu->rip = target;
    if (fn) {
        guest_cpu_recomp_counted(cpu);
        fn(cpu);
    } else {
        count_batched(&host_calls, &pending_host_calls);
        note_host_target(target);
        guest_cpu_call_host_at_rip(cpu); /* pops the return address into rip */
    }
    if (cpu->rip != back) landed(cpu); /* a longjmp */
}
static void rc_import_checked(GuestCpu *cpu, uint64_t *host, uint64_t stub, uint64_t slot, uint64_t next) {
    (void)host;
    (void)slot;
    if (next) rc_call_checked(cpu, stub, next);
    else {
        cpu->rip = stub;
        rc_tail_checked(cpu);
    }
}

static void rc_bail(GuestCpu *cpu, uint64_t entry_rsp) {
    const uint64_t back = guest_load(entry_rsp, 8);
    run_translated(cpu, entry_rsp);
    cpu->rip = back;
}

/* The instructions that stop the game, as the interpreter stops it (guest_cpu_fatal). */
__attribute__((noreturn)) static void rc_fault(GuestCpu *cpu, uint64_t rip, int kind) {
    static const char *const what[] = {"undefined instruction", "unsupported instruction", "privileged instruction",
                                       "division by zero or overflow", "instruction without compiled code"};
    cpu->rip = rip;
    const GuestBlock *block = guest_cpu_block(rip);
    guest_cpu_fatal(cpu, block->count ? &block->insn[0] : NULL,
                kind >= 0 && kind <= RC_FAULT_NOT_COMPILED ? what[kind] : "fault");
}

/* int3, as the interpreter runs it: the trap handlers, the registers as after the int3. */
static uint64_t rc_trap(GuestCpu *cpu, uint64_t rip) {
    GuestRegs regs;
    memcpy(regs.r, cpu->r, sizeof(regs.r));
    regs.rip = rip;
    if (!guest_cpu_trap(cpu, &regs)) {
        cpu->rip = rip - 1;
        const GuestBlock *block = guest_cpu_block(rip - 1);
        guest_cpu_fatal(cpu, block->count ? &block->insn[0] : NULL, "unhandled int3");
    }
    memcpy(cpu->r, regs.r, sizeof(regs.r));
    return regs.rip;
}

static void rc_set_mxcsr(GuestCpu *cpu, uint32_t value) {
    cpu->mxcsr = value;
    guest_cpu_apply_host_fp(value);
}

/* ---- hooks of the host program (guest_cpu.h) ---- */

/* The code of the program's hooks (stubs it writes), which hook points run. */
enum { HOOK_CODE_MAX = 8 };
static struct { uint64_t start, end; } hook_code[HOOK_CODE_MAX];
static int hook_code_count;
void guest_cpu_recomp_hook_code(uint64_t start, size_t size) {
    if (hook_code_count == HOOK_CODE_MAX) {
        fputs("Recompiled: too many ranges of hook code\n", stderr);
        abort();
    }
    hook_code[hook_code_count].start = start;
    hook_code[hook_code_count].end = start + size;
    ++hook_code_count;
}
static int in_hook_code(uint64_t address) {
    for (int i = 0; i < hook_code_count; ++i)
        if (address >= hook_code[i].start && address < hook_code[i].end) return 1;
    return 0;
}

/* The hook code's recompiled C (recompile --hook-code, from the code a run with
 * GUEST_RECOMP_HOOK_DUMP wrote): each function of its library is the code from one address of the
 * hook code until that code leaves the function (cpu->rip where it went on), written for any
 * address. What differs from run to run and with the place of the code (RcFunction.live: host
 * addresses, distances to the game's code, the data between the instructions) is read as it
 * runs, and a function is the code wherever the hook code holds its other bytes
 * (guest_cpu_recomp_hook_hash). hook_fns: each address of the hook code looked up so far, with the
 * function that is the code there or hook_none; a slot for every two bytes of hook code or more. */
static const RcFunction *hook_functions;
int guest_cpu_recomp_hook_count;
static struct { uint64_t address; RcFn fn; } *hook_fns;
static uint64_t hook_fn_mask;
static int hook_fn_shift;
static pthread_mutex_t hook_fn_lock = PTHREAD_MUTEX_INITIALIZER;
static void hook_none(GuestCpu *cpu) { (void)cpu; }

uint64_t guest_cpu_recomp_hook_hash(uint64_t start, uint64_t offset, uint64_t size, const uint64_t *live,
                                    size_t live_count) {
    uint64_t h = 0xcbf29ce484222325ull;
    size_t next_live = 0;
    for (uint64_t b = 0; b < size; ++b) {
        const uint64_t a = offset + b;
        while (next_live < live_count && live[2 * next_live] + live[2 * next_live + 1] <= a) ++next_live;
        const uint8_t byte = next_live < live_count && live[2 * next_live] <= a ? 0 : ((const uint8_t *)(uintptr_t)start)[b];
        h = (h ^ byte) * 0x100000001b3ull;
    }
    return h;
}

/* The function whose bytes the hook code holds at `address`, or hook_none. */
static RcFn hook_match(uint64_t address) {
    uint64_t end = 0;
    for (int i = 0; i < hook_code_count; ++i)
        if (address >= hook_code[i].start && address < hook_code[i].end) end = hook_code[i].end;
    for (int i = 0; i < guest_cpu_recomp_hook_count; ++i) {
        const RcFunction *f = &hook_functions[i];
        if (address + f->size <= end &&
            guest_cpu_recomp_hook_hash(address, f->offset, f->size, (const uint64_t *)f->live, f->live_count) == f->hash)
            return f->fn;
    }
    return hook_none;
}

RcFn guest_cpu_recomp_hook_at(uint64_t address) {
    if (!in_hook_code(address)) return NULL;
    for (uint64_t i = (address * 0x9e3779b97f4a7c15ull) >> hook_fn_shift;; i = (i + 1) & hook_fn_mask) {
        const uint64_t at = __atomic_load_n(&hook_fns[i].address, __ATOMIC_ACQUIRE);
        if (at == address) return hook_fns[i].fn == hook_none ? NULL : hook_fns[i].fn;
        if (!at) break;
    }
    pthread_mutex_lock(&hook_fn_lock);
    const RcFn fn = hook_match(address);
    uint64_t i = (address * 0x9e3779b97f4a7c15ull) >> hook_fn_shift;
    while (hook_fns[i].address && hook_fns[i].address != address) i = (i + 1) & hook_fn_mask;
    if (!hook_fns[i].address) { /* the function, then its address, as add publishes them */
        hook_fns[i].fn = fn;
        __atomic_store_n(&hook_fns[i].address, address, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&hook_fn_lock);
    return fn == hook_none ? NULL : fn;
}

static uint64_t hook_runs, hook_steps;
static _Thread_local uint32_t pending_hook_runs, pending_hook_steps;

/* RcApi.hook: the bytes at [cpu->rip, next) are not the compiled ones. They and the hook code
 * they lead to run in the interpreter until the guest goes on elsewhere: past them (where the
 * compiled code goes on), back to the function's caller, anywhere else (its dispatch). Calls of
 * guest code run as rc_call runs them, to their return: a call of a function the stub's code jumps
 * into would otherwise end the run inside the call. */
static void rc_hook(GuestCpu *cpu, uint64_t next) {
    const uint64_t site = cpu->rip;
    count_batched(&hook_runs, &pending_hook_runs);
    note_hook_site(site);
    /* A hook's jump to its stub (jmp rel32), the bytes at most sites. */
    if (next - site >= 5 && *(const uint8_t *)(uintptr_t)site == 0xe9) {
        int32_t rel;
        memcpy(&rel, (const void *)(uintptr_t)(site + 1), 4);
        cpu->rip = site + 5 + (uint64_t)(int64_t)rel;
    }
    while ((cpu->rip >= site && cpu->rip < next) || in_hook_code(cpu->rip)) {
        /* The hook code's recompiled C from here on, until the code leaves its function. */
        const RcFn fn = guest_cpu_recomp_hook_count ? guest_cpu_recomp_hook_at(cpu->rip) : NULL;
        if (fn) {
            fn(cpu);
            continue;
        }
        count_batched(&hook_steps, &pending_hook_steps);
        note_hook_step(cpu->rip);
        const GuestBlock *block = guest_cpu_block(cpu->rip);
        if (!block->count) guest_cpu_fatal(cpu, NULL, "undecodable code at a hook point");
        const GuestInsn *in = &block->insn[0];
        if (in->mnemonic == ZYDIS_MNEMONIC_CALL) {
            const GuestOp *op = &in->op[0];
            const uint64_t target = op->type == OP_IMM ? cpu->rip + (uint64_t)op->disp : guest_cpu_read(cpu, in, op);
            if (guest_cpu_is_guest_code(target)) {
                rc_call(cpu, target, cpu->rip + in->length);
                continue;
            }
        }
        cpu->rip = guest_cpu_step_insn(cpu, in);
    }
}

_Static_assert(offsetof(GuestCpu, rip) == offsetof(GuestHookCpu, rip) && offsetof(GuestCpu, flags) == offsetof(GuestHookCpu, flags) &&
                   offsetof(GuestCpu, v) == offsetof(GuestHookCpu, v) && sizeof(GuestVec) == sizeof(((GuestHookCpu *)0)->v[0]),
               "GuestHookCpu: the start of GuestCpu");

/* Entry hooks: each hooked function's table entry and direct slot hold a wrapper of its own,
 * which runs the hook with `inner` (the function, or the wrapper of an earlier hook) to call. */
enum { ENTRY_HOOKS_MAX = 128 };
typedef struct { uint64_t address; GuestEntryHook hook; void *data; RcFn inner; } EntryHook;
static EntryHook entry_hooks[ENTRY_HOOKS_MAX];
static int entry_hook_count;

static void run_entry_hook(GuestCpu *cpu, int i);
static void run_entry_inner(GuestHookCpu *cpu, int i);
#define ENTRY_HOOK(a, b) \
    static void entry_hook_##a##b(GuestCpu *c) { run_entry_hook(c, (a) * 8 + (b)); } \
    static void entry_inner_##a##b(GuestHookCpu *c) { run_entry_inner(c, (a) * 8 + (b)); }
#define ENTRY_HOOK8(a) ENTRY_HOOK(a, 0) ENTRY_HOOK(a, 1) ENTRY_HOOK(a, 2) ENTRY_HOOK(a, 3) ENTRY_HOOK(a, 4) \
    ENTRY_HOOK(a, 5) ENTRY_HOOK(a, 6) ENTRY_HOOK(a, 7)
ENTRY_HOOK8(0) ENTRY_HOOK8(1) ENTRY_HOOK8(2) ENTRY_HOOK8(3) ENTRY_HOOK8(4) ENTRY_HOOK8(5) ENTRY_HOOK8(6) ENTRY_HOOK8(7)
ENTRY_HOOK8(8) ENTRY_HOOK8(9) ENTRY_HOOK8(10) ENTRY_HOOK8(11) ENTRY_HOOK8(12) ENTRY_HOOK8(13) ENTRY_HOOK8(14) ENTRY_HOOK8(15)
#define ENTRY_NAMES(p, a) p##a##0, p##a##1, p##a##2, p##a##3, p##a##4, p##a##5, p##a##6, p##a##7
#define ENTRY_NAMES16(p) ENTRY_NAMES(p, 0), ENTRY_NAMES(p, 1), ENTRY_NAMES(p, 2), ENTRY_NAMES(p, 3), ENTRY_NAMES(p, 4), \
    ENTRY_NAMES(p, 5), ENTRY_NAMES(p, 6), ENTRY_NAMES(p, 7), ENTRY_NAMES(p, 8), ENTRY_NAMES(p, 9), ENTRY_NAMES(p, 10), \
    ENTRY_NAMES(p, 11), ENTRY_NAMES(p, 12), ENTRY_NAMES(p, 13), ENTRY_NAMES(p, 14), ENTRY_NAMES(p, 15)
static const RcFn entry_hook_fns[ENTRY_HOOKS_MAX] = {ENTRY_NAMES16(entry_hook_)};
static void (*const entry_inner_fns[ENTRY_HOOKS_MAX])(GuestHookCpu *) = {ENTRY_NAMES16(entry_inner_)};

static void run_entry_hook(GuestCpu *cpu, int i) {
    const EntryHook *h = &entry_hooks[i];
    h->hook((GuestHookCpu *)cpu, h->data, entry_inner_fns[i]);
}
static void run_entry_inner(GuestHookCpu *hook_cpu, int i) {
    GuestCpu *cpu = (GuestCpu *)hook_cpu;
    cpu->rip = entry_hooks[i].address;
    entry_hooks[i].inner(cpu);
}

/* A dropped function behind an entry hook: the translator from its entry, its first instruction
 * stepped here (the translator would look the entry up in the table, which leads to the hook). */
static void run_dropped(GuestCpu *cpu) {
    const uint64_t slot = cpu->r[RSP];
    const GuestBlock *block = guest_cpu_block(cpu->rip);
    if (!block->count) guest_cpu_fatal(cpu, NULL, "undecodable code");
    cpu->rip = guest_cpu_step_insn(cpu, &block->insn[0]);
    if (cpu->r[RSP] == slot + 8) return; /* it returned */
    const uint64_t back = guest_load(slot, 8);
    run_translated(cpu, slot);
    cpu->rip = back;
}

/* The direct slot of a function that does not run from the library (not loaded, dropped,
 * GUEST_RECOMP_NODIRECT): a call of cpu->rip as RcApi.call makes it, the return address the
 * library's call pushed taken off first. A tail call this way returns to the function that made
 * it, which returns. */
static void run_unloaded(GuestCpu *cpu) {
    const uint64_t next = guest_load(cpu->r[RSP], 8);
    cpu->r[RSP] += 8;
    rc_call_checked(cpu, cpu->rip, next);
}

static Loaded *loaded_starting(uint64_t address) {
    const size_t i = loaded_through(address);
    return i && loaded[i - 1].start == address ? &loaded[i - 1] : NULL;
}

/* What the library's calls of a function in use run (its slot holds NULL for the unpatched one). */
static RcFn slot_runs(const Loaded *l) {
    return *l->direct ? *l->direct : l->self;
}

int guest_cpu_recomp_hook_entry(uint64_t address, GuestEntryHook hook, void *data, uint64_t unhooked) {
    const RcFn fn = guest_cpu_recomp_at(address);
    if (!fn || entry_hook_count == ENTRY_HOOKS_MAX) return 0;
    const int i = entry_hook_count++;
    entry_hooks[i] = (EntryHook){address, hook, data, fn};
    add(address, entry_hook_fns[i]);
    Loaded *l = loaded_starting(address);
    if (l && l->direct && slot_runs(l) == fn) *l->direct = entry_hook_fns[i];
    if (unhooked) add(unhooked, fn);
    return 1;
}

void guest_cpu_hook_call_host(GuestHookCpu *hook_cpu, const void *fn) {
    GuestCpu *cpu = (GuestCpu *)hook_cpu;
    cpu->rip = (uint64_t)(uintptr_t)fn;
    guest_cpu_call_host_at_rip(cpu);
}

/* Whether the bytes [from, to) of a function the version runs are ones it reads as it runs. */
static int reads_live(Loaded *l, uint64_t from, uint64_t to) {
    if (!l->version) return 0;
    const RcSpan *live = l->version->live;
    for (uint64_t at = from; at < to;) {
        uint32_t k = 0;
        while (k < l->version->live_count &&
               !(loaded_base + live[k].offset <= at && at < loaded_base + live[k].offset + live[k].size))
            ++k;
        if (k == l->version->live_count) return 0;
        at = loaded_base + live[k].offset + live[k].size;
    }
    return 1;
}

/* Calls each(l, from, to) for each function in use holding bytes of [address, end), with the part
 * it holds (its own code or a span); stops at a zero. */
static int each_holding(uint64_t address, uint64_t end, int (*each)(Loaded *l, uint64_t from, uint64_t to)) {
    for (size_t i = loaded_through(end - 1); i-- > 0 && loaded[i].reach > address;) {
        Loaded *l = &loaded[i];
        if (!l->dropped && l->end > address &&
            !each(l, address > l->start ? address : l->start, end < l->end ? end : l->end))
            return 0;
    }
    for (size_t i = 0; i < spanned_count; ++i) {
        Loaded *l = &loaded[spanned[i]];
        for (uint32_t k = 0; !l->dropped && k < l->version->span_count; ++k) {
            const uint64_t start = loaded_base + l->version->spans[k].offset, stop = start + l->version->spans[k].size;
            if (start < end && address < stop &&
                !each(l, address > start ? address : start, end < stop ? end : stop))
                return 0;
        }
    }
    return 1;
}

int guest_cpu_recomp_code_checked(uint64_t address, size_t size) {
    return !size || each_holding(address, address + size, reads_live);
}

static int drop(Loaded *l, uint64_t from, uint64_t to) {
    (void)from;
    (void)to;
    l->dropped = 1;
    for (uint64_t k = 0; k <= table_mask; ++k) /* its entry and the addresses of its unhooked self */
        if (table[k].address && table[k].fn == l->fn) table[k].fn = NULL;
    for (int k = 0; k < entry_hook_count; ++k)
        if (entry_hooks[k].inner == l->fn) entry_hooks[k].inner = run_dropped;
    if (l->direct && slot_runs(l) == l->fn) *l->direct = run_unloaded;
    printf("Recompiled: the function at %#llx is left to the translator: its code is rewritten\n",
           (unsigned long long)(l->start - loaded_base));
    return 1;
}

static int drop_unread(Loaded *l, uint64_t from, uint64_t to) {
    if (!reads_live(l, from, to)) drop(l, from, to);
    return 1;
}

void guest_cpu_recomp_drop(uint64_t address, size_t size) {
    if (size) each_holding(address, address + size, drop_unread);
}

/* Calls of recompiled functions per second, every 10 s while there are any. */
static void *report(void *unused) {
    (void)unused;
    for (uint64_t last = 0;;) {
        sleep(10);
        const uint64_t now = __atomic_load_n(&guest_cpu_recomp_calls, __ATOMIC_RELAXED);
        static uint64_t last_runs;
        const uint64_t runs = __atomic_load_n(&translated_runs, __ATOMIC_RELAXED);
        if (now != last)
            printf("Recompiled: %.0f calls/s; %.0f/s into the translator, %.0f/s straight to host functions, "
                   "%.0f/s at hook points (%.0f instructions/s of them in the interpreter), %llu longjmps to outer "
                   "frames, frames up to %llu deep; longjmps: %.0f/s to compiled setjmps, %llu into the translator\n",
                   (double)(now - last) / 10.0,
                   (double)(runs - last_runs) / 10.0, (double)(__atomic_exchange_n(&host_calls, 0, __ATOMIC_RELAXED)) / 10.0,
                   (double)(__atomic_exchange_n(&hook_runs, 0, __ATOMIC_RELAXED)) / 10.0,
                   (double)(__atomic_exchange_n(&hook_steps, 0, __ATOMIC_RELAXED)) / 10.0,
                   (unsigned long long)__atomic_load_n(&frame_jumps, __ATOMIC_RELAXED),
                   (unsigned long long)__atomic_load_n(&deepest_frame, __ATOMIC_RELAXED),
                   (double)(__atomic_exchange_n(&resume_jumps, 0, __ATOMIC_RELAXED)) / 10.0,
                   (unsigned long long)__atomic_load_n(&landings_left, __ATOMIC_RELAXED));
        last = now;
        last_runs = runs;
        if (count_targets()) {
            uint64_t best[10][2];
            take_best(targets, best);
            {
                uint64_t top[15][2] = {{0}};
                for (int m = 0; m < STEP_MNEMONICS; ++m) {
                    const uint64_t c = __atomic_exchange_n(&step_counts[m], 0, __ATOMIC_RELAXED);
                    for (int k = 0; k < 15; ++k)
                        if (c > top[k][1]) { memmove(top[k + 1], top[k], (size_t)(14 - k) * sizeof(top[0])); top[k][0] = (uint64_t)m; top[k][1] = c; break; }
                }
                printf("Recompiled: interpreted instructions per second:");
                for (int k = 0; k < 15 && top[k][1]; ++k)
                    printf(" %s:%.0f", ZydisMnemonicGetString((ZydisMnemonic)top[k][0]), (double)top[k][1] / 10.0);
                printf("\n");
            }
            printf("Recompiled: calls into the translator by target (x64 samples):");
            for (int k = 0; k < 10 && best[k][1]; ++k)
                printf(" %#llx:%llu", (unsigned long long)(best[k][0] - recomp_image_base), (unsigned long long)best[k][1] * 64);
            printf("\n");
            {
                uint64_t from[10][3] = {{0}};
                for (int i = 0; i < TARGET_SLOTS; ++i) {
                    const uint64_t c = __atomic_exchange_n(&targets_from[i].count, 0, __ATOMIC_RELAXED);
                    for (int k = 0; k < 10; ++k)
                        if (c > from[k][2]) {
                            memmove(from[k + 1], from[k], (size_t)(9 - k) * sizeof(from[0]));
                            from[k][0] = targets_from[i].rip;
                            from[k][1] = targets_from[i].back;
                            from[k][2] = c;
                            break;
                        }
                }
                printf("Recompiled: calls into the translator by target and return address (x64 samples):");
                for (int k = 0; k < 10 && from[k][2]; ++k)
                    printf(" %#llx<-%#llx:%llu", (unsigned long long)(from[k][0] - recomp_image_base),
                           (unsigned long long)(from[k][1] - recomp_image_base), (unsigned long long)from[k][2] * 64);
                printf("\n");
            }
            {
                uint64_t top[40][2] = {{0}};
                for (int i = 0; i < TARGET_SLOTS; ++i) {
                    const uint64_t c = __atomic_exchange_n(&hook_sites[i].count, 0, __ATOMIC_RELAXED);
                    for (int k = 0; k < 40; ++k)
                        if (c > top[k][1]) {
                            memmove(top[k + 1], top[k], (size_t)(39 - k) * sizeof(top[0]));
                            top[k][0] = hook_sites[i].rip;
                            top[k][1] = c;
                            break;
                        }
                }
                printf("Recompiled: hook points by site (x64 samples):");
                for (int k = 0; k < 40 && top[k][1]; ++k)
                    printf(" %#llx:%llu", (unsigned long long)(top[k][0] - recomp_image_base), (unsigned long long)top[k][1] * 64);
                printf("\n");
            }
            take_best(hook_step_sites, best);
            printf("Recompiled: instructions of hook points in the interpreter by address (x64 samples):");
            for (int k = 0; k < 10 && best[k][1]; ++k) {
                const uint64_t at = best[k][0];
                if (hook_code_count && at >= hook_code[0].start && at < hook_code[0].end)
                    printf(" hook+%#llx:%llu", (unsigned long long)(at - hook_code[0].start), (unsigned long long)best[k][1] * 64);
                else printf(" %#llx:%llu", (unsigned long long)(at - recomp_image_base), (unsigned long long)best[k][1] * 64);
            }
            printf("\n");
            /* The host functions behind the import stubs: image+offset (atos resolves a static
             * function; dladdr names only exported ones, given when exact). */
            take_best(host_targets, best);
            printf("Recompiled: host functions called straight by address (x64 samples):");
            for (int k = 0; k < 10 && best[k][1]; ++k) {
                Dl_info info;
                if (dladdr((void *)(uintptr_t)best[k][0], &info) && info.dli_fname) {
                    const char *file = strrchr(info.dli_fname, '/');
                    printf(" %s+%#llx", file ? file + 1 : info.dli_fname,
                           (unsigned long long)(best[k][0] - (uintptr_t)info.dli_fbase));
                    if (info.dli_sname && (uintptr_t)info.dli_saddr == best[k][0]) printf("(%s)", info.dli_sname);
                } else {
                    printf(" %#llx", (unsigned long long)best[k][0]);
                }
                printf(":%llu", (unsigned long long)best[k][1] * 64);
            }
            printf("\n");
            printf("Recompiled: %.0f instructions/s in the interpreter in all; by address (x16 samples):",
                   (double)__atomic_exchange_n(&interpreted, 0, __ATOMIC_RELAXED) / 10.0);
            take_best(interpreted_at, best);
            for (int k = 0; k < 10 && best[k][1]; ++k)
                printf(" %#llx:%llu", (unsigned long long)best[k][0], (unsigned long long)best[k][1] * 16);
            printf("\n");
            printf("Recompiled: %.0f host calls of guest code/s; by target (x16 samples):",
                   (double)__atomic_exchange_n(&entries, 0, __ATOMIC_RELAXED) / 10.0);
            take_best(entries_at, best);
            for (int k = 0; k < 10 && best[k][1]; ++k)
                printf(" %#llx:%llu", (unsigned long long)best[k][0], (unsigned long long)best[k][1] * 16);
            printf("\n");
        }
    }
    return NULL;
}

/* A set of image offsets from an environment variable: "OFFSET,OFFSET..." or "@FILE" (offsets
 * separated by commas, spaces or lines). */
typedef struct { uint64_t *v; size_t n; int given; } OffsetSet;
static int by_value(const void *a, const void *b) {
    const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}
static OffsetSet offset_set(const char *name) {
    OffsetSet set = {0};
    const char *value = getenv(name);
    if (!value || !*value) return set;
    set.given = 1;
    char *text = NULL;
    if (value[0] == '@') {
        FILE *f = fopen(value + 1, "r");
        if (!f) { fprintf(stderr, "Recompiled: %s: cannot read %s\n", name, value + 1); return set; }
        fseek(f, 0, SEEK_END);
        const long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        text = calloc((size_t)size + 1, 1);
        if (text && fread(text, 1, (size_t)size, f) != (size_t)size) text[0] = 0;
        fclose(f);
    } else {
        text = strdup(value);
    }
    size_t capacity = 0;
    for (char *p = text; p && *p;) {
        char *end;
        const uint64_t v = strtoull(p, &end, 0);
        if (end == p) { ++p; continue; }
        if (set.n == capacity) { capacity = capacity ? capacity * 2 : 256; set.v = realloc(set.v, capacity * sizeof(*set.v)); }
        set.v[set.n++] = v;
        p = end;
    }
    free(text);
    if (set.n) qsort(set.v, set.n, sizeof(*set.v), by_value);
    return set;
}
static int in_set(const OffsetSet *set, uint64_t offset) {
    return set->n && bsearch(&offset, set->v, set->n, sizeof(*set->v), by_value) != NULL;
}

static size_t recomp_used;

/* Code the runtime rewrites while the game runs (live switches): the functions that hold it stay
 * with the translator, whose blocks guest_cpu_invalidate drops. */
enum { MAX_EXCLUDED = 64 };
static struct { uint64_t start, end; } excluded[MAX_EXCLUDED];
static int excluded_count;
void guest_cpu_recomp_exclude(uint64_t address, size_t size) {
    if (excluded_count == MAX_EXCLUDED) {
        fputs("Recompiled: too many excluded ranges\n", stderr);
        abort();
    }
    excluded[excluded_count].start = address;
    excluded[excluded_count].end = address + size;
    ++excluded_count;
}
static int is_excluded(uint64_t start, uint64_t size) {
    for (int i = 0; i < excluded_count; ++i)
        if (excluded[i].start < start + size && start < excluded[i].end) return 1;
    return 0;
}

size_t guest_cpu_recomp_functions(void) {
    return recomp_used;
}

/* The libraries in the register convention by image (their guest_recomp_find), and the entries
 * RcApi.find gives the calls of other libraries: one for each target and calling library, kept
 * for good (open addressing, at most half full). */
typedef struct {
    uint64_t base, size;
    RcFind find;
} AbiLibrary;
enum { ABI_LIBRARIES = 16, FOREIGN_BITS = 16 };
static AbiLibrary abi_libraries[ABI_LIBRARIES];
static int abi_library_count;
typedef struct {
    RcEntry entry;
    uint64_t target, caller_base;
} ForeignEntry;
static const ForeignEntry *foreign_entries[1u << FOREIGN_BITS];
static unsigned foreign_count;
static pthread_mutex_t foreign_lock = PTHREAD_MUTEX_INITIALIZER;
static void foreign_held_fn(GuestCpu *cpu) {
    (void)cpu;
    __builtin_trap();
}
static RcFn foreign_held = foreign_held_fn; /* check and slot of an entry without a_<offset> */

static const RcEntry *rc_find_foreign(uint64_t target, uint64_t caller_base) {
    const uint64_t mask = (UINT64_C(1) << FOREIGN_BITS) - 1;
    uint64_t k = ((target ^ caller_base * 0xc2b2ae3d27d4eb4full) * 0x9e3779b97f4a7c15ull) >> (64 - FOREIGN_BITS);
    for (const ForeignEntry *f; (f = __atomic_load_n(&foreign_entries[k], __ATOMIC_ACQUIRE)); k = (k + 1) & mask)
        if (f->target == target && f->caller_base == caller_base) return &f->entry;
    pthread_mutex_lock(&foreign_lock);
    for (const ForeignEntry *f; (f = foreign_entries[k]); k = (k + 1) & mask) /* another thread's since */
        if (f->target == target && f->caller_base == caller_base) {
            pthread_mutex_unlock(&foreign_lock);
            return &f->entry;
        }
    ForeignEntry *f = foreign_count < (mask + 1) / 2 ? malloc(sizeof(*f)) : NULL;
    if (f) {
        const RcEntry *own = NULL;
        const int libraries = __atomic_load_n(&abi_library_count, __ATOMIC_ACQUIRE);
        for (int i = 0; i < libraries && !own; ++i)
            if (target - abi_libraries[i].base < abi_libraries[i].size)
                own = abi_libraries[i].find(target - abi_libraries[i].base);
        f->entry = own ? (RcEntry){target - caller_base, own->slot, own->check, own->abi}
                       : (RcEntry){target - caller_base, &foreign_held, &foreign_held, NULL};
        f->target = target;
        f->caller_base = caller_base;
        __atomic_store_n(&foreign_entries[k], f, __ATOMIC_RELEASE);
        ++foreign_count;
    }
    pthread_mutex_unlock(&foreign_lock);
    return f ? &f->entry : NULL;
}
/* With GUEST_RECOMP_NODIRECT or a thread filter: calls into other libraries go through RcApi.call
 * and RcApi.import, which check. */
static const RcEntry *rc_find_none(uint64_t target, uint64_t caller_base) {
    (void)target;
    (void)caller_base;
    return NULL;
}

/* What a library of the image at image_base calls the runtime through: the library keeps it. */
static const RcApi *library_api(uint64_t image_base) {
    const int checked = no_direct() || guest_cpu_recomp_threads_filtered;
    RcApi *api = calloc(1, sizeof(*api));
    if (!api) abort();
    *api = (RcApi){.version = GUEST_RECOMP_API_VERSION, .image_base = image_base, .step = rc_step,
                   .call = checked ? rc_call_checked : rc_call, .tail = checked ? rc_tail_checked : rc_tail,
                   .bail = rc_bail, .fault = rc_fault, .trap = rc_trap, .set_mxcsr = rc_set_mxcsr,
                   .hook = rc_hook, .resume = rc_resume, .leave = rc_leave,
                   .import = checked ? rc_import_checked : rc_import,
                   .find = checked ? rc_find_none : rc_find_foreign};
    return api;
}

/* The library at `path`, of the image at image_base (the executable, or a module when `module`
 * names it): its functions whose code the image holds join the table. 0: not loaded. */
static int load_library(const char *path, uint64_t image_base, const char *module) {
    void *library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    const RcInit init = library ? (RcInit)dlsym(library, GUEST_RECOMP_INIT) : NULL;
    if (!init) {
        fprintf(stderr, "Recompiled: %s: %s\n", path, dlerror());
        return 0;
    }
    size_t count = 0;
    const RcFunction *functions = init(library_api(image_base), &count);
    if (!module) reserve(count + MODULE_FUNCTIONS);
    else if ((size_t)guest_cpu_recomp_count + count > table_room) {
        fprintf(stderr, "Recompiled: %s: no room for its %zu functions\n", module, count);
        return 0;
    }
    /* GUEST_RECOMP_OFF: these not; GUEST_RECOMP_ONLY: only these (bisecting); the executable's. */
    const OffsetSet off = module ? (OffsetSet){0} : offset_set("GUEST_RECOMP_OFF"),
                    only = module ? (OffsetSet){0} : offset_set("GUEST_RECOMP_ONLY");
    size_t used = 0, changed = 0, kept = 0, games = 0, patched = 0, patch_code = 0;
    for (size_t i = 0, end; i < count; i = end) {
        /* The versions of one function, the unpatched one first: the one the loaded code is. */
        for (end = i + 1; end < count && functions[end].offset == functions[i].offset; ++end) {}
        const RcFunction *first = &functions[i];
        if (first->direct) *first->direct = run_unloaded; /* until it loads */
        const int game = !(first->flags & RC_PATCH_CODE);
        games += game;
        if (in_set(&off, first->offset) || (only.given && !in_set(&only, first->offset))) continue;
        if (is_excluded(image_base + first->offset, first->size)) {
            ++kept;
            continue;
        }
        const RcFunction *chosen = NULL;
        for (size_t k = i; k < end && !chosen; ++k) {
            const RcFunction *v = &functions[k];
            if (guest_cpu_recomp_hash_parts(image_base, v->offset, v->size, (const uint64_t *)v->spans, v->span_count,
                                        (const uint64_t *)v->live, v->live_count) == v->hash)
                chosen = v;
        }
        RcFn fn = chosen ? chosen->fn : hooked_entry(image_base + first->offset, first->size, first->hash, first->fn);
        if (!fn) {
            changed += game; /* patched since (game patches, hooks elsewhere): the translator runs it */
            if (count_targets())
                printf("Recompiled: %s%#llx: no version with the loaded code\n", game ? "" : "function of patches ",
                       (unsigned long long)first->offset);
            continue;
        }
        patched += game && chosen && chosen != first;
        patch_code += !game;
        add(image_base + first->offset, fn);
        if (first->direct && !no_direct()) *first->direct = fn == first->fn ? NULL : fn;
        ++used;
        if (!module) {
            const RcFunction *v = chosen ? chosen : first;
            loaded = realloc(loaded, (loaded_count + 1) * sizeof(*loaded));
            if (!loaded) abort();
            loaded[loaded_count++] = (Loaded){image_base + v->offset, image_base + v->offset + v->size, 0, chosen, fn,
                                              first->direct, 0, first->fn};
        }
    }
    if (!module) {
        loaded_base = image_base;
        qsort(loaded, loaded_count, sizeof(*loaded), loaded_order);
        for (size_t k = 0; k < loaded_count; ++k) {
            loaded[k].reach = k && loaded[k - 1].reach > loaded[k].end ? loaded[k - 1].reach : loaded[k].end;
            if (loaded[k].version && loaded[k].version->span_count) {
                spanned = realloc(spanned, (spanned_count + 1) * sizeof(*spanned));
                if (!spanned) abort();
                spanned[spanned_count++] = k;
            }
        }
    }
    /* Its functions in the register convention, their slots filled: for the calls of other
     * libraries (RcApi.find). */
    const RcFind find = (RcFind)dlsym(library, GUEST_RECOMP_FIND);
    if (find && abi_library_count < ABI_LIBRARIES) {
        uint64_t size = 0;
        for (size_t i = 0; i < count; ++i)
            if (functions[i].offset + functions[i].size > size) size = functions[i].offset + functions[i].size;
        abi_libraries[abi_library_count] = (AbiLibrary){image_base, size, find};
        __atomic_store_n(&abi_library_count, abi_library_count + 1, __ATOMIC_RELEASE);
    }
    recomp_used += used;
    if (module)
        printf("Recompiled: %s: %zu of %zu functions replaced, %zu changed (%s)\n", module, used, games, changed, path);
    else
        printf("Recompiled: %zu of %zu game functions replaced (%zu by a patched version, %d with a hook at the entry), "
               "%zu functions of patches, %zu changed by patches, %zu rewritten live (%s)\n",
               used - patch_code, games, patched, hooked_count, patch_code, changed, kept, path);
    return 1;
}

void guest_cpu_recomp_load(uint64_t image_base) {
    recomp_image_base = image_base;
    const char *path = getenv("GUEST_RECOMP_LIB");
    if (!path || !*path) return;
    guest_cpu_return_hook = return_hook;
    guest_cpu_recomp_threads_filtered = getenv("GUEST_RECOMP_THREADS") || getenv("GUEST_RECOMP_NOT_THREADS");
    if (!load_library(path, image_base, NULL)) return;
    guest_cpu_recomp_census = count_targets();
    pthread_t thread;
    if (recomp_used && pthread_create(&thread, NULL, report, NULL) == 0) pthread_detach(thread);
}

void guest_cpu_recomp_load_module(const char *path, uint64_t image_base, const char *name) {
    if (!table || access(path, R_OK) != 0) return;
    load_library(path, image_base, name);
}

/* GUEST_RECOMP_HOOK_DUMP=PREFIX: the hook code as the program wrote it, for recompile --hook-code:
 * its bytes in PREFIX.bin, and in PREFIX.txt its address, then the offsets in it that the jumps
 * and calls of the hook sites lead to (the fields the functions in use read as they run that hold
 * a jmp rel32 or a call rel32). */
static void dump_hook_code(const char *prefix) {
    const uint64_t start = hook_code[0].start, end = hook_code[0].end;
    char path[1024];
    snprintf(path, sizeof(path), "%s.bin", prefix);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite((const void *)(uintptr_t)start, 1, end - start, f) != end - start) {
        perror(path);
        abort();
    }
    fclose(f);
    snprintf(path, sizeof(path), "%s.txt", prefix);
    f = fopen(path, "w");
    if (!f) {
        perror(path);
        abort();
    }
    fprintf(f, "%#llx\n", (unsigned long long)start);
    size_t entries = 0;
    for (size_t k = 0; k < loaded_count; ++k) {
        const RcFunction *v = loaded[k].version;
        for (uint32_t i = 0; v && i < v->live_count; ++i) {
            const uint64_t at = loaded_base + v->live[i].offset;
            int32_t rel;
            if (v->live[i].size < 5 || (*(const uint8_t *)(uintptr_t)at | 1) != 0xe9) continue;
            memcpy(&rel, (const void *)(uintptr_t)(at + 1), 4);
            const uint64_t target = at + 5 + (uint64_t)(int64_t)rel;
            if (target < start || target >= end) continue;
            fprintf(f, "%#llx\n", (unsigned long long)(target - start));
            ++entries;
        }
    }
    fclose(f);
    printf("Recompiled: hook code written to %s.bin, the %zu places hook sites jump to in it to %s.txt\n", prefix, entries,
           prefix);
}

void guest_cpu_recomp_load_hook_code(const char *path) {
    if (!table || hook_code_count != 1) return;
    const char *dump = getenv("GUEST_RECOMP_HOOK_DUMP");
    if (dump && *dump) dump_hook_code(dump);
    if (access(path, R_OK) != 0) return;
    void *library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    const RcInit init = library ? (RcInit)dlsym(library, GUEST_RECOMP_INIT) : NULL;
    if (!init) {
        fprintf(stderr, "Recompiled: %s: %s\n", path, dlerror());
        return;
    }
    const uint64_t start = hook_code[0].start, end = hook_code[0].end;
    size_t count = 0;
    hook_functions = init(library_api(start), &count);
    int bits = 4;
    while ((UINT64_C(1) << bits) < 2 * (end - start)) ++bits;
    hook_fns = calloc(UINT64_C(1) << bits, sizeof(*hook_fns));
    if (!hook_fns) abort();
    hook_fn_mask = (UINT64_C(1) << bits) - 1;
    hook_fn_shift = 64 - bits;
    /* How many are the code where the dump had them: all, when the program wrote the same hooks. */
    size_t in_place = 0;
    for (size_t i = 0; i < count; ++i) {
        const RcFunction *f = &hook_functions[i];
        in_place += start + f->offset + f->size <= end &&
                    guest_cpu_recomp_hook_hash(start + f->offset, f->offset, f->size, (const uint64_t *)f->live,
                                               f->live_count) == f->hash;
    }
    guest_cpu_recomp_hook_count = (int)count;
    printf("Recompiled: hook code: %zu functions, %zu of them the code at their places (%s)\n", count, in_place, path);
}
