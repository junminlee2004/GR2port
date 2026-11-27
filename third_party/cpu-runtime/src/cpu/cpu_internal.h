/* guest-cpu internals: guest state, decoded instructions, helpers shared by the interpreter parts. */
#ifndef GUEST_CPU_INTERNAL_H
#define GUEST_CPU_INTERNAL_H
#include "guest_cpu.h"
#include <Zydis/Zydis.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>

/* General registers in x86 encoding order. */
enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };
/* RFLAGS bits. */
enum {
    F_CF = 1u << 0, F_PF = 1u << 2, F_AF = 1u << 4, F_ZF = 1u << 6, F_SF = 1u << 7,
    F_DF = 1u << 10, F_OF = 1u << 11,
    F_ARITH = F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF,
};

typedef union {
    uint8_t b[32];
    int8_t sb[32];
    uint16_t w[16];
    int16_t sw[16];
    uint32_t d[8];
    int32_t sd[8];
    uint64_t q[4];
    int64_t sq[4];
    float f[8];
    double fd[4];
} GuestVec;

enum { WATCH_RECORDS = 64 };
typedef struct { uint64_t tick, rip, address, value, sequence, reserved[3]; } GuestWatchRecord;
_Static_assert(sizeof(GuestWatchRecord) == 64 && WATCH_RECORDS == 64, "JIT watch ring addressing");
typedef struct GuestWatch {
    uint64_t count;
    GuestWatchRecord records[WATCH_RECORDS];
    struct GuestWatch *next;
    char name[32];
} GuestWatch;
void guest_cpu_watch_dump(void);

typedef struct GuestCpu {
    uint64_t r[16];
    uint64_t rip;
    uint64_t flags;
    GuestVec v[16];
    uint32_t mxcsr;
    uint64_t fs_base, gs_base;
    /* GS-relative slot the loader's rewritten TLS loads read: gs_base + displacement = &tcb. */
    uint64_t tcb;
    /* x87: values as doubles (the game uses x87 for few, plain computations). */
    double st[8];
    int top;
    uint16_t fcw, fsw;
    /* Nesting of host -> guest calls on this thread. */
    int depth;
    /* The guest stack's end (stack arguments of host calls are copied up to it). */
    uint64_t stack_top;
    /* JIT flag state (jit_arm64.c): bits 31-28 NZCV with C = !CF, the other bits RFLAGS's;
     * PF and AF from the last result (low byte parity) and its operands' xor (bit 4). */
    uint64_t jf_flags, jf_res, jf_ab;
    /* JIT: the direct branch of the block exit just taken (patched to the next block's code), and
     * this thread's indirect branch cache (guest rip -> translated code). */
    uint64_t jit_link;
    struct GuestJitCacheEntry *jit_cache;
    uint64_t instructions;
    GuestWatch *watch; /* allocated only for GUEST_JIT_WATCH diagnostics */
    /* Calls of recompiled functions not yet added to guest_cpu_recomp_calls (trace.h). Last: libraries
     * built before it read the fields above at the same offsets. */
    uint32_t recomp_pending;
    /* A nested call's state (run_nested, cpu.c): the calling one, which stays as it was, and its
     * RSP at the call (the nested call's stack is below). NULL for a thread's own state. */
    struct GuestCpu *outer;
    uint64_t outer_rsp;
} GuestCpu;

/* Operand of a decoded instruction (compact copy of Zydis's). */
enum { OP_NONE, OP_REG, OP_MEM, OP_IMM, OP_AGEN };
enum { RK_GPR, RK_VEC, RK_X87, RK_OTHER };
typedef struct {
    uint8_t type;
    uint8_t size;      /* bytes */
    uint8_t kind;      /* register kind */
    uint8_t reg;       /* GPR 0-15, vector 0-15, x87 0-7 */
    uint8_t high8;     /* AH, CH, DH, BH */
    uint8_t base, index, scale; /* memory: 0xff = none, base 0xfe = rip */
    uint8_t segment;   /* 0 none, 1 fs, 2 gs */
    int64_t disp;      /* memory displacement or immediate */
} GuestOp;

typedef struct GuestInsn {
    uint16_t mnemonic;    /* ZydisMnemonic */
    uint8_t length;
    uint8_t count;        /* visible operands */
    uint8_t opsize;       /* operand width, bytes */
    uint8_t vex;          /* VEX/EVEX encoded */
    uint8_t vl;           /* vector length, bytes (VEX) */
    uint8_t lock, rep, repne;
    uint8_t branch;       /* ends a block */
    uint16_t freads;      /* arithmetic flags read (RFLAGS bits) */
    uint16_t fwrites;     /* arithmetic flags written, defined or not */
    GuestOp op[5];
} GuestInsn;

/* A decoded run of instructions ending at a branch (or a size cap). */
typedef struct GuestBlock {
    uint64_t start;
    void *jit_code;        /* arm64 translation (jit_arm64.c), or null */
    uint8_t jit_failed;    /* its first instruction is interpreted */
    uint32_t count;
    struct GuestBlock *next;  /* hash chain */
    GuestInsn insn[];
} GuestBlock;

/* Stops guest execution of the current thread with a message (an unsupported instruction). */
_Noreturn void guest_cpu_fatal(GuestCpu *cpu, const GuestInsn *in, const char *what);
/* An int3's trap handlers for `cpu` (regs: its registers, rip past the int3); 0: none took it. */
int guest_cpu_trap(GuestCpu *cpu, GuestRegs *regs);
/* Host function call bridge (hostcall_<arch>.S): integer args, then 16-byte vector args. */
typedef struct {
    uint64_t gpr[8];      /* rdi rsi rdx rcx r8 r9, then the first two stack slots */
    uint64_t stack[16];   /* the guest's stack arguments */
    GuestVec xmm[8];         /* low 16 bytes used */
    uint64_t rax, rdx;
    GuestVec xmm0, xmm1;
} GuestHostArgs;
void guest_cpu_hostcall(const void *fn, GuestHostArgs *args);
/* The same call reading the arguments where they are: GuestCpu.r and .v, `slots` (at most 16)
 * stack arguments at `stack`; the results go to r[RAX], r[RDX], v[0] and v[1]. */
void guest_cpu_hostcall_regs(const void *fn, uint64_t *r, GuestVec *v, const uint64_t *stack, uint64_t slots);

/* Decode cache. */
const GuestBlock *guest_cpu_block(uint64_t rip);
int guest_cpu_is_guest_code(uint64_t address);

/* Interpreter: runs from cpu->rip until rip == stop (the sentinel return address). */
void guest_cpu_run(GuestCpu *cpu, uint64_t stop);
/* Called by guest_cpu_run when the guest reaches host code other than `stop` (recomp.c: the return
 * address of an outer frame of the runtime, after a longjmp or an exception; it does not return
 * then). NULL: none. */
extern void (*guest_cpu_return_hook)(GuestCpu *cpu);
/* Executes one instruction at cpu->rip; returns the next rip (interp.c). */
uint64_t guest_cpu_step(GuestCpu *cpu, const GuestInsn *in);
/* A host function reached at cpu->rip with its return address on the guest stack (interp.c). */
void guest_cpu_call_host_at_rip(GuestCpu *cpu);
GuestBlock *guest_cpu_block_uncached(uint64_t rip); /* decode.c: the caller frees it */
/* JIT (jit_arm64.c, arm64 hosts): runs like guest_cpu_run; 0 when unavailable (GUEST_JIT=0). */
int guest_cpu_jit_run(GuestCpu *cpu, uint64_t stop);
/* The guest TSC as virtual counter * num / den (arm64; interp.c). */
void guest_cpu_tsc_scale(uint64_t *num, uint64_t *den);
/* Whether the JIT translates the instruction itself (not through the interpreter). */
int guest_cpu_jit_translates(const GuestInsn *in, uint64_t rip);
/* Vector and x87 instructions (interp_vec.c, interp_x87.c); return 0 when not theirs. */
int guest_cpu_exec_vec(GuestCpu *cpu, const GuestInsn *in);
int guest_cpu_exec_x87(GuestCpu *cpu, const GuestInsn *in);

/* Operand helpers (interp.c). */
uint64_t guest_cpu_addr(GuestCpu *cpu, const GuestInsn *in, const GuestOp *op);
uint64_t guest_cpu_read(GuestCpu *cpu, const GuestInsn *in, const GuestOp *op);
void guest_cpu_write(GuestCpu *cpu, const GuestInsn *in, const GuestOp *op, uint64_t value);

/* Guest memory accesses. x86 orders stores after stores and loads after loads (TSO); arm64
 * does not, and the game publishes data between threads with plain stores (glyphs rendered by a
 * worker went missing). On arm64 guest loads acquire and stores release (guest_fence_* around the
 * accesses that cannot: unaligned and vector ones). */
#if defined(__aarch64__)
#define guest_fence_load() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define guest_fence_store() __atomic_thread_fence(__ATOMIC_RELEASE)
#else
#define guest_fence_load() ((void)0)
#define guest_fence_store() ((void)0)
#endif
static inline uint64_t guest_load(uint64_t address, int size) {
#if defined(__aarch64__)
    if (!(address & (uint64_t)(size - 1))) {
        switch (size) {
        case 1: return __atomic_load_n((const uint8_t *)address, __ATOMIC_ACQUIRE);
        case 2: return __atomic_load_n((const uint16_t *)address, __ATOMIC_ACQUIRE);
        case 4: return __atomic_load_n((const uint32_t *)address, __ATOMIC_ACQUIRE);
        default: return __atomic_load_n((const uint64_t *)address, __ATOMIC_ACQUIRE);
        }
    }
    uint64_t v = 0;
    memcpy(&v, (const void *)address, (size_t)size);
    guest_fence_load();
    return v;
#endif
    switch (size) {
    case 1: return *(const uint8_t *)address;
    case 2: { uint16_t v; memcpy(&v, (const void *)address, 2); return v; }
    case 4: { uint32_t v; memcpy(&v, (const void *)address, 4); return v; }
    default: { uint64_t v; memcpy(&v, (const void *)address, 8); return v; }
    }
}
static inline void guest_store(uint64_t address, int size, uint64_t value) {
#if defined(__aarch64__)
    if (!(address & (uint64_t)(size - 1))) {
        switch (size) {
        case 1: __atomic_store_n((uint8_t *)address, (uint8_t)value, __ATOMIC_RELEASE); return;
        case 2: __atomic_store_n((uint16_t *)address, (uint16_t)value, __ATOMIC_RELEASE); return;
        case 4: __atomic_store_n((uint32_t *)address, (uint32_t)value, __ATOMIC_RELEASE); return;
        default: __atomic_store_n((uint64_t *)address, value, __ATOMIC_RELEASE); return;
        }
    }
    guest_fence_store();
#endif
    switch (size) {
    case 1: *(uint8_t *)address = (uint8_t)value; break;
    case 2: { uint16_t v = (uint16_t)value; memcpy((void *)address, &v, 2); break; }
    case 4: { uint32_t v = (uint32_t)value; memcpy((void *)address, &v, 4); break; }
    default: memcpy((void *)address, &value, 8); break;
    }
}
static inline uint64_t guest_mask(int size) {
    return size >= 8 ? ~UINT64_C(0) : (UINT64_C(1) << (size * 8)) - 1;
}
static inline uint64_t guest_sign(int size) { return UINT64_C(1) << (size * 8 - 1); }
static inline int64_t guest_sext(uint64_t value, int size) {
    const int shift = 64 - size * 8;
    return shift ? (int64_t)(value << shift) >> shift : (int64_t)value;
}

_Static_assert(__builtin_offsetof(GuestHostArgs, stack) == 64, "hostcall.S layout");
_Static_assert(__builtin_offsetof(GuestHostArgs, xmm) == 192, "hostcall.S layout");
_Static_assert(__builtin_offsetof(GuestHostArgs, rax) == 448, "hostcall.S layout");
_Static_assert(__builtin_offsetof(GuestHostArgs, xmm0) == 464, "hostcall.S layout");
_Static_assert(__builtin_offsetof(GuestHostArgs, xmm1) == 496, "hostcall.S layout");

/* Sets this host thread's FPCR.FZ from the guest MXCSR (cpu.c). */
void guest_cpu_apply_host_fp(uint32_t mxcsr);

/* A guest CPU state of a host stack's own (guest_cpu_context_switch): the CPU, and the recompiled
 * code's frames and calls of setjmp (recomp.c). */
struct GuestContext {
    GuestCpu *cpu;
    /* The outermost state of cpu, which destroy frees: cpu is a nested call's (on the host stack)
     * when the switch came from one. */
    GuestCpu *own;
    void *frames, *resumes;
    uint32_t frame_depth, frame_capacity, resume_depth, resume_capacity;
};
/* A longjmp (or an exception) of a nested call's code that goes on at sp, in the stack of a call
 * outside it: the state moves out to that call's, which this thread runs from then on (cpu.c). */
GuestCpu *guest_cpu_leave_nested(GuestCpu *cpu, uint64_t sp);
/* Moves this thread's recompiled-code frames and calls of setjmp into save and load's into this
 * thread (recomp.c). */
void guest_cpu_recomp_switch_frames(struct GuestContext *save, struct GuestContext *load);

#endif
