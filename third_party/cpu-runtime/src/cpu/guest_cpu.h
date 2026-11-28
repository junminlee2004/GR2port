/* guest-cpu: runs a PS4 game's x86-64 code on an arm64 host. Guest memory is the
 * host's (identity-mapped); a guest call or jump to an address outside the guest code ranges is a
 * call of a host function (SysV arguments bridged to the host ABI). */
#ifndef GUEST_CPU_H
#define GUEST_CPU_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* guest-cpu is built on macOS (src/cpu, build.sh); elsewhere guest code runs natively. */
#if defined(__APPLE__)
#define HAVE_GUEST_CPU 1
#endif
#ifdef HAVE_GUEST_CPU

/* Whether guest code runs through guest-cpu (GUEST_CPU=interp in x86-64 builds; always on arm64). */
int guest_cpu_enabled(void);
/* The ranges holding guest code (the linked image); anything else is host code. */
void guest_cpu_add_guest_code(uintptr_t start, size_t size);
/* Calls guest function `fn` on this thread with up to 6 integer arguments; returns rax. */
uint64_t guest_cpu_call(uintptr_t fn, int count, const uint64_t *args);
/* Like guest_cpu_call with SysV integer arguments (rdi rsi rdx rcx r8 r9) and vector ones (xmm0-7);
 * rax, rdx and xmm0, xmm1 back. */
void guest_cpu_call_full(uintptr_t fn, const uint64_t gpr[6], const uint8_t xmm[8][16], uint64_t out_gpr[2],
                     uint8_t out_xmm[2][16]);
/* Like guest_cpu_call_full with `stack_count` integer arguments after the sixth, `stack`, on the
 * stack (the first at rsp + 8 at the callee's entry). */
void guest_cpu_call_full_stack(uintptr_t fn, const uint64_t gpr[6], const uint8_t xmm[8][16], const uint64_t *stack,
                               int stack_count, uint64_t out_gpr[2], uint8_t out_xmm[2][16]);
/* Native versions of game functions (bloodborne-recomp's docs/RECOMPILATION.md): a call of guest `address` runs host
 * function `fn` (SysV arguments bridged, like a host import). Set before guest code runs. */
void guest_cpu_set_native(uint64_t address, const void *fn);
/* Probes (diagnostics): host function `fn` runs when guest code reaches `address`, with the
 * guest's registers (rax rcx rdx rbx rsp rbp rsi rdi r8-r15) and `tag`; the guest code then runs
 * as usual. Set before guest code runs. */
typedef void (*GuestProbe)(const uint64_t regs[16], uint64_t address, uint32_t tag);
void guest_cpu_set_probe(uint64_t address, GuestProbe fn, uint32_t tag);
/* Guards: `enter` runs when guest code reaches `address` (a function's first instruction) and
 * returns a cookie; with a non-zero cookie, `leave(cookie)` runs when that call returns (its
 * return address is kept on a per-thread stack and the guest's slot holds GUEST_GUARD_RETURN until
 * then). Recompiled functions (GUEST_RECOMP_LIB) call each other directly: not guarded there. */
typedef uint64_t (*GuestGuardEnter)(const uint64_t regs[16], uint32_t tag);
typedef void (*GuestGuardLeave)(uint64_t cookie);
void guest_cpu_set_guard(uint64_t address, GuestGuardEnter enter, GuestGuardLeave leave, uint32_t tag);
/* Active guards on this thread, filtered by owner; terminating diagnostics only. */
size_t guest_cpu_guard_cookies(GuestGuardLeave leave, uint64_t *cookies, size_t capacity);
/* GUEST_RECOMP_LIB: recompiled game functions replace the translated ones (recomp.c). */
void guest_cpu_recomp_load(uint64_t image_base);
/* After guest_cpu_recomp_load: the recompiled library at `path` of the game module `name` loaded at
 * image_base (its functions join the others as the module loads); nothing without the file. */
void guest_cpu_recomp_load_module(const char *path, uint64_t image_base, const char *name);
/* Code at [address, address + size) is rewritten while the game runs: the recompiled library
 * leaves the functions holding it to the translator. Set before guest_cpu_recomp_load. */
void guest_cpu_recomp_exclude(uint64_t address, size_t size);
/* How many game functions the recompiled library replaced (0: none loaded). */
size_t guest_cpu_recomp_functions(void);
/* Hooks of the host program in recompiled functions (after guest_cpu_recomp_load, before guest code
 * runs). A hook sees the guest registers as the start of the thread's CPU state. */
typedef struct {
    uint64_t r[16]; /* rax rcx rdx rbx rsp rbp rsi rdi r8-r15 */
    uint64_t rip;
    uint64_t flags;
    uint8_t v[16][32]; /* ymm0-15 */
} GuestHookCpu;
/* A hook at a recompiled function's entry: it runs for each call with the registers as the caller
 * left them (rip the function, its return address on top of the stack) and returns with rip where
 * the call returned to. `function(cpu)` runs the function itself (rip as the hook got it) and
 * returns once the function returned. */
typedef void (*GuestEntryHook)(GuestHookCpu *cpu, void *data, void (*function)(GuestHookCpu *cpu));
/* Makes each call of the recompiled function at `address` run hook(cpu, data, ...) in its place.
 * With `unhooked`, an unused address of guest code, a call of that address runs the function as it
 * was before. A second hook of the function runs first. 0: no recompiled function starts there,
 * or no room for another hook. */
int guest_cpu_recomp_hook_entry(uint64_t address, GuestEntryHook hook, void *data, uint64_t unhooked);
/* In a hook: runs host function `fn` as the guest's call it handles (arguments in the guest's
 * registers, results in rax, rdx, xmm0 and xmm1), and pops the return address into rip. */
void guest_cpu_hook_call_host(GuestHookCpu *cpu, const void *fn);
/* Whether code written at [address, address + size) takes effect: no recompiled function holds
 * those bytes, or the one holding them reads them as it runs (fields of patches, hook points). */
int guest_cpu_recomp_code_checked(uint64_t address, size_t size);
/* The recompiled functions holding bytes of [address, address + size) that they do not read as
 * they run leave their calls to the translator. */
void guest_cpu_recomp_drop(uint64_t address, size_t size);
/* [start, start + size) holds the program's hook code (stubs that hooked code jumps to): where
 * a hook point finds other bytes than the compiled ones, they and this code run in the
 * interpreter until the guest goes on elsewhere. */
void guest_cpu_recomp_hook_code(uint64_t start, size_t size);
/* After the hook code is written and before guest code runs it: loads the library of its
 * recompiled C at `path`, when that file exists, whose functions then run in place of the code
 * they were written from wherever the hook code holds it. Each call adds a library: one for each
 * hook code that settings of the program give. */
void guest_cpu_recomp_load_hook_code(const char *path);
/* After the libraries of the hook code load: whether one of them is this hook code, made from a
 * dump of the same code: each of its functions the code at its place, and one at each place the
 * hook sites jump to. Otherwise some of the hook code can run in the interpreter. */
int guest_cpu_recomp_hook_fits(void);
/* Writes the hook code as GUEST_RECOMP_HOOK_DUMP=prefix does, for recompile --hook-code. */
void guest_cpu_recomp_dump_hook_code(const char *prefix);
/* Like guest_cpu_call on a new guest stack [stack, stack + size) (thread entry, game entry). */
uint64_t guest_cpu_call_on_stack(uintptr_t fn, int count, const uint64_t *args, void *stack,
                             size_t size);
/* Like guest_cpu_call_on_stack with no return address pushed: fn starts with the stack pointer
 * at `rsp`, the stack below the top as the caller filled it for fn, and never returns. */
void guest_cpu_start_on_stack(uintptr_t fn, int count, const uint64_t *args, uint64_t rsp, void *stack,
                              size_t size);
/* The guest thread pointer (TCB, the guest's fs base) of this thread. */
void guest_cpu_set_thread_pointer(uintptr_t tcb);
/* Guest code on host stacks of its own that switch on one host thread (fibers: one switches away
 * in the middle of a call and comes back later) needs a guest CPU state per stack: registers, call
 * depth and the recompiled code's frames. guest_cpu_context_create makes an empty one, which gets its
 * CPU on its first guest call. guest_cpu_context_switch moves this thread's current state into `save`
 * and makes `load`'s current, with this thread's thread pointer and floating-point control
 * applied. Guards (guest_cpu_set_guard) stay per host thread. */
typedef struct GuestContext GuestContext;
GuestContext *guest_cpu_context_create(void);
void guest_cpu_context_switch(GuestContext *save, GuestContext *load);
void guest_cpu_context_destroy(GuestContext *context);
/* Optional diagnostic label for the calling thread. */
void guest_cpu_set_thread_label(const char *name);
/* The guest's SSE and x87 control words (MXCSR, FCW) of this thread: get reads them (the defaults
 * before the thread ran guest code), set gives them to its guest code. A new thread starts with its
 * creator's, as FreeBSD's libthr does. */
void guest_cpu_get_fp_control(uint32_t *mxcsr, uint16_t *fcw);
void guest_cpu_set_fp_control(uint32_t mxcsr, uint16_t fcw);
/* Note a completed host memory write for opt-in JIT watch diagnostics. */
void guest_cpu_watch_write(uintptr_t address, size_t size);
/* The GS displacement of the loader's rewritten `mov rax, fs:[0]` (relocation kind 3). */
void guest_cpu_set_tls_displacement(uint32_t displacement);
/* Whether address is guest code. */
int guest_cpu_is_guest(uintptr_t address);
/* Decoded code over [address, address + size) is dropped (guest code patched at run time). */
void guest_cpu_invalidate(uintptr_t address, size_t size);

/* int3 in guest code (hooks patched into the game): the handler gets the guest's registers as
 * the hardware would after the int3 (rip past it), may change them, and returns 1 if it handled
 * the trap. */
typedef struct {
    uint64_t r[16]; /* rax rcx rdx rbx rsp rbp rsi rdi r8-r15 */
    uint64_t rip;
} GuestRegs;
/* Handlers are tried in the order they were set (at most four) until one handles the trap. */
void guest_cpu_set_trap_handler(int (*handler)(GuestRegs *regs));
/* In a trap handler: the rest of the thread's guest state, readable and writable until the
 * handler returns (rflags, MXCSR, ymm0-15 as 32 bytes each). Returns 0 outside a handler. */
typedef struct {
    uint64_t *flags;
    uint32_t *mxcsr;
    uint8_t (*ymm)[32];
} GuestTrapState;
int guest_cpu_trap_state(GuestTrapState *state);

/* The guest's time stamp counter (rdtsc): on x86-64 the host's, as guest code running natively
 * reads it; on arm64 a 1.6 GHz counter like the PS4's (GUEST_CPU_TSC_HZ). */
uint64_t guest_cpu_tsc(void);
#define GUEST_CPU_TSC_HZ UINT64_C(1600000000)

/* A fault in translated code that only needs an unaligned access emulated (arm64): handled. */
int guest_cpu_handle_alignment_fault(void *context);
/* Registers at a translated fault; rip is exact with GUEST_JIT_FAULT_MAP=1, otherwise zero.
 * Returns 0 outside translated code. Called only by a terminating fault handler. */
int guest_cpu_fault_guest_regs(void *context, GuestRegs *regs);
/* Describes a fault in translated code (guest block and registers); 0 when not one. */
int guest_cpu_describe_fault(void *context, char *out, size_t size);

/* Statistics and the guest location of this thread, for logs. */
uint64_t guest_cpu_guest_rip(void);
/* In a host import called from guest code: the guest return address (the game's call site,
 * past its call), read at the guest stack pointer; 0 when not known. */
uint64_t guest_cpu_import_return(void);
/* A return address read from the guest stack, as the guest's code left it: where the recompiled
 * library entered the translator, a frame's own address stands there instead of the caller's
 * (recomp.c), and this gives the caller's back. Any other value is returned as it is. */
uint64_t guest_cpu_guest_return(uint64_t value);
void guest_cpu_report(void);

#else
static inline int guest_cpu_enabled(void) { return 0; }
static inline int guest_cpu_is_guest(uintptr_t address) { (void)address; return 0; }
static inline void guest_cpu_add_guest_code(uintptr_t start, size_t size) { (void)start; (void)size; }
static inline void guest_cpu_recomp_exclude(uint64_t address, size_t size) { (void)address; (void)size; }
static inline void guest_cpu_invalidate(uintptr_t address, size_t size) { (void)address; (void)size; }
static inline uint64_t guest_cpu_call(uintptr_t fn, int count, const uint64_t *args) {
    (void)fn; (void)count; (void)args; return 0;
}
static inline void guest_cpu_set_thread_pointer(uintptr_t tcb) { (void)tcb; }
static inline void guest_cpu_set_thread_label(const char *name) { (void)name; }
static inline void guest_cpu_get_fp_control(uint32_t *mxcsr, uint16_t *fcw) { *mxcsr = 0x1f80; *fcw = 0x37f; }
static inline void guest_cpu_set_fp_control(uint32_t mxcsr, uint16_t fcw) { (void)mxcsr; (void)fcw; }
static inline void guest_cpu_watch_write(uintptr_t address, size_t size) { (void)address; (void)size; }
static inline uint64_t guest_cpu_tsc(void) { return 0; }
static inline uint64_t guest_cpu_import_return(void) { return 0; }
static inline uint64_t guest_cpu_guest_return(uint64_t value) { return value; }
static inline int guest_cpu_handle_alignment_fault(void *context) { (void)context; return 0; }
static inline int guest_cpu_describe_fault(void *context, char *out, size_t size) { (void)context; (void)out; (void)size; return 0; }
#define GUEST_CPU_TSC_HZ UINT64_C(1600000000)
typedef struct { uint64_t r[16]; uint64_t rip; } GuestRegs;
static inline int guest_cpu_fault_guest_regs(void *context, GuestRegs *regs) { (void)context; (void)regs; return 0; }
static inline void guest_cpu_set_trap_handler(int (*handler)(GuestRegs *regs)) { (void)handler; }
typedef struct { uint64_t *flags; uint32_t *mxcsr; uint8_t (*ymm)[32]; } GuestTrapState;
static inline int guest_cpu_trap_state(GuestTrapState *state) { (void)state; return 0; }
#endif

/* Convenience for host code calling guest function pointers. */
static inline uint64_t guest_call0(const void *fn) { return guest_cpu_call((uintptr_t)fn, 0, NULL); }
static inline uint64_t guest_call1(const void *fn, uint64_t a) {
    return guest_cpu_call((uintptr_t)fn, 1, &a);
}
static inline uint64_t guest_call2(const void *fn, uint64_t a, uint64_t b) {
    const uint64_t args[2] = {a, b};
    return guest_cpu_call((uintptr_t)fn, 2, args);
}
static inline uint64_t guest_call3(const void *fn, uint64_t a, uint64_t b, uint64_t c) {
    const uint64_t args[3] = {a, b, c};
    return guest_cpu_call((uintptr_t)fn, 3, args);
}
static inline uint64_t guest_call4(const void *fn, uint64_t a, uint64_t b, uint64_t c,
                                      uint64_t d) {
    const uint64_t args[4] = {a, b, c, d};
    return guest_cpu_call((uintptr_t)fn, 4, args);
}
static inline uint64_t guest_call5(const void *fn, uint64_t a, uint64_t b, uint64_t c,
                                      uint64_t d, uint64_t e) {
    const uint64_t args[5] = {a, b, c, d, e};
    return guest_cpu_call((uintptr_t)fn, 5, args);
}

#ifdef __cplusplus
}
#endif
#endif
