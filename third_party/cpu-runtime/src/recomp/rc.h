/* The runtime interface of recompiled game code (bloodborne-recomp's docs/RECOMPILATION.md): what the C written by
 * tools/recomp/recompile.c includes, and what the program (the game, tools/recomp/replay.c) gives it.
 *
 * A recompiled function takes the guest CPU state at the function's entry (its return address on
 * top of the guest stack) and returns with cpu->rip at the address it returned to, as the guest
 * code does. Guest registers live in locals inside; the CPU flags are computed lazily (RcFlags):
 * clang drops what no instruction reads. */
#ifndef GUEST_RECOMP_RC_H
#define GUEST_RECOMP_RC_H
#include <setjmp.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#elif defined(__x86_64__)
#include <emmintrin.h>
#endif
#include "../cpu/cpu_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GUEST_RECOMP_API_VERSION 12u
#define GUEST_RECOMP_INIT "guest_recomp_init"
/* A library in the register convention (recompile --abi): its function at an image offset that
 * other libraries call as a_<offset>, or NULL (RcFind). */
#define GUEST_RECOMP_FIND "guest_recomp_find"

/* A call of setjmp in generated code (recompile: a call of an import stub named setjmp, _setjmp or
 * sigsetjmp). The code calls _setjmp(jump) there, then RcApi.resume, then the guest's setjmp. A
 * guest longjmp to it (the guest returning to `rip` with RSP at `rsp`, from any code) comes back to
 * `jump`, the guest's registers in the GuestCpu, and the code goes on after the call. `frames`: the
 * runtime's frame depth at the call; `at`: its place among the thread's live ones, ~0u for none. */
typedef struct {
    jmp_buf jump;
    uint64_t rip, rsp;
    uint32_t frames, at;
} RcResume;

typedef struct {
    uint32_t version;
    uint64_t image_base;
    /* Runs the guest instruction at cpu->rip (the interpreter); returns the next rip. */
    uint64_t (*step)(GuestCpu *cpu);
    /* A call of guest `target` returning to `next` (nothing pushed yet); cpu->rip is `next` after. */
    void (*call)(GuestCpu *cpu, uint64_t target, uint64_t next);
    /* A tail call of cpu->rip (the function's return address on top of the stack); cpu->rip is
     * where it returned to after. */
    void (*tail)(GuestCpu *cpu);
    /* The rest of the function from cpu->rip in the translator (its return address at entry_rsp);
     * cpu->rip is where it returned to after. */
    void (*bail)(GuestCpu *cpu, uint64_t entry_rsp);
    /* Code generated with GUEST_RECOMP_TRACE=1 calls this before each instruction (its image
     * offset, the guest registers): tools/recomp/replay.c --lockstep. NULL elsewhere. */
    void (*trace)(uint64_t offset, const uint64_t r[16], const GuestCpu *cpu);
    /* An instruction that stops the game (RcFault: ud2, int N, hlt, a privileged or undefined
     * one, a division by zero or overflow) at `rip`: reported as the interpreter reports it. */
    void (*fault)(GuestCpu *cpu, uint64_t rip, int kind) __attribute__((noreturn));
    /* int3: the trap handlers (guest_cpu_set_trap_handler) with the guest's registers, rip past it;
     * returns the rip they go on at. */
    uint64_t (*trap)(GuestCpu *cpu, uint64_t rip);
    /* ldmxcsr: the guest's MXCSR, given to the host's floating point too. */
    void (*set_mxcsr)(GuestCpu *cpu, uint32_t value);
    /* A hook point (recompile --hooks) whose bytes the program rewrote: runs the code now at
     * cpu->rip (a jump to the program's stub, a return) until it is back in the game's code outside
     * [cpu->rip, next); cpu->rip is where it went on. */
    void (*hook)(GuestCpu *cpu, uint64_t next);
    /* A call of setjmp (RcResume) returning to `next`: after _setjmp(resume->jump) returned 0, before
     * the guest's setjmp runs (RSP as before the call). */
    void (*resume)(GuestCpu *cpu, RcResume *resume, uint64_t next);
    /* The function with these `count` calls of setjmp returns: longjmps to them are over. */
    void (*leave)(RcResume *resume, int count);
    /* A call of the import stub at `stub` returning to `next`, or with `next` 0 its tail call (the
     * function's return address on top of the stack): what the stub's slot `slot` holds, at once
     * when that is the host function in `*host` (the call's own copy, which this fills). cpu->rip
     * is where it returned to after. */
    void (*import)(GuestCpu *cpu, uint64_t *host, uint64_t stub, uint64_t slot, uint64_t next);
    /* For an indirect call of `target` outside the calling library (its image at caller_base): the
     * function of another library in the register convention there, at offset target - caller_base
     * for the call's cache; else one at that offset whose check and slot are never NULL and that
     * has no a_<offset> (a host function, other code). NULL: none kept, the call goes on as for
     * rc_none. */
    const struct RcEntry *(*find)(uint64_t target, uint64_t caller_base);
} RcApi;

/* Why RcApi.fault stops the game. RC_FAULT_NOT_COMPILED: an instruction the generator has no C for
 * (recompile lists them). */
enum { RC_FAULT_UNDEFINED, RC_FAULT_INTERRUPT, RC_FAULT_PRIVILEGED, RC_FAULT_DIVIDE, RC_FAULT_NOT_COMPILED };

#ifndef GUEST_CPU_TRACE_H
typedef void (*RcFn)(GuestCpu *cpu);
#endif
/* Image bytes [offset, offset + size). */
typedef struct { uint64_t offset, size; } RcSpan;
/* A recompiled function: its image offset and size, and the hash of the code it was generated
 * from (guest_cpu_recomp_hash_parts). `direct`: the slot the library's own calls and tail calls of
 * it go through, which the program fills before the code runs: NULL where `fn` runs (the library
 * then calls the function straight), else what runs in its place (a version, a wrapper of it, or
 * for a function it does not run one that runs it as RcApi.call does). A call pushes its return
 * address and calls what the slot holds, with cpu->rip the function's address; a tail call calls
 * it the same way, the function's own return address on top of the stack.
 * A function the game's patches change has a version for each change, after the unpatched one
 * and with the same offset: `spans` are the patch's code outside the function that the version
 * runs (a code cave), hashed after the function's own bytes. `live` are the fields of its
 * instructions (immediates, displacements) that patches set to other values, and its hook points'
 * bytes (recompile --hooks): the code reads them from the image as it runs, and the hash takes
 * them as zeros. The program runs the version whose hash the loaded code has. RC_PATCH_CODE: code
 * only a patch writes (a function in a code cave), no version of it unpatched. */
enum { RC_PATCH_CODE = 1 };
typedef struct {
    uint64_t offset, size, hash;
    RcFn fn;
    RcFn *direct;
    const RcSpan *spans, *live;
    uint32_t span_count, live_count, flags;
} RcFunction;
/* The library's entry point: keeps `api`, returns its functions. */
typedef const RcFunction *(*RcInit)(const RcApi *api, size_t *count);

/* ---- for generated code ---- */

/* RAX and RDX as a function in the register convention gives them back (recompile --abi:
 * a_<offset>, RSP, RDI, RSI, RDX and RCX its arguments), and as the GuestCpu has them after
 * other code ran the rest of one (the translator, the function a slot holds). */
typedef struct { uint64_t rax, rdx; } RcRet;
static inline RcRet rc_back(const GuestCpu *cpu) { return (RcRet){cpu->r[RAX], cpu->r[RDX]}; }
/* A function of the library for its indirect calls: its offset, its slot (NULL while the function
 * runs as written), `check` (the slot, or for a function that holds a hook site one that is never
 * NULL: a call of it stores the callee-saved registers, which the hook's code reads) and
 * a_<offset>; the library's rc_find gives the one at an offset, each call keeps the last it called
 * (at first rc_none, at no offset). */
typedef RcRet (*RcAbi)(GuestCpu *c, uint64_t a4, uint64_t a7, uint64_t a6, uint64_t a2, uint64_t a1);
typedef struct RcEntry { uint64_t offset; RcFn *slot, *check; RcAbi abi; } RcEntry;
typedef const RcEntry *(*RcFind)(uint64_t offset);

/* Lazy flags: the last flag-setting operation and its operands, or FK_CPU (in cpu->flags).
 * FK_COMIS: comiss/ucomiss/comisd/ucomisd, the operands as doubles (rc_dbits). */
enum { FK_CPU, FK_ADD, FK_SUB, FK_LOGIC, FK_INC, FK_DEC, FK_COMIS };
static inline uint64_t rc_mask(int size) { return size >= 8 ? ~UINT64_C(0) : (UINT64_C(1) << (size * 8)) - 1; }
static inline uint64_t rc_dbits(double x) {
    uint64_t v;
    __builtin_memcpy(&v, &x, 8);
    return v;
}
static inline double rc_bitsd(uint64_t v) {
    double x;
    __builtin_memcpy(&x, &v, 8);
    return x;
}

/* comiss/ucomiss flags (the interpreter's comis). */
static inline uint64_t rc_comis(uint64_t f, double a, double b) {
    f &= ~(uint64_t)F_ARITH;
    if (a != a || b != b) f |= F_ZF | F_PF | F_CF;
    else if (a < b) f |= F_CF;
    else if (a == b) f |= F_ZF;
    return f;
}

/* RFLAGS from the lazy state (as the interpreter computes them). Inlined where clang would not in a
 * large function: the state is mostly constant at the call, and the call costs more than the
 * flags it folds to. */
__attribute__((always_inline)) static inline uint64_t rc_flags(int kind, uint64_t a, uint64_t b, uint64_t r,
                                                               int size, int saved_cf, uint64_t cpu_flags) {
    if (kind == FK_CPU) return cpu_flags;
    if (kind == FK_COMIS) return rc_comis(cpu_flags, rc_bitsd(a), rc_bitsd(b));
    const uint64_t m = rc_mask(size), sign = UINT64_C(1) << (size * 8 - 1);
    a &= m; b &= m; r &= m;
    uint64_t f = cpu_flags & ~(uint64_t)F_ARITH;
    int cf = 0, of = 0, af = 0;
    switch (kind) {
    case FK_ADD: cf = r < a; of = ((a ^ r) & (b ^ r) & sign) != 0; af = ((a ^ b ^ r) & 0x10) != 0; break;
    case FK_SUB: cf = a < b; of = ((a ^ b) & (a ^ r) & sign) != 0; af = ((a ^ b ^ r) & 0x10) != 0; break;
    case FK_INC: cf = saved_cf; of = r == sign; af = ((a ^ 1 ^ r) & 0x10) != 0; break;
    case FK_DEC: cf = saved_cf; of = r == sign - 1; af = ((a ^ 1 ^ r) & 0x10) != 0; break;
    default: break; /* FK_LOGIC: CF, OF, AF clear */
    }
    if (cf) f |= F_CF;
    if (of) f |= F_OF;
    if (af) f |= F_AF;
    if (!r) f |= F_ZF;
    if (r & sign) f |= F_SF;
    if (!__builtin_parity((unsigned)(r & 0xff))) f |= F_PF;
    return f;
}

/* OF of a + b (or a - b if `sub`) at `size` bytes. */
__attribute__((always_inline)) static inline int rc_signed_overflow(uint64_t a, uint64_t b, int size, int sub) {
    switch (size) {
    case 1: { int8_t t; return sub ? __builtin_sub_overflow((int8_t)a, (int8_t)b, &t) : __builtin_add_overflow((int8_t)a, (int8_t)b, &t); }
    case 2: { int16_t t; return sub ? __builtin_sub_overflow((int16_t)a, (int16_t)b, &t) : __builtin_add_overflow((int16_t)a, (int16_t)b, &t); }
    case 4: { int32_t t; return sub ? __builtin_sub_overflow((int32_t)a, (int32_t)b, &t) : __builtin_add_overflow((int32_t)a, (int32_t)b, &t); }
    default: { int64_t t; return sub ? __builtin_sub_overflow((int64_t)a, (int64_t)b, &t) : __builtin_add_overflow((int64_t)a, (int64_t)b, &t); }
    }
}

/* a < b (or a <= b if `or_equal`) at `size` bytes, as signed numbers if `is_signed`: in the C type of
 * that size, for a compare of that width. */
__attribute__((always_inline)) static inline int rc_less(uint64_t a, uint64_t b, int size, int is_signed, int or_equal) {
    switch (size) {
    case 1: return is_signed ? (or_equal ? (int8_t)a <= (int8_t)b : (int8_t)a < (int8_t)b)
                             : (or_equal ? (uint8_t)a <= (uint8_t)b : (uint8_t)a < (uint8_t)b);
    case 2: return is_signed ? (or_equal ? (int16_t)a <= (int16_t)b : (int16_t)a < (int16_t)b)
                             : (or_equal ? (uint16_t)a <= (uint16_t)b : (uint16_t)a < (uint16_t)b);
    case 4: return is_signed ? (or_equal ? (int32_t)a <= (int32_t)b : (int32_t)a < (int32_t)b)
                             : (or_equal ? (uint32_t)a <= (uint32_t)b : (uint32_t)a < (uint32_t)b);
    default: return is_signed ? (or_equal ? (int64_t)a <= (int64_t)b : (int64_t)a < (int64_t)b)
                              : (or_equal ? a <= b : a < b);
    }
}

/* Whether a + b at `size` bytes as signed numbers is negative before it wraps (SF != OF of the add). */
__attribute__((always_inline)) static inline int rc_add_negative(uint64_t a, uint64_t b, int size) {
    switch (size) {
    case 1: return (int8_t)a + (int8_t)b < 0;
    case 2: return (int16_t)a + (int16_t)b < 0;
    case 4: return (int64_t)(int32_t)a + (int32_t)b < 0;
    default: { int64_t t; return __builtin_add_overflow((int64_t)a, (int64_t)b, &t) ? (int64_t)a < 0 : t < 0; }
    }
}

/* Condition code cc (0 o, 1 no, 2 b, 3 nb, 4 z, ... 15 nle) of the lazy state (inlined as
 * rc_flags is). For an operation it is a comparison of the operands or the result (a < b signed
 * for jl after cmp), which compilers turn into the host's compare and branch: RFLAGS built from
 * them first and its bits tested is up to ten times the instructions. */
__attribute__((always_inline)) static inline int rc_cond(int cc, int kind, uint64_t a, uint64_t b, uint64_t r,
                                                         int size, int saved_cf, uint64_t cpu_flags) {
    if (kind == FK_ADD || kind == FK_SUB || kind == FK_LOGIC || kind == FK_INC || kind == FK_DEC) {
        const uint64_t m = rc_mask(size), sign = UINT64_C(1) << (size * 8 - 1), ones = ~UINT64_C(0);
        a &= m; b &= m; r &= m;
        const int sf = rc_less(r, 0, size, 1, 0);
        int v;
        switch (cc >> 1) {
        case 0: /* OF */
            v = kind == FK_ADD ? rc_signed_overflow(a, b, size, 0) : kind == FK_SUB ? rc_signed_overflow(a, b, size, 1)
                : kind == FK_INC ? r == sign : kind == FK_DEC ? r == sign - 1 : 0;
            break;
        case 1: /* CF */
            v = kind == FK_ADD ? rc_less(r, a, size, 0, 0) : kind == FK_SUB ? rc_less(a, b, size, 0, 0)
                : kind == FK_LOGIC ? 0 : saved_cf;
            break;
        case 2: v = kind == FK_SUB ? a == b : r == 0; break; /* ZF */
        case 3: /* CF or ZF */
            v = kind == FK_ADD ? rc_less(r, a, size, 0, 0) || r == 0 : kind == FK_SUB ? rc_less(a, b, size, 0, 1)
                : kind == FK_LOGIC ? r == 0 : saved_cf || r == 0;
            break;
        case 4: v = sf; break;                                        /* SF */
        case 5: v = !__builtin_parity((unsigned)(r & 0xff)); break; /* PF */
        case 6: /* SF != OF: the exact result is negative */
            v = kind == FK_ADD ? rc_add_negative(a, b, size) : kind == FK_SUB ? rc_less(a, b, size, 1, 0) : kind == FK_LOGIC ? sf
                : kind == FK_INC ? rc_less(a, ones, size, 1, 0) : rc_less(a, 1, size, 1, 0);
            break;
        default: /* ZF or SF != OF */
            v = kind == FK_ADD ? r == 0 || rc_add_negative(a, b, size) : kind == FK_SUB ? rc_less(a, b, size, 1, 1)
                : kind == FK_LOGIC ? rc_less(r, 0, size, 1, 1) : kind == FK_INC ? rc_less(a, ones, size, 1, 1)
                : rc_less(a, 1, size, 1, 1);
            break;
        }
        return (cc & 1) ? !v : v;
    }
    if (kind == FK_COMIS) { /* the comparison itself: SF and OF are clear */
        const double x = rc_bitsd(a), y = rc_bitsd(b);
        int v;
        switch (cc >> 1) {
        case 1: v = !(x >= y); break;              /* CF: below or unordered */
        case 2: case 7: v = !(x < y || x > y); break; /* ZF: equal or unordered */
        case 3: v = !(x > y); break;               /* CF or ZF */
        case 5: v = x != x || y != y; break;       /* PF: unordered */
        default: v = 0; break;                     /* OF, SF, SF != OF */
        }
        return (cc & 1) ? !v : v;
    }
    const uint64_t f = rc_flags(kind, a, b, r, size, saved_cf, cpu_flags);
    const int cf = !!(f & F_CF), zf = !!(f & F_ZF), sf = !!(f & F_SF), of = !!(f & F_OF), pf = !!(f & F_PF);
    int v;
    switch (cc >> 1) {
    case 0: v = of; break;
    case 1: v = cf; break;
    case 2: v = zf; break;
    case 3: v = cf || zf; break;
    case 4: v = sf; break;
    case 5: v = pf; break;
    case 6: v = sf != of; break;
    default: v = zf || sf != of; break;
    }
    return (cc & 1) ? !v : v;
}

/* Guest memory: x86 ordering between threads (guest_load/guest_store: acquire/release). The stack too:
 * other threads read and write structures on it (the game's jobs report into their creator's
 * stack), so plain accesses there let the compiler reorder or merge what another thread sees. */
static inline uint64_t rc_ld(uint64_t address, int size) { return guest_load(address, size); }
/* A field of an instruction that patches set (RcFunction.live), as the code holds it now. */
static inline uint64_t rc_code(uint64_t address, int size) {
    uint64_t v = 0;
    __builtin_memcpy(&v, (const void *)(uintptr_t)address, (size_t)size);
    return v;
}
static inline void rc_st(uint64_t address, int size, uint64_t value) { guest_store(address, size, value); }
static inline uint64_t rc_lds(uint64_t address, int size) { return guest_load(address, size); }
static inline void rc_sts(uint64_t address, int size, uint64_t value) { guest_store(address, size, value); }

/* One atomic compare-and-exchange (lock prefixes; the interpreter's atomic_cas): *seen gets the
 * value the operation found. */
static inline int rc_cas(uint64_t address, int size, uint64_t expected, uint64_t desired, uint64_t *seen) {
    int ok;
    switch (size) {
    case 1: { uint8_t e = (uint8_t)expected; ok = __atomic_compare_exchange_n((uint8_t *)address, &e, (uint8_t)desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); *seen = e; break; }
    case 2: { uint16_t e = (uint16_t)expected; ok = __atomic_compare_exchange_n((uint16_t *)address, &e, (uint16_t)desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); *seen = e; break; }
    case 4: { uint32_t e = (uint32_t)expected; ok = __atomic_compare_exchange_n((uint32_t *)address, &e, (uint32_t)desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); *seen = e; break; }
    default: { uint64_t e = expected; ok = __atomic_compare_exchange_n((uint64_t *)address, &e, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); *seen = e; break; }
    }
    return ok;
}

/* cmpxchg16b: rdx:rax (*hi:*lo) against the 16 bytes at `address`, rcx:rbx stored if equal;
 * otherwise *hi:*lo get what was there. 1: stored. */
static inline int rc_cas16(uint64_t address, uint64_t *lo, uint64_t *hi, uint64_t new_lo, uint64_t new_hi) {
#if defined(__x86_64__)
    unsigned char ok;
    __asm__ __volatile__("lock cmpxchg16b %1\n\tsete %0"
                         : "=q"(ok), "+m"(*(volatile unsigned __int128 *)(uintptr_t)address), "+a"(*lo), "+d"(*hi)
                         : "b"(new_lo), "c"(new_hi)
                         : "memory", "cc");
    return ok;
#else
    unsigned __int128 e = ((unsigned __int128)*hi << 64) | *lo;
    const int ok = __atomic_compare_exchange_n((unsigned __int128 *)(uintptr_t)address, &e,
                                               ((unsigned __int128)new_hi << 64) | new_lo, 0, __ATOMIC_SEQ_CST,
                                               __ATOMIC_SEQ_CST);
    *lo = (uint64_t)e;
    *hi = (uint64_t)(e >> 64);
    return ok;
#endif
}

/* SSE4.2 string compares (the interpreter's pcmpstr): element i of v in the format of imm's bits
 * 0-1, a string's length (to its first null element, or from rax/rdx for the explicit forms),
 * the aggregation of imm's bits 2-3 after the polarity of bits 4-5, and the mask form of a result
 * (bit 6: one element per bit, else the bits). */
static inline int64_t rc_pcmpstr_element(const GuestVec *v, int imm, int i) {
    switch (imm & 3) {
    case 0: return v->b[i];
    case 1: return v->w[i];
    case 2: return v->sb[i];
    default: return v->sw[i];
    }
}
static inline int rc_pcmpstr_length(const GuestVec *v, int imm, int count) {
    int length = 0;
    while (length < count && rc_pcmpstr_element(v, imm, length) != 0) ++length;
    return length;
}
static inline int rc_pcmpstr_explicit(int64_t value, int count) {
    return value > count || value < -count ? count : (int)(value < 0 ? -value : value);
}
static inline uint32_t rc_pcmpstr(const GuestVec *a, const GuestVec *b, int imm, int la, int lb) {
    const int count = imm & 1 ? 8 : 16;
    uint32_t result = 0;
    for (int j = 0; j < count; ++j) {
        int bit = 0;
        switch ((imm >> 2) & 3) {
        case 0:
            for (int i = 0; j < lb && i < la; ++i) bit |= rc_pcmpstr_element(a, imm, i) == rc_pcmpstr_element(b, imm, j);
            break;
        case 1:
            for (int i = 0; j < lb && i + 1 < la; i += 2) {
                const int64_t v = rc_pcmpstr_element(b, imm, j);
                bit |= rc_pcmpstr_element(a, imm, i) <= v && v <= rc_pcmpstr_element(a, imm, i + 1);
            }
            break;
        case 2:
            bit = j < la && j < lb ? rc_pcmpstr_element(a, imm, j) == rc_pcmpstr_element(b, imm, j) : j >= la && j >= lb;
            break;
        default:
            bit = 1;
            for (int i = 0; i < la && i + j < count; ++i) {
                if (i + j >= lb || rc_pcmpstr_element(a, imm, i) != rc_pcmpstr_element(b, imm, i + j)) {
                    bit = 0;
                    break;
                }
            }
            break;
        }
        result |= (uint32_t)bit << j;
    }
    switch ((imm >> 4) & 3) {
    case 1: result ^= (1u << count) - 1; break;
    case 3: result ^= (1u << lb) - 1; break;
    default: break;
    }
    return result;
}
static inline GuestVec rc_pcmpstr_mask(uint32_t result, int imm) {
    GuestVec mask;
    __builtin_memset(&mask, 0, sizeof(mask));
    const int count = imm & 1 ? 8 : 16;
    if (imm & 0x40) {
        for (int i = 0; i < count; ++i) {
            if (count == 8) mask.w[i] = (result >> i & 1) ? 0xffff : 0;
            else mask.b[i] = (result >> i & 1) ? 0xff : 0;
        }
    } else {
        mask.q[0] = result;
    }
    return mask;
}

/* x87 (the interpreter's interp_x87.c): values as doubles in cpu->st, ST(i) at (top + i) & 7;
 * 80-bit memory operands converted. */
static inline double *rc_x87(GuestCpu *c, int i) { return &c->st[(c->top + i) & 7]; }
static inline void rc_fpush(GuestCpu *c, double v) {
    c->top = (c->top - 1) & 7;
    *rc_x87(c, 0) = v;
}
static inline double rc_fpop(GuestCpu *c) {
    const double v = *rc_x87(c, 0);
    c->top = (c->top + 1) & 7;
    return v;
}
static inline double rc_load80(uint64_t address) {
    uint64_t mant;
    uint16_t se;
    __builtin_memcpy(&mant, (const void *)(uintptr_t)address, 8);
    __builtin_memcpy(&se, (const void *)(uintptr_t)(address + 8), 2);
    const int sign = se >> 15, exp = se & 0x7fff;
    double v;
    if (exp == 0 && mant == 0) v = 0.0;
    else if (exp == 0x7fff) v = (mant << 1) ? __builtin_nan("") : __builtin_inf();
    else v = __builtin_ldexp((double)mant, exp - 16383 - 63);
    return sign ? -v : v;
}
static inline void rc_store80(uint64_t address, double v) {
    uint64_t mant = 0;
    uint16_t se = __builtin_signbit(v) ? 0x8000 : 0;
    if (__builtin_isnan(v)) {
        mant = UINT64_C(0xC000000000000000);
        se |= 0x7fff;
    } else if (__builtin_isinf(v)) {
        mant = UINT64_C(0x8000000000000000);
        se |= 0x7fff;
    } else if (v != 0.0) {
        int exp;
        const double frac = __builtin_frexp(__builtin_fabs(v), &exp);
        mant = (uint64_t)__builtin_ldexp(frac, 64);
        se |= (uint16_t)(exp - 1 + 16383);
    }
    __builtin_memcpy((void *)(uintptr_t)address, &mant, 8);
    __builtin_memcpy((void *)(uintptr_t)(address + 8), &se, 2);
}
/* A memory operand of `size` bytes: an integer (fild, fiadd...) or a float, double or 80-bit value. */
static inline double rc_fmem(uint64_t address, int size, int integer) {
    if (integer) {
        const uint64_t v = guest_load(address, size);
        return size == 2 ? (double)(int16_t)v : size == 4 ? (double)(int32_t)v : (double)(int64_t)v;
    }
    if (size == 4) {
        float f;
        __builtin_memcpy(&f, (const void *)(uintptr_t)address, 4);
        return f;
    }
    if (size == 8) {
        double d;
        __builtin_memcpy(&d, (const void *)(uintptr_t)address, 8);
        return d;
    }
    return rc_load80(address);
}
static inline void rc_fstore(uint64_t address, int size, double v) {
    if (size == 4) {
        const float f = (float)v;
        __builtin_memcpy((void *)(uintptr_t)address, &f, 4);
    } else if (size == 8) {
        __builtin_memcpy((void *)(uintptr_t)address, &v, 8);
    } else {
        rc_store80(address, v);
    }
}
static inline int64_t rc_fint(const GuestCpu *c, double v, int truncate) {
    if (truncate) return (int64_t)v;
    switch ((c->fcw >> 10) & 3) {
    case 0: return (int64_t)__builtin_nearbyint(v);
    case 1: return (int64_t)__builtin_floor(v);
    case 2: return (int64_t)__builtin_ceil(v);
    default: return (int64_t)v;
    }
}
/* kind 0 add, 1 mul, 2 sub, 3 subr, 4 div, 5 divr. */
static inline double rc_farith(int kind, double a, double b) {
    switch (kind) {
    case 0: return a + b;
    case 1: return a * b;
    case 2: return a - b;
    case 3: return b - a;
    case 4: return a / b;
    default: return b / a;
    }
}
/* fcom: C0, C2, C3 of the status word; fcomi: CF, PF, ZF of `flags`. */
static inline void rc_fcom(GuestCpu *c, double a, double b) {
    uint16_t sw = c->fsw & ~0x4500;
    if (a != a || b != b) sw |= 0x4500;
    else if (a < b) sw |= 0x100;
    else if (a == b) sw |= 0x4000;
    c->fsw = sw;
}
static inline uint64_t rc_fcomi(uint64_t flags, double a, double b) {
    uint64_t f = flags & ~(uint64_t)F_ARITH;
    if (a != a || b != b) f |= F_ZF | F_PF | F_CF;
    else if (a < b) f |= F_CF;
    else if (a == b) f |= F_ZF;
    return f;
}
static inline uint16_t rc_fsw(const GuestCpu *c) { return (uint16_t)((c->fsw & ~0x3800) | ((c->top & 7) << 11)); }
static inline void rc_fxam(GuestCpu *c) {
    const double v = *rc_x87(c, 0);
    uint16_t sw = c->fsw & ~0x4700;
    if (__builtin_signbit(v)) sw |= 0x200;
    if (__builtin_isnan(v)) sw |= 0x100;
    else if (__builtin_isinf(v)) sw |= 0x500;
    else if (v == 0.0) sw |= 0x4000;
    else sw |= 0x400;
    c->fsw = sw;
}
/* The 28-byte protected-mode environment (all exceptions masked after fnstenv). */
static inline void rc_fnstenv(GuestCpu *c, uint64_t a) {
    __builtin_memset((void *)(uintptr_t)a, 0, 28);
    guest_store(a, 2, c->fcw);
    guest_store(a + 4, 2, rc_fsw(c));
    guest_store(a + 8, 2, 0);
    c->fcw |= 0x3f;
}
static inline void rc_fldenv(GuestCpu *c, uint64_t a) {
    c->fcw = (uint16_t)guest_load(a, 2);
    const uint16_t sw = (uint16_t)guest_load(a + 4, 2);
    c->fsw = sw & ~0x3800;
    c->top = (sw >> 11) & 7;
}

/* SZP of a result into flags f (the interpreter's set_szp). */
static inline uint64_t rc_szp(uint64_t f, uint64_t r, int size) {
    r &= rc_mask(size);
    f &= ~(uint64_t)(F_ZF | F_SF | F_PF);
    if (!r) f |= F_ZF;
    if (r & (UINT64_C(1) << (size * 8 - 1))) f |= F_SF;
    if (!__builtin_parity((unsigned)(r & 0xff))) f |= F_PF;
    return f;
}

/* Flags after adc (sub = 0) or sbb (sub = 1) of a and b with carry giving r (the interpreter's). */
static inline uint64_t rc_carry_flags(uint64_t f, int sub, uint64_t a, uint64_t b, uint64_t carry, uint64_t r, int size) {
    const uint64_t m = rc_mask(size), sign = UINT64_C(1) << (size * 8 - 1);
    a &= m; b &= m; r &= m;
    f &= ~(uint64_t)F_ARITH;
    if (sub ? (carry ? a <= b : a < b) : (carry ? r <= a : r < a)) f |= F_CF;
    if (sub ? ((a ^ b) & (a ^ r) & sign) : ((a ^ r) & (b ^ r) & sign)) f |= F_OF;
    if ((a ^ b ^ r) & 0x10) f |= F_AF;
    return rc_szp(f, r, size);
}

/* Float to integer conversions (the interpreter's cvt_f2i32/64): out of range, the "integer
 * indefinite"; rounding from mxcsr unless truncating. */
static inline double rc_round_mxcsr(double x, uint32_t mxcsr) {
    switch ((mxcsr >> 13) & 3) {
    case 0: return __builtin_nearbyint(x);
    case 1: return __builtin_floor(x);
    case 2: return __builtin_ceil(x);
    default: return x;
    }
}
static inline int32_t rc_f2i32(double x, int truncate, uint32_t mxcsr) {
#if defined(__x86_64__)
    /* cvttsd2si and cvtsd2si: the same indefinite, and the host rounds to nearest, mxcsr's default */
    if (truncate) return _mm_cvttsd_si32(_mm_set_sd(x));
    if (!(mxcsr & 0x6000)) return _mm_cvtsd_si32(_mm_set_sd(x));
#endif
    if (x != x || x >= 2147483648.0 || x < -2147483648.0) return INT32_MIN;
    return (int32_t)(truncate ? x : rc_round_mxcsr(x, mxcsr));
}
static inline int64_t rc_f2i64(double x, int truncate, uint32_t mxcsr) {
#if defined(__x86_64__)
    if (truncate) return _mm_cvttsd_si64(_mm_set_sd(x));
    if (!(mxcsr & 0x6000)) return _mm_cvtsd_si64(_mm_set_sd(x));
#endif
    if (x != x || x >= 9223372036854775808.0 || x < -9223372036854775808.0) return INT64_MIN;
    return (int64_t)(truncate ? x : rc_round_mxcsr(x, mxcsr));
}

/* Flags after shl/shr/sar of `a` by `count` (masked, non-zero) giving `r` (the interpreter's). */
static inline uint64_t rc_shift_flags(uint64_t f, int kind, uint64_t a, unsigned count, uint64_t r, int size) {
    const int bits = size * 8;
    const uint64_t sign = UINT64_C(1) << (bits - 1);
    int cf;
    f &= ~(uint64_t)(F_CF | F_OF);
    if (kind == 0) { /* shl */
        cf = count <= (unsigned)bits && ((a >> (bits - count)) & 1);
        if (((r & sign) != 0) != cf) f |= F_OF;
    } else if (kind == 1) { /* shr */
        cf = (int)((a >> (count - 1)) & 1);
        if (a & sign) f |= F_OF;
    } else { /* sar: a sign-extended */
        cf = (int)(((int64_t)a >> (count - 1)) & 1);
    }
    if (cf) f |= F_CF;
    return rc_szp(f, r, size);
}

/* Vector operands (the interpreter's vread/vwrite): memory loads and stores with x86 ordering;
 * a register destination gets `bytes`, VEX forms zeros above them. */
static inline GuestVec rc_vld(uint64_t address, int size) {
    GuestVec v;
    memset(&v, 0, sizeof(v));
    memcpy(v.b, (const void *)address, (size_t)size);
    guest_fence_load();
    return v;
}
static inline void rc_vst(uint64_t address, int size, GuestVec v) {
    guest_fence_store();
    memcpy((void *)address, v.b, (size_t)size);
}
/* pause: the host's spin-wait hint (pause on x86-64, yield on arm64), which leaves a spinning
 * thread's core to its other hardware thread as the instruction does natively. */
static inline void rc_pause(void) {
#if defined(__x86_64__)
    _mm_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#endif
}
/* The SQRT instructions' square root. __builtin_sqrtf keeps errno for a negative operand: a branch
 * to libm in every lane, which also stops clang from making the lanes one instruction. */
static inline float rc_sqrtf(float x) { return __builtin_elementwise_sqrt(x); }
static inline double rc_sqrt(double x) { return __builtin_elementwise_sqrt(x); }
/* rcpss/rsqrtss and rcpps/rsqrtps (over `bytes`, 16 or 32, of x): on x86-64 the host's
 * approximations, the values the game gets running natively there, three or four cycles where a
 * division takes ten or more; 1/x and 1/sqrt(x) elsewhere. */
static inline float rc_rcpf(float x) {
#if defined(__x86_64__)
    return _mm_cvtss_f32(_mm_rcp_ss(_mm_set_ss(x)));
#else
    return 1.0f / x;
#endif
}
static inline float rc_rsqrtf(float x) {
#if defined(__x86_64__)
    return _mm_cvtss_f32(_mm_rsqrt_ss(_mm_set_ss(x)));
#else
    return 1.0f / rc_sqrtf(x);
#endif
}
__attribute__((always_inline)) static inline GuestVec rc_rcpps(GuestVec x, int rsqrt, int bytes) {
    GuestVec r;
    memset(&r, 0, sizeof(r));
#if defined(__x86_64__)
    for (int h = 0; h < bytes; h += 16) {
        __m128 v;
        memcpy(&v, x.b + h, 16);
        v = rsqrt ? _mm_rsqrt_ps(v) : _mm_rcp_ps(v);
        memcpy(r.b + h, &v, 16);
    }
#else
    for (int i = 0; i < bytes / 4; ++i) r.f[i] = rsqrt ? 1.0f / rc_sqrtf(x.f[i]) : 1.0f / x.f[i];
#endif
    return r;
}
/* extrq: bits idx to idx + len - 1 of x's low quadword (len 0: 64), zero-extended, the upper
 * quadword zero; with constant idx and len a shift and a mask on x86-64. */
__attribute__((always_inline)) static inline GuestVec rc_extrq(GuestVec x, unsigned idx, unsigned len) {
    const uint64_t mk = len ? (1ull << len) - 1 : ~0ull;
    GuestVec r;
    memset(&r, 0, sizeof(r));
#if defined(__x86_64__)
    __m128i v;
    memcpy(&v, x.b, 16);
    v = _mm_and_si128(_mm_srl_epi64(v, _mm_cvtsi32_si128((int)idx)), _mm_set_epi64x(0, (long long)mk));
    memcpy(r.b, &v, 16);
#else
    r.q[0] = x.q[0] >> idx & mk;
#endif
    return r;
}
/* blendvps/blendvpd/pblendvb: the elements of b whose element of mk has its top bit set, the rest
 * of a (es: the element's bytes). On x86-64 the instructions themselves (SSE4.1, as roundps
 * below); elsewhere the masks from arithmetic shifts instead of a branch per lane. */
__attribute__((always_inline)) static inline GuestVec rc_blendv(GuestVec a, GuestVec b, GuestVec mk, int es) {
#if defined(__x86_64__)
    GuestVec r;
    for (int h = 0; h < 32; h += 16) {
        __m128i va, vb, vm;
        memcpy(&va, a.b + h, 16);
        memcpy(&vb, b.b + h, 16);
        memcpy(&vm, mk.b + h, 16);
        if (es == 4) __asm__("blendvps %2, %1, %0" : "+x"(va) : "x"(vb), "Yz"(vm));
        else if (es == 8) __asm__("blendvpd %2, %1, %0" : "+x"(va) : "x"(vb), "Yz"(vm));
        else __asm__("pblendvb %2, %1, %0" : "+x"(va) : "x"(vb), "Yz"(vm));
        memcpy(r.b + h, &va, 16);
    }
    return r;
#else
    typedef int8_t I1 __attribute__((vector_size(16)));
    typedef int32_t I4 __attribute__((vector_size(16)));
    typedef int64_t I8 __attribute__((vector_size(16)));
    GuestVec r;
    for (int h = 0; h < 32; h += 16) {
        I1 va, vb, sel;
        memcpy(&va, a.b + h, 16);
        memcpy(&vb, b.b + h, 16);
        if (es == 8) {
            I8 m;
            memcpy(&m, mk.b + h, 16);
            m >>= 63;
            memcpy(&sel, &m, 16);
        } else if (es == 4) {
            I4 m;
            memcpy(&m, mk.b + h, 16);
            m >>= 31;
            memcpy(&sel, &m, 16);
        } else {
            memcpy(&sel, mk.b + h, 16);
            sel >>= 7;
        }
        const I1 v = (vb & sel) | (va & ~sel);
        memcpy(r.b + h, &v, 16);
    }
    return r;
#endif
}
/* pshufb/vpshufb over `bytes` (16 or 32): each byte of b picks the byte of a's 16-byte half its low
 * four bits name, or zero where its top bit is set; zeros above `bytes`. One table lookup per half:
 * tbl on arm64 (an index of 16 or more gives zero), pshufb on x86-64, which compilers for the
 * baseline x86-64 (SSE2) do not emit, so in assembly. */
__attribute__((always_inline)) static inline GuestVec rc_pshufb(GuestVec a, GuestVec b, int bytes) {
    GuestVec r;
    memset(&r, 0, sizeof(r));
    for (int h = 0; h < bytes; h += 16) {
#if defined(__aarch64__)
        const uint8x16_t index = vandq_u8(vld1q_u8(b.b + h), vdupq_n_u8(0x8f));
        vst1q_u8(r.b + h, vqtbl1q_u8(vld1q_u8(a.b + h), index));
#elif defined(__x86_64__)
        typedef long long V2 __attribute__((vector_size(16)));
        V2 t, index;
        memcpy(&t, a.b + h, 16);
        memcpy(&index, b.b + h, 16);
        __asm__("pshufb %1, %0" : "+x"(t) : "x"(index));
        memcpy(r.b + h, &t, 16);
#else
        for (int i = 0; i < 16; ++i) r.b[h + i] = (b.b[h + i] & 0x80) ? 0 : a.b[h + (b.b[h + i] & 15)];
#endif
    }
    return r;
}
/* psllw/pslld/psllq (op 0), psrlw/psrld/psrlq (op 1) and psraw/psrad (op 2) over `bytes` (16 or 32)
 * of x, by the count in the low quadword of c, `lane` the element size (2, 4 or 8): zeros past the
 * element's width, copies of its sign bit for op 2. x86-64 has an instruction per half that takes
 * the count from a vector register; given the count as a value, compilers for the baseline x86-64
 * shift each element on its own. */
__attribute__((always_inline)) static inline GuestVec rc_vshift(GuestVec x, GuestVec c, int op, int lane, int bytes) {
    GuestVec r;
    memset(&r, 0, sizeof(r));
#if defined(__x86_64__)
    __m128i n;
    memcpy(&n, c.b, 16);
    for (int h = 0; h < bytes; h += 16) {
        __m128i v;
        memcpy(&v, x.b + h, 16);
        switch (op * 16 + lane) {
        case 0x02: v = _mm_sll_epi16(v, n); break;
        case 0x04: v = _mm_sll_epi32(v, n); break;
        case 0x08: v = _mm_sll_epi64(v, n); break;
        case 0x12: v = _mm_srl_epi16(v, n); break;
        case 0x14: v = _mm_srl_epi32(v, n); break;
        case 0x18: v = _mm_srl_epi64(v, n); break;
        case 0x22: v = _mm_sra_epi16(v, n); break;
        default: v = _mm_sra_epi32(v, n); break;
        }
        memcpy(r.b + h, &v, 16);
    }
#else
    const uint64_t n = c.q[0];
    switch (op * 16 + lane) {
    case 0x02: for (int i = 0; i < bytes / 2; ++i) r.w[i] = n > 15 ? 0 : (uint16_t)(x.w[i] << n); break;
    case 0x04: for (int i = 0; i < bytes / 4; ++i) r.d[i] = n > 31 ? 0 : x.d[i] << n; break;
    case 0x08: for (int i = 0; i < bytes / 8; ++i) r.q[i] = n > 63 ? 0 : x.q[i] << n; break;
    case 0x12: for (int i = 0; i < bytes / 2; ++i) r.w[i] = n > 15 ? 0 : (uint16_t)(x.w[i] >> n); break;
    case 0x14: for (int i = 0; i < bytes / 4; ++i) r.d[i] = n > 31 ? 0 : x.d[i] >> n; break;
    case 0x18: for (int i = 0; i < bytes / 8; ++i) r.q[i] = n > 63 ? 0 : x.q[i] >> n; break;
    case 0x22: for (int i = 0; i < bytes / 2; ++i) r.sw[i] = (int16_t)(x.sw[i] >> (n > 15 ? 15 : n)); break;
    default: for (int i = 0; i < bytes / 4; ++i) r.sd[i] = x.sd[i] >> (n > 31 ? 31 : n); break;
    }
#endif
    return r;
}
/* Generated code's vector registers: each 128-bit half a vector, which the compiler keeps in a SIMD
 * register. GuestVec, the GuestCpu's form, comes apart into scalars where code reads it lane by
 * lane, so the locals convert at their edges: rc_cv_get/rc_cv_put with the GuestCpu, rc_gv to a
 * GuestVec operand, rc_vset from a result. */
typedef uint32_t RcU4 __attribute__((vector_size(16)));
typedef struct { RcU4 lo, hi; } RcV;
static inline RcV rc_cv_get(const GuestVec *v) {
    RcV x;
    memcpy(&x.lo, v->b, 16);
    memcpy(&x.hi, v->b + 16, 16);
    return x;
}
static inline void rc_cv_put(GuestVec *v, RcV x) {
    memcpy(v->b, &x.lo, 16);
    memcpy(v->b + 16, &x.hi, 16);
}
static inline GuestVec rc_gv(RcV x) {
    GuestVec v;
    memcpy(v.b, &x.lo, 16);
    memcpy(v.b + 16, &x.hi, 16);
    return v;
}
/* Ones in the low `bytes` bytes (0 to 16). */
static inline RcU4 rc_low_bytes(int bytes) {
    static const uint8_t ones[32] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                     0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    RcU4 m;
    memcpy(&m, ones + 16 - bytes, 16);
    return m;
}
/* Writes `bytes` of v to register `reg` at d (VEX forms zero the rest) and marks it in the
 * function's mask of vector registers written since the entry or the last reload, which tells
 * the copies to the GuestCpu what to store on paths that do not all write it (recompile). */
static inline void rc_vset_reg(RcV *d, GuestVec v, int bytes, int vex, uint32_t *dirty, int reg) {
    RcU4 lo, hi;
    memcpy(&lo, v.b, 16);
    memcpy(&hi, v.b + 16, 16);
    *dirty |= 1u << reg;
    if (bytes >= 32) {
        d->lo = lo;
        d->hi = hi;
        return;
    }
    if (bytes < 16) {
        const RcU4 m = rc_low_bytes(bytes);
        lo = vex ? lo & m : (lo & m) | (d->lo & ~m);
    }
    d->lo = lo;
    if (vex) d->hi = (RcU4){0, 0, 0, 0};
}
/* In a generated function: its locals xmm[] and dv. */
#define rc_vset(d, v, bytes, vex) rc_vset_reg((d), (v), (bytes), (vex), &dv, (int)((d) - xmm))
static inline GuestVec rc_vgpr(uint64_t value) {
    GuestVec v;
    memset(&v, 0, sizeof(v));
    v.q[0] = value;
    return v;
}

/* cmpps predicates (the interpreter's fcompare). */
static inline int rc_fcompare(double a, double b, int predicate) {
    const int unordered = a != a || b != b;
    int r;
    switch (predicate & 7) {
    case 0: r = !unordered && a == b; break;
    case 1: r = !unordered && a < b; break;
    case 2: r = !unordered && a <= b; break;
    case 3: r = unordered; break;
    case 4: r = unordered || a != b; break;
    case 5: r = unordered || !(a < b); break;
    case 6: r = unordered || !(a <= b); break;
    default: r = !unordered; break;
    }
    if (predicate & 8) {
        switch (predicate & 7) {
        case 0: r = unordered || a == b; break;
        case 1: r = unordered || !(a >= b); break;
        case 2: r = unordered || !(a > b); break;
        case 3: r = 0; break;
        case 4: r = !unordered && a != b; break;
        case 5: r = !unordered && a >= b; break;
        case 6: r = !unordered && a > b; break;
        default: r = 1; break;
        }
    }
    return r;
}

/* roundss/roundps (the interpreter's round_mode): the mode from imm, or mxcsr with imm bit 2. An
 * x86-64 host rounds with roundss, roundsd, roundps and roundpd (SSE4.1, which every x86-64 host
 * this runs on has; the build's baseline leaves them out, and its libm calls for them clobber the
 * vector registers): RC_ROUND rounds x in place with mode m. */
#if defined(__x86_64__)
#define RC_ROUND(op, x, m) __asm__(op " %1, %0, %0" : "+x"(x) : "i"(m))
#define RC_ROUND_MODES(op, x, mode)                                                                \
    do {                                                                                           \
        switch (mode) {                                                                            \
        case 0: RC_ROUND(op, x, 0); break;                                                         \
        case 1: RC_ROUND(op, x, 1); break;                                                         \
        case 2: RC_ROUND(op, x, 2); break;                                                         \
        default: RC_ROUND(op, x, 3); break;                                                        \
        }                                                                                          \
    } while (0)
#endif
static inline double rc_round_mode(double x, int imm, uint32_t mxcsr) {
    const int mode = (imm & 4) ? (int)((mxcsr >> 13) & 3) : imm & 3;
#if defined(__x86_64__)
    RC_ROUND_MODES("roundsd", x, mode);
    return x;
#else
    switch (mode) {
    case 0: return __builtin_nearbyint(x);
    case 1: return __builtin_floor(x);
    case 2: return __builtin_ceil(x);
    default: return __builtin_trunc(x);
    }
#endif
}
static inline float rc_round_modef(float x, int imm, uint32_t mxcsr) {
    const int mode = (imm & 4) ? (int)((mxcsr >> 13) & 3) : imm & 3;
#if defined(__x86_64__)
    RC_ROUND_MODES("roundss", x, mode);
    return x;
#else
    switch (mode) {
    case 0: return __builtin_nearbyintf(x);
    case 1: return __builtin_floorf(x);
    case 2: return __builtin_ceilf(x);
    default: return __builtin_truncf(x);
    }
#endif
}
/* roundps, or roundpd if dbl, over `bytes` (16 or 32) of x; the rest zero. */
static inline GuestVec rc_round_packed(GuestVec x, int dbl, int bytes, int imm, uint32_t mxcsr) {
    GuestVec r;
    memset(&r, 0, sizeof(r));
#if defined(__x86_64__)
    const int mode = (imm & 4) ? (int)((mxcsr >> 13) & 3) : imm & 3;
    for (int h = 0; h < bytes; h += 16) {
        RcU4 v;
        memcpy(&v, x.b + h, 16);
        if (dbl) RC_ROUND_MODES("roundpd", v, mode);
        else RC_ROUND_MODES("roundps", v, mode);
        memcpy(r.b + h, &v, 16);
    }
#else
    for (int i = 0; i < bytes / (dbl ? 8 : 4); ++i) {
        if (dbl) r.fd[i] = rc_round_mode(x.fd[i], imm, mxcsr);
        else r.f[i] = rc_round_modef(x.f[i], imm, mxcsr);
    }
#endif
    return r;
}
/* cvttps2dq, or cvtps2dq unless truncating, over `bytes` (16 or 32) of x; the rest zero. */
static inline GuestVec rc_cvtps2dq(GuestVec x, int bytes, int truncate, uint32_t mxcsr) {
    GuestVec r;
    memset(&r, 0, sizeof(r));
#if defined(__x86_64__)
    if (truncate || !(mxcsr & 0x6000)) {
        for (int h = 0; h < bytes; h += 16) {
            __m128 v;
            memcpy(&v, x.b + h, 16);
            const __m128i q = truncate ? _mm_cvttps_epi32(v) : _mm_cvtps_epi32(v);
            memcpy(r.b + h, &q, 16);
        }
        return r;
    }
#endif
    for (int i = 0; i < bytes / 4; ++i) r.sd[i] = rc_f2i32(x.f[i], truncate, mxcsr);
    return r;
}

/* Saturations of the pack and saturating add/sub instructions (the interpreter's). */
static inline int16_t rc_sat16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }
static inline uint16_t rc_usat16(int32_t v) { return v > 65535 ? 65535 : v < 0 ? 0 : (uint16_t)v; }
static inline int8_t rc_sat8(int32_t v) { return v > 127 ? 127 : v < -128 ? -128 : (int8_t)v; }
static inline uint8_t rc_usat8(int32_t v) { return v > 255 ? 255 : v < 0 ? 0 : (uint8_t)v; }

/* rdtsc: the guest's counter, as guest_cpu_tsc reads it (interp.c): 1.6 GHz on arm64, the host's TSC
 * on x86-64. */
#if defined(__aarch64__)
static inline uint64_t rc_tsc(void) {
    static uint64_t num, den;
    if (!__atomic_load_n(&den, __ATOMIC_ACQUIRE)) {
        uint64_t hz;
        __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(hz));
        uint64_t x = GUEST_CPU_TSC_HZ, y = hz ? hz : 24000000;
        while (y) { const uint64_t r = x % y; x = y; y = r; }
        num = GUEST_CPU_TSC_HZ / x;
        __atomic_store_n(&den, (hz ? hz : 24000000) / x, __ATOMIC_RELEASE);
    }
    uint64_t counter;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(counter));
    return counter * num / __atomic_load_n(&den, __ATOMIC_ACQUIRE);
}
#else
static inline uint64_t rc_tsc(void) { return __builtin_ia32_rdtsc(); }
#endif

/* vcvtps2ph and vcvtph2ps (the interpreter's f32_to_f16 and f16_to_f32: round to nearest even). */
static inline uint16_t rc_f32_to_f16(float value) {
    uint32_t x;
    memcpy(&x, &value, 4);
    const uint32_t sign = (x >> 16) & 0x8000;
    int32_t exp = (int32_t)((x >> 23) & 0xff) - 127 + 15;
    uint32_t mant = x & 0x7fffff;
    if (((x >> 23) & 0xff) == 0xff) return (uint16_t)(sign | 0x7c00 | (mant ? 0x200 : 0));
    if (exp >= 31) return (uint16_t)(sign | 0x7c00);
    if (exp <= 0) {
        if (exp < -10) return (uint16_t)sign;
        mant |= 0x800000;
        const int shift = 14 - exp;
        uint32_t half = mant >> shift;
        const uint32_t rest = mant & ((1u << shift) - 1), mid = 1u << (shift - 1);
        if (rest > mid || (rest == mid && (half & 1))) ++half;
        return (uint16_t)(sign | half);
    }
    uint32_t half = ((uint32_t)exp << 10) | (mant >> 13);
    const uint32_t rest = mant & 0x1fff;
    if (rest > 0x1000 || (rest == 0x1000 && (half & 1))) ++half;
    return (uint16_t)(sign | half);
}
static inline float rc_f16_to_f32(uint16_t h) {
    const uint32_t sign = (uint32_t)(h & 0x8000) << 16;
    uint32_t exp = (h >> 10) & 0x1f, mant = h & 0x3ff, x;
    if (exp == 0) {
        if (!mant) x = sign;
        else {
            exp = 127 - 15 + 1;
            while (!(mant & 0x400)) { mant <<= 1; --exp; }
            x = sign | (exp << 23) | ((mant & 0x3ff) << 13);
        }
    } else if (exp == 31) x = sign | 0x7f800000 | (mant << 13);
    else x = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    float f;
    memcpy(&f, &x, 4);
    return f;
}

/* cpuid: the CPU the interpreter reports (its cpuid), from rax and rcx in r. */
static inline void rc_cpuid(uint64_t r[16]) {
    const uint32_t leaf = (uint32_t)r[RAX];
    uint32_t a = 0, b = 0, c = 0, d = 0;
    switch (leaf) {
    case 0: /* "AuthenticAMD", like the PS4's Jaguar */
        a = 0xd; b = 0x68747541; d = 0x69746e65; c = 0x444d4163; break;
    case 1: /* family 16h; SSE-SSE4.2, SSSE3, POPCNT, AVX, F16C, MOVBE, CX16, XSAVE/OSXSAVE */
        a = 0x00700f01;
        c = (1u << 0) | (1u << 1) | (1u << 9) | (1u << 13) | (1u << 19) | (1u << 20) |
            (1u << 22) | (1u << 23) | (1u << 26) | (1u << 27) | (1u << 28) | (1u << 29);
        d = (1u << 0) | (1u << 4) | (1u << 8) | (1u << 15) | (1u << 19) | (1u << 23) |
            (1u << 24) | (1u << 25) | (1u << 26);
        break;
    case 7: b = (1u << 3); break; /* BMI1 */
    case 0x80000000: a = 0x80000008; break;
    case 0x80000001: c = (1u << 0) | (1u << 5) | (1u << 6); d = (1u << 29) | (1u << 11); break;
    default: break;
    }
    r[RAX] = a; r[RBX] = b; r[RCX] = c; r[RDX] = d;
}

#ifdef __cplusplus
}
#endif
#endif
