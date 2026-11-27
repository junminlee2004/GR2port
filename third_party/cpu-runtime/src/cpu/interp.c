/* guest-cpu interpreter: integer instructions, control flow and the guest -> host call bridge.
 * Vector and x87 instructions are in interp_vec.c and interp_x87.c. Flags are computed eagerly. */
#include "cpu_internal.h"
#include "trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define M(name) ZYDIS_MNEMONIC_##name

/* ---- operands ---- */

uint64_t guest_cpu_addr(GuestCpu *cpu, const GuestInsn *in, const GuestOp *op) {
    uint64_t address = (uint64_t)op->disp;
    if (op->base == 0xfe) address += cpu->rip + in->length;
    else if (op->base != 0xff) address += cpu->r[op->base];
    if (op->index != 0xff) address += cpu->r[op->index] * op->scale;
    if (op->segment == 1) address += cpu->fs_base;
    else if (op->segment == 2) address += cpu->gs_base;
    return address;
}

static uint64_t read_gpr(const GuestCpu *cpu, const GuestOp *op, int size) {
    if (op->high8) return (cpu->r[op->reg] >> 8) & 0xff;
    return cpu->r[op->reg] & guest_mask(size);
}

static void write_gpr(GuestCpu *cpu, const GuestOp *op, int size, uint64_t value) {
    uint64_t *r = &cpu->r[op->reg];
    if (op->high8) *r = (*r & ~UINT64_C(0xff00)) | ((value & 0xff) << 8);
    else if (size == 8) *r = value;
    else if (size == 4) *r = (uint32_t)value; /* 32-bit writes zero the upper half */
    else *r = (*r & ~guest_mask(size)) | (value & guest_mask(size));
}

uint64_t guest_cpu_read(GuestCpu *cpu, const GuestInsn *in, const GuestOp *op) {
    switch (op->type) {
    case OP_REG:
        if (op->kind == RK_GPR) return read_gpr(cpu, op, op->size);
        if (op->kind == RK_VEC) return cpu->v[op->reg].q[0] & guest_mask(op->size);
        guest_cpu_fatal(cpu, in, "read of an unsupported register kind");
    case OP_MEM: return guest_load(guest_cpu_addr(cpu, in, op), op->size);
    case OP_AGEN: return guest_cpu_addr(cpu, in, op);
    case OP_IMM: return (uint64_t)op->disp & guest_mask(op->size ? op->size : 8);
    default: guest_cpu_fatal(cpu, in, "read of an empty operand");
    }
}

void guest_cpu_write(GuestCpu *cpu, const GuestInsn *in, const GuestOp *op, uint64_t value) {
    if (op->type == OP_REG && op->kind == RK_GPR) write_gpr(cpu, op, op->size, value);
    else if (op->type == OP_MEM) guest_store(guest_cpu_addr(cpu, in, op), op->size, value);
    else guest_cpu_fatal(cpu, in, "write to an unsupported operand");
}

/* Immediates are sign-extended to the destination size. */
static uint64_t read_src(GuestCpu *cpu, const GuestInsn *in, const GuestOp *op, int size) {
    if (op->type == OP_IMM) return (uint64_t)op->disp & guest_mask(size);
    return guest_cpu_read(cpu, in, op);
}

static void push(GuestCpu *cpu, uint64_t value) {
    cpu->r[RSP] -= 8;
    guest_store(cpu->r[RSP], 8, value);
}
static uint64_t pop(GuestCpu *cpu) {
    const uint64_t value = guest_load(cpu->r[RSP], 8);
    cpu->r[RSP] += 8;
    return value;
}

/* ---- flags ---- */

static void set_szp(GuestCpu *cpu, uint64_t result, int size) {
    result &= guest_mask(size);
    uint64_t f = cpu->flags & ~(uint64_t)(F_ZF | F_SF | F_PF);
    if (!result) f |= F_ZF;
    if (result & guest_sign(size)) f |= F_SF;
    if (!__builtin_parity((unsigned)(result & 0xff))) f |= F_PF;
    cpu->flags = f;
}

static void flags_add(GuestCpu *cpu, uint64_t a, uint64_t b, uint64_t carry, uint64_t r, int size) {
    const uint64_t m = guest_mask(size);
    a &= m; b &= m; r &= m;
    uint64_t f = cpu->flags & ~(uint64_t)F_ARITH;
    if (carry ? r <= a : r < a) f |= F_CF;
    if ((a ^ r) & (b ^ r) & guest_sign(size)) f |= F_OF;
    if ((a ^ b ^ r) & 0x10) f |= F_AF;
    cpu->flags = f;
    set_szp(cpu, r, size);
}

static void flags_sub(GuestCpu *cpu, uint64_t a, uint64_t b, uint64_t borrow, uint64_t r, int size) {
    const uint64_t m = guest_mask(size);
    a &= m; b &= m; r &= m;
    uint64_t f = cpu->flags & ~(uint64_t)F_ARITH;
    if (borrow ? a <= b : a < b) f |= F_CF;
    if ((a ^ b) & (a ^ r) & guest_sign(size)) f |= F_OF;
    if ((a ^ b ^ r) & 0x10) f |= F_AF;
    cpu->flags = f;
    set_szp(cpu, r, size);
}

static void flags_logic(GuestCpu *cpu, uint64_t r, int size) {
    cpu->flags &= ~(uint64_t)(F_CF | F_OF | F_AF);
    set_szp(cpu, r, size);
}

static int condition(const GuestCpu *cpu, int cc) {
    const uint64_t f = cpu->flags;
    const int cf = !!(f & F_CF), zf = !!(f & F_ZF), sf = !!(f & F_SF), of = !!(f & F_OF),
              pf = !!(f & F_PF);
    int r;
    switch (cc >> 1) {
    case 0: r = of; break;
    case 1: r = cf; break;
    case 2: r = zf; break;
    case 3: r = cf || zf; break;
    case 4: r = sf; break;
    case 5: r = pf; break;
    case 6: r = sf != of; break;
    default: r = zf || sf != of; break;
    }
    return (cc & 1) ? !r : r;
}

/* Condition code of jcc/setcc/cmovcc mnemonics (0 o, 1 no, 2 b, 3 nb, ... 15 nle), or -1. */
static int cc_of(int mnemonic) {
    static const int jcc[16] = {M(JO), M(JNO), M(JB), M(JNB), M(JZ), M(JNZ), M(JBE), M(JNBE),
                                M(JS), M(JNS), M(JP), M(JNP), M(JL), M(JNL), M(JLE), M(JNLE)};
    static const int setcc[16] = {M(SETO), M(SETNO), M(SETB), M(SETNB), M(SETZ), M(SETNZ),
                                  M(SETBE), M(SETNBE), M(SETS), M(SETNS), M(SETP), M(SETNP),
                                  M(SETL), M(SETNL), M(SETLE), M(SETNLE)};
    static const int cmovcc[16] = {M(CMOVO), M(CMOVNO), M(CMOVB), M(CMOVNB), M(CMOVZ),
                                   M(CMOVNZ), M(CMOVBE), M(CMOVNBE), M(CMOVS), M(CMOVNS),
                                   M(CMOVP), M(CMOVNP), M(CMOVL), M(CMOVNL), M(CMOVLE),
                                   M(CMOVNLE)};
    for (int i = 0; i < 16; ++i)
        if (jcc[i] == mnemonic || setcc[i] == mnemonic || cmovcc[i] == mnemonic) return i;
    return -1;
}
static int8_t cc_table[ZYDIS_MNEMONIC_MAX_VALUE + 1];
static int8_t kind_table[ZYDIS_MNEMONIC_MAX_VALUE + 1]; /* 1 jcc, 2 setcc, 3 cmovcc */
static void init_tables(void) {
    for (int m = 0; m <= ZYDIS_MNEMONIC_MAX_VALUE; ++m) {
        cc_table[m] = (int8_t)cc_of(m);
        if (cc_table[m] < 0) continue;
        const char *name = ZydisMnemonicGetString((ZydisMnemonic)m);
        kind_table[m] = name[0] == 'j' ? 1 : name[0] == 's' ? 2 : 3;
    }
}

/* ---- host calls ---- */

static void call_host(GuestCpu *cpu, uint64_t target, uint64_t stack_args) {
    /* Up to 16 stack slots, not past the end of the guest's stack (a thread's first frame sits
     * at the end of its mapping) nor, for stacks guest-cpu does not know, past the page. */
    uint64_t bytes = 128;
    if (cpu->stack_top > stack_args && cpu->stack_top - stack_args < bytes) bytes = cpu->stack_top - stack_args;
    else if (cpu->stack_top <= stack_args || cpu->stack_top - stack_args > (UINT64_C(1) << 26)) {
        const uint64_t page_left = 4096 - (stack_args & 4095);
        if (page_left < bytes) bytes = page_left;
    }
    guest_cpu_hostcall_regs((const void *)target, cpu->r, cpu->v, (const uint64_t *)stack_args, bytes / 8);
}

/* ---- the interpreter ---- */

/* int3 handlers, tried in the order they were set (the runtime's guest hooks, code hooks). */
enum { MAX_TRAP_HANDLERS = 4 };
static int (*trap_handlers[MAX_TRAP_HANDLERS])(GuestRegs *regs);
static int trap_handler_count;
void guest_cpu_set_trap_handler(int (*handler)(GuestRegs *regs)) {
    for (int i = 0; i < trap_handler_count; ++i)
        if (trap_handlers[i] == handler) return;
    if (trap_handler_count == MAX_TRAP_HANDLERS) {
        fputs("guest-cpu: too many trap handlers\n", stderr);
        abort();
    }
    trap_handlers[trap_handler_count++] = handler;
}
/* The thread's state while its trap handler runs (guest_cpu_trap_state). */
_Static_assert(sizeof(GuestVec) == 32, "guest_cpu_trap_state: ymm registers of 32 bytes");
static _Thread_local GuestCpu *trap_cpu;
int guest_cpu_trap(GuestCpu *cpu, GuestRegs *regs) {
    GuestCpu *const outer = trap_cpu;
    trap_cpu = cpu;
    int handled = 0;
    for (int i = 0; i < trap_handler_count && !handled; ++i) handled = trap_handlers[i](regs);
    trap_cpu = outer;
    return handled;
}
int guest_cpu_trap_state(GuestTrapState *state) {
    if (!trap_cpu) return 0;
    state->flags = &trap_cpu->flags;
    state->mxcsr = &trap_cpu->mxcsr;
    state->ymm = (uint8_t (*)[32])(void *)trap_cpu->v;
    return 1;
}

/* On arm64 a 1.6 GHz counter, like the PS4's TSC (GUEST_CPU_TSC_HZ): the virtual counter scaled by
 * num / den, which translated rdtsc computes the same way (jit_arm64.c). On x86-64 the host's
 * TSC, as guest code running natively reads it and the host's kernel library reports it (a clock
 * read costs a system call where the host's clock source is not the TSC). */
#if defined(__aarch64__)
static uint64_t tsc_num, tsc_den;
void guest_cpu_tsc_scale(uint64_t *num, uint64_t *den) {
    if (!__atomic_load_n(&tsc_den, __ATOMIC_ACQUIRE)) {
        uint64_t hz;
        __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(hz));
        uint64_t x = GUEST_CPU_TSC_HZ, y = hz ? hz : 24000000;
        while (y) { const uint64_t r = x % y; x = y; y = r; }
        tsc_num = GUEST_CPU_TSC_HZ / x;
        __atomic_store_n(&tsc_den, (hz ? hz : 24000000) / x, __ATOMIC_RELEASE);
    }
    *num = tsc_num;
    *den = __atomic_load_n(&tsc_den, __ATOMIC_ACQUIRE);
}
uint64_t guest_cpu_tsc(void) {
    uint64_t num, den, counter;
    guest_cpu_tsc_scale(&num, &den);
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(counter));
    return counter * num / den;
}
#else
uint64_t guest_cpu_tsc(void) { return __builtin_ia32_rdtsc(); }
#endif
static uint64_t rdtsc(void) { return guest_cpu_tsc(); }

static void cpuid(GuestCpu *cpu) {
    const uint32_t leaf = (uint32_t)cpu->r[RAX], sub = (uint32_t)cpu->r[RCX];
    uint32_t a = 0, b = 0, c = 0, d = 0;
    (void)sub;
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
    cpu->r[RAX] = a; cpu->r[RBX] = b; cpu->r[RCX] = c; cpu->r[RDX] = d;
}

static int string_size(int mnemonic) {
    switch (mnemonic) {
    case M(MOVSB): case M(STOSB): case M(LODSB): case M(CMPSB): case M(SCASB): return 1;
    case M(MOVSW): case M(STOSW): case M(LODSW): case M(CMPSW): case M(SCASW): return 2;
    case M(MOVSD): case M(STOSD): case M(LODSD): case M(CMPSD): case M(SCASD): return 4;
    default: return 8;
    }
}

/* movs/stos/lods/cmps/scas with optional rep prefixes. */
static void string_op(GuestCpu *cpu, const GuestInsn *in) {
    const int m = in->mnemonic, size = string_size(m);
    const int64_t step = (cpu->flags & F_DF) ? -size : size;
    const int repeat = in->rep || in->repne;
    const int compares = m == M(CMPSB) || m == M(CMPSW) || m == M(CMPSD) || m == M(CMPSQ) ||
                         m == M(SCASB) || m == M(SCASW) || m == M(SCASD) || m == M(SCASQ);
    for (;;) {
        if (repeat && cpu->r[RCX] == 0) break;
        switch (m) {
        case M(MOVSB): case M(MOVSW): case M(MOVSD): case M(MOVSQ):
            if (repeat && step > 0 && size == 1 && cpu->r[RCX] > 16) {
                /* Fast path: forward byte copy (memmove semantics match only without overlap
                 * hazards, which forward rep movsb shares with memcpy for dst < src or apart). */
                const uint64_t n = cpu->r[RCX];
                const uint64_t s = cpu->r[RSI], d = cpu->r[RDI];
                if (d + n <= s || s + n <= d || d < s) {
                    guest_fence_store();
                    memmove((void *)d, (const void *)s, n);
                    guest_fence_load();
                    cpu->r[RSI] += n; cpu->r[RDI] += n; cpu->r[RCX] = 0;
                    return;
                }
            }
            guest_store(cpu->r[RDI], size, guest_load(cpu->r[RSI], size));
            cpu->r[RSI] += step; cpu->r[RDI] += step;
            break;
        case M(STOSB): case M(STOSW): case M(STOSD): case M(STOSQ):
            if (repeat && step > 0 && size == 1 && cpu->r[RCX] > 16) {
                guest_fence_store();
                memset((void *)cpu->r[RDI], (int)(cpu->r[RAX] & 0xff), cpu->r[RCX]);
                cpu->r[RDI] += cpu->r[RCX]; cpu->r[RCX] = 0;
                return;
            }
            guest_store(cpu->r[RDI], size, cpu->r[RAX]);
            cpu->r[RDI] += step;
            break;
        case M(LODSB): case M(LODSW): case M(LODSD): case M(LODSQ): {
            GuestOp rax = {.type = OP_REG, .kind = RK_GPR, .reg = RAX, .size = (uint8_t)size};
            write_gpr(cpu, &rax, size, guest_load(cpu->r[RSI], size));
            cpu->r[RSI] += step;
            break;
        }
        case M(CMPSB): case M(CMPSW): case M(CMPSD): case M(CMPSQ): {
            const uint64_t a = guest_load(cpu->r[RSI], size), b = guest_load(cpu->r[RDI], size);
            flags_sub(cpu, a, b, 0, a - b, size);
            cpu->r[RSI] += step; cpu->r[RDI] += step;
            break;
        }
        default: { /* scas */
            const uint64_t a = cpu->r[RAX] & guest_mask(size), b = guest_load(cpu->r[RDI], size);
            flags_sub(cpu, a, b, 0, a - b, size);
            cpu->r[RDI] += step;
            break;
        }
        }
        if (!repeat) break;
        --cpu->r[RCX];
        if (compares && in->rep && !(cpu->flags & F_ZF)) break;
        if (compares && in->repne && (cpu->flags & F_ZF)) break;
    }
}

static int is_string(const GuestInsn *in) {
    switch (in->mnemonic) {
    case M(MOVSB): case M(MOVSW): case M(MOVSQ): case M(STOSB): case M(STOSW): case M(STOSD):
    case M(STOSQ): case M(LODSB): case M(LODSW): case M(LODSD): case M(LODSQ): case M(CMPSB):
    case M(CMPSW): case M(CMPSQ): case M(SCASB): case M(SCASW): case M(SCASD): case M(SCASQ):
        return 1;
    case M(MOVSD): case M(CMPSD): /* also SSE instructions: the string forms have no XMM operand */
        return in->count == 0 || !(in->op[0].type == OP_REG && in->op[0].kind == RK_VEC);
    default:
        return 0;
    }
}

static void shift(GuestCpu *cpu, const GuestInsn *in) {
    const GuestOp *dst = &in->op[0];
    const int size = dst->size, bits = size * 8;
    const unsigned count_mask = size == 8 ? 63 : 31;
    unsigned count = in->count > 1 ? (unsigned)read_src(cpu, in, &in->op[1], 1) : 1;
    count &= count_mask;
    const uint64_t m = guest_mask(size);
    uint64_t a = guest_cpu_read(cpu, in, dst) & m, r = a;
    if (!count) return;
    uint64_t f = cpu->flags;
    switch (in->mnemonic) {
    case M(SHL): {
        r = count >= (unsigned)bits ? 0 : (a << count) & m;
        const int cf = count <= (unsigned)bits && ((a >> (bits - count)) & 1);
        f = (f & ~(uint64_t)(F_CF | F_OF)) | (cf ? F_CF : 0);
        if (((r & guest_sign(size)) != 0) != cf) f |= F_OF;
        cpu->flags = f;
        set_szp(cpu, r, size);
        break;
    }
    case M(SHR): {
        r = count >= (unsigned)bits ? 0 : a >> count;
        const int cf = (a >> (count - 1)) & 1;
        f = (f & ~(uint64_t)(F_CF | F_OF)) | (cf ? F_CF : 0);
        if (a & guest_sign(size)) f |= F_OF;
        cpu->flags = f;
        set_szp(cpu, r, size);
        break;
    }
    case M(SAR): {
        const int64_t s = guest_sext(a, size);
        r = (uint64_t)(s >> (count >= (unsigned)bits ? bits - 1 : count)) & m;
        const int cf = (int)((s >> (count - 1)) & 1);
        f = (f & ~(uint64_t)(F_CF | F_OF)) | (cf ? F_CF : 0);
        cpu->flags = f;
        set_szp(cpu, r, size);
        break;
    }
    case M(ROL): {
        const unsigned c = count % bits;
        r = c ? ((a << c) | (a >> (bits - c))) & m : a;
        const int cf = r & 1;
        f = (f & ~(uint64_t)(F_CF | F_OF)) | (cf ? F_CF : 0);
        if (((r & guest_sign(size)) != 0) != cf) f |= F_OF;
        cpu->flags = f;
        break;
    }
    case M(ROR): {
        const unsigned c = count % bits;
        r = c ? ((a >> c) | (a << (bits - c))) & m : a;
        const int msb = (r & guest_sign(size)) != 0, msb2 = (r & (guest_sign(size) >> 1)) != 0;
        f = (f & ~(uint64_t)(F_CF | F_OF)) | (msb ? F_CF : 0) | (msb != msb2 ? F_OF : 0);
        cpu->flags = f;
        break;
    }
    case M(RCL): case M(RCR): {
        const unsigned c = count % (bits + 1);
        uint64_t cf = (f & F_CF) ? 1 : 0;
        for (unsigned i = 0; i < c; ++i) {
            if (in->mnemonic == M(RCL)) {
                const uint64_t out = (r >> (bits - 1)) & 1;
                r = ((r << 1) | cf) & m;
                cf = out;
            } else {
                const uint64_t out = r & 1;
                r = (r >> 1) | (cf << (bits - 1));
                cf = out;
            }
        }
        f = (f & ~(uint64_t)(F_CF | F_OF)) | (cf ? F_CF : 0);
        if (in->mnemonic == M(RCL)) { if (((r & guest_sign(size)) != 0) != (int)cf) f |= F_OF; }
        else if (((r ^ (r << 1)) & guest_sign(size)) != 0) f |= F_OF;
        cpu->flags = f;
        break;
    }
    default: guest_cpu_fatal(cpu, in, "shift");
    }
    guest_cpu_write(cpu, in, dst, r);
}

static void double_shift(GuestCpu *cpu, const GuestInsn *in) {
    const GuestOp *dst = &in->op[0];
    const int size = dst->size, bits = size * 8;
    unsigned count = (unsigned)read_src(cpu, in, &in->op[2], 1) & (size == 8 ? 63 : 31);
    if (!count) return;
    const uint64_t m = guest_mask(size);
    const uint64_t a = guest_cpu_read(cpu, in, dst) & m, b = guest_cpu_read(cpu, in, &in->op[1]) & m;
    uint64_t r;
    int cf;
    if (in->mnemonic == M(SHLD)) {
        r = ((a << count) | (count == (unsigned)bits ? b : b >> (bits - count))) & m;
        cf = (a >> (bits - count)) & 1;
    } else {
        r = ((a >> count) | (b << (bits - count))) & m;
        cf = (a >> (count - 1)) & 1;
    }
    uint64_t f = (cpu->flags & ~(uint64_t)(F_CF | F_OF)) | (cf ? F_CF : 0);
    if (((r ^ a) & guest_sign(size)) != 0) f |= F_OF;
    cpu->flags = f;
    set_szp(cpu, r, size);
    guest_cpu_write(cpu, in, dst, r);
}

static void multiply(GuestCpu *cpu, const GuestInsn *in) {
    const int m = in->mnemonic;
    if (m == M(IMUL) && in->count >= 2) {
        const GuestOp *dst = &in->op[0];
        const int size = dst->size;
        const int64_t a = guest_sext(in->count == 3 ? guest_cpu_read(cpu, in, &in->op[1])
                                                  : guest_cpu_read(cpu, in, dst), size);
        const int64_t b = in->count == 3 ? in->op[2].disp
                                         : guest_sext(guest_cpu_read(cpu, in, &in->op[1]), size);
        __int128 full = (__int128)a * b;
        const uint64_t r = (uint64_t)full & guest_mask(size);
        const int overflow = (__int128)guest_sext(r, size) != full;
        cpu->flags = (cpu->flags & ~(uint64_t)(F_CF | F_OF)) | (overflow ? F_CF | F_OF : 0);
        set_szp(cpu, r, size);
        guest_cpu_write(cpu, in, dst, r);
        return;
    }
    const int size = in->op[0].size;
    const uint64_t src = guest_cpu_read(cpu, in, &in->op[0]);
    const uint64_t a = cpu->r[RAX] & guest_mask(size);
    uint64_t lo, hi;
    int overflow;
    if (m == M(MUL)) {
        const unsigned __int128 full = (unsigned __int128)a * src;
        lo = (uint64_t)full & guest_mask(size);
        hi = size == 8 ? (uint64_t)(full >> 64) : (uint64_t)(full >> (size * 8)) & guest_mask(size);
        overflow = hi != 0;
    } else {
        const __int128 full = (__int128)guest_sext(a, size) * guest_sext(src, size);
        lo = (uint64_t)full & guest_mask(size);
        hi = size == 8 ? (uint64_t)((unsigned __int128)full >> 64)
                       : (uint64_t)(full >> (size * 8)) & guest_mask(size);
        overflow = (__int128)guest_sext(lo, size) != full;
    }
    if (size == 1) {
        cpu->r[RAX] = (cpu->r[RAX] & ~UINT64_C(0xffff)) | lo | (hi << 8);
    } else {
        GuestOp rax = {.type = OP_REG, .kind = RK_GPR, .reg = RAX, .size = (uint8_t)size};
        GuestOp rdx = {.type = OP_REG, .kind = RK_GPR, .reg = RDX, .size = (uint8_t)size};
        write_gpr(cpu, &rax, size, lo);
        write_gpr(cpu, &rdx, size, hi);
    }
    cpu->flags = (cpu->flags & ~(uint64_t)(F_CF | F_OF)) | (overflow ? F_CF | F_OF : 0);
}

static void divide(GuestCpu *cpu, const GuestInsn *in) {
    const int size = in->op[0].size;
    const uint64_t divisor = guest_cpu_read(cpu, in, &in->op[0]);
    if (!divisor) guest_cpu_fatal(cpu, in, "division by zero");
    if (in->mnemonic == M(DIV)) {
        unsigned __int128 dividend;
        if (size == 1) dividend = cpu->r[RAX] & 0xffff;
        else dividend = ((unsigned __int128)(cpu->r[RDX] & guest_mask(size)) << (size * 8)) |
                        (cpu->r[RAX] & guest_mask(size));
        const unsigned __int128 q = dividend / divisor, rem = dividend % divisor;
        if (q > guest_mask(size)) guest_cpu_fatal(cpu, in, "division overflow");
        if (size == 1) {
            cpu->r[RAX] = (cpu->r[RAX] & ~UINT64_C(0xffff)) | (uint64_t)q | ((uint64_t)rem << 8);
        } else {
            GuestOp rax = {.type = OP_REG, .kind = RK_GPR, .reg = RAX, .size = (uint8_t)size};
            GuestOp rdx = {.type = OP_REG, .kind = RK_GPR, .reg = RDX, .size = (uint8_t)size};
            write_gpr(cpu, &rax, size, (uint64_t)q);
            write_gpr(cpu, &rdx, size, (uint64_t)rem);
        }
    } else {
        __int128 dividend;
        if (size == 1) dividend = (int16_t)(cpu->r[RAX] & 0xffff);
        else dividend = (__int128)(((unsigned __int128)(cpu->r[RDX] & guest_mask(size)) << (size * 8)) |
                                   (cpu->r[RAX] & guest_mask(size)));
        if (size < 8) dividend = (__int128)(dividend << (128 - size * 16)) >> (128 - size * 16);
        const int64_t d = guest_sext(divisor, size);
        const __int128 q = dividend / d, rem = dividend % d;
        if (q != (__int128)guest_sext((uint64_t)q, size)) guest_cpu_fatal(cpu, in, "division overflow");
        if (size == 1) {
            cpu->r[RAX] = (cpu->r[RAX] & ~UINT64_C(0xffff)) | ((uint64_t)q & 0xff) |
                          (((uint64_t)rem & 0xff) << 8);
        } else {
            GuestOp rax = {.type = OP_REG, .kind = RK_GPR, .reg = RAX, .size = (uint8_t)size};
            GuestOp rdx = {.type = OP_REG, .kind = RK_GPR, .reg = RDX, .size = (uint8_t)size};
            write_gpr(cpu, &rax, size, (uint64_t)q);
            write_gpr(cpu, &rdx, size, (uint64_t)rem);
        }
    }
}

/* Locked (or implicitly locked) read-modify-write on memory: atomic compare-exchange loop. */
#define ATOMIC_RMW(size, address, old, expr)                                                      \
    do {                                                                                          \
        for (;;) {                                                                                \
            old = guest_load(address, size);                                                         \
            const uint64_t new_value_ = (expr);                                                   \
            uint64_t seen_;                                                                       \
            if (atomic_cas(address, size, old, new_value_, &seen_)) break;                      \
        }                                                                                         \
    } while (0)

/* One atomic compare-and-exchange; *seen gets the value the operation found (x86 cmpxchg's rax). */
static int atomic_cas(uint64_t address, int size, uint64_t expected, uint64_t desired, uint64_t *seen) {
    int ok;
    switch (size) {
    case 1: { uint8_t e = (uint8_t)expected; ok = __atomic_compare_exchange_n((uint8_t *)address, &e, (uint8_t)desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); *seen = e; break; }
    case 2: { uint16_t e = (uint16_t)expected; ok = __atomic_compare_exchange_n((uint16_t *)address, &e, (uint16_t)desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); *seen = e; break; }
    case 4: { uint32_t e = (uint32_t)expected; ok = __atomic_compare_exchange_n((uint32_t *)address, &e, (uint32_t)desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); *seen = e; break; }
    default: { uint64_t e = expected; ok = __atomic_compare_exchange_n((uint64_t *)address, &e, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); *seen = e; break; }
    }
    return ok;
}
/* add/or/adc/sbb/and/sub/xor/cmp/test and their locked forms. */
static void alu(GuestCpu *cpu, const GuestInsn *in) {
    const GuestOp *dst = &in->op[0];
    const int size = dst->size, m = in->mnemonic;
    const uint64_t b = read_src(cpu, in, &in->op[1], size);
    const uint64_t carry = (cpu->flags & F_CF) ? 1 : 0;
    const int locked = in->lock && dst->type == OP_MEM;
    const uint64_t address = dst->type == OP_MEM ? guest_cpu_addr(cpu, in, dst) : 0;
    uint64_t a, r;
#define CALC(a_)                                                                                  \
    (m == M(ADD) ? (a_) + b : m == M(ADC) ? (a_) + b + carry : m == M(SUB) || m == M(CMP) ? (a_) - b \
     : m == M(SBB) ? (a_) - b - carry : m == M(AND) || m == M(TEST) ? (a_) & b                   \
     : m == M(OR) ? (a_) | b : (a_) ^ b)
    if (locked) {
        ATOMIC_RMW(size, address, a, CALC(a) & guest_mask(size));
    } else {
        a = dst->type == OP_MEM ? guest_load(address, size) : guest_cpu_read(cpu, in, dst);
    }
    r = CALC(a) & guest_mask(size);
#undef CALC
    switch (m) {
    case M(ADD): flags_add(cpu, a, b, 0, r, size); break;
    case M(ADC): flags_add(cpu, a, b, carry, r, size); break;
    case M(SUB): case M(CMP): flags_sub(cpu, a, b, 0, r, size); break;
    case M(SBB): flags_sub(cpu, a, b, carry, r, size); break;
    default: flags_logic(cpu, r, size); break;
    }
    if (m == M(CMP) || m == M(TEST) || locked) return;
    if (dst->type == OP_MEM) guest_store(address, size, r);
    else guest_cpu_write(cpu, in, dst, r);
}

static void unary(GuestCpu *cpu, const GuestInsn *in) {
    const GuestOp *dst = &in->op[0];
    const int size = dst->size, m = in->mnemonic;
    const int locked = in->lock && dst->type == OP_MEM;
    const uint64_t address = dst->type == OP_MEM ? guest_cpu_addr(cpu, in, dst) : 0;
    uint64_t a;
#define CALC1(a_) (m == M(INC) ? (a_) + 1 : m == M(DEC) ? (a_) - 1 : m == M(NEG) ? 0 - (a_) : ~(a_))
    if (locked) ATOMIC_RMW(size, address, a, CALC1(a) & guest_mask(size));
    else a = guest_cpu_read(cpu, in, dst);
    const uint64_t r = CALC1(a) & guest_mask(size);
#undef CALC1
    const uint64_t cf = cpu->flags & F_CF;
    switch (m) {
    case M(INC): flags_add(cpu, a, 1, 0, r, size); cpu->flags = (cpu->flags & ~(uint64_t)F_CF) | cf; break;
    case M(DEC): flags_sub(cpu, a, 1, 0, r, size); cpu->flags = (cpu->flags & ~(uint64_t)F_CF) | cf; break;
    case M(NEG): flags_sub(cpu, 0, a, 0, r, size); break;
    default: break; /* not: no flags */
    }
    if (!locked) guest_cpu_write(cpu, in, dst, r);
}

static void bit_test(GuestCpu *cpu, const GuestInsn *in) {
    const GuestOp *dst = &in->op[0];
    const int size = dst->size, m = in->mnemonic;
    int64_t bit = (int64_t)read_src(cpu, in, &in->op[1], size);
    uint64_t address = 0;
    int width = size;
    if (dst->type == OP_MEM) {
        address = guest_cpu_addr(cpu, in, dst);
        if (in->op[1].type == OP_REG) { /* register bit offsets reach outside the operand */
            bit = guest_sext((uint64_t)bit, size);
            address += (uint64_t)((bit >> 3) & ~(int64_t)(size - 1));
            bit &= size * 8 - 1;
        } else {
            bit &= size * 8 - 1;
        }
    } else {
        bit &= size * 8 - 1;
    }
    const uint64_t mask = UINT64_C(1) << bit;
    uint64_t a;
#define CALC2(a_) (m == M(BTS) ? (a_) | mask : m == M(BTR) ? (a_) & ~mask : m == M(BTC) ? (a_) ^ mask : (a_))
    if (dst->type == OP_MEM && in->lock && m != M(BT)) ATOMIC_RMW(width, address, a, CALC2(a));
    else a = dst->type == OP_MEM ? guest_load(address, width) : guest_cpu_read(cpu, in, dst);
    const uint64_t r = CALC2(a);
#undef CALC2
    cpu->flags = (cpu->flags & ~(uint64_t)F_CF) | ((a & mask) ? F_CF : 0);
    if (m == M(BT) || (dst->type == OP_MEM && in->lock)) return;
    if (dst->type == OP_MEM) guest_store(address, width, r);
    else guest_cpu_write(cpu, in, dst, r);
}

static void cmpxchg(GuestCpu *cpu, const GuestInsn *in) {
    const GuestOp *dst = &in->op[0];
    const int size = dst->size;
    const uint64_t expected = cpu->r[RAX] & guest_mask(size);
    const uint64_t desired = guest_cpu_read(cpu, in, &in->op[1]);
    uint64_t old;
    int success;
    if (dst->type == OP_MEM) {
        /* The value the operation itself found: a separate load after a failure could see the
         * expected value again (another thread changed it back) and report a failed exchange as
         * a successful one (rax == expected, ZF set): locks built on it let two threads in. */
        success = atomic_cas(guest_cpu_addr(cpu, in, dst), size, expected, desired, &old);
    } else {
        old = guest_cpu_read(cpu, in, dst);
        success = old == expected;
        if (success) guest_cpu_write(cpu, in, dst, desired);
    }
    flags_sub(cpu, expected, old, 0, expected - old, size);
    if (!success) {
        GuestOp rax = {.type = OP_REG, .kind = RK_GPR, .reg = RAX, .size = (uint8_t)size};
        write_gpr(cpu, &rax, size, old);
    }
}

static void cmpxchg16b(GuestCpu *cpu, const GuestInsn *in) {
    const uint64_t address = guest_cpu_addr(cpu, in, &in->op[0]);
    unsigned __int128 expected = ((unsigned __int128)cpu->r[RDX] << 64) | cpu->r[RAX];
    const unsigned __int128 desired = ((unsigned __int128)cpu->r[RCX] << 64) | cpu->r[RBX];
    const int success = __atomic_compare_exchange_n((unsigned __int128 *)address, &expected,
                                                    desired, 0, __ATOMIC_SEQ_CST,
                                                    __ATOMIC_SEQ_CST);
    cpu->flags = (cpu->flags & ~(uint64_t)F_ZF) | (success ? F_ZF : 0);
    if (!success) {
        cpu->r[RAX] = (uint64_t)expected;
        cpu->r[RDX] = (uint64_t)(expected >> 64);
    }
}

static void bit_scan(GuestCpu *cpu, const GuestInsn *in) {
    const GuestOp *dst = &in->op[0];
    const int size = dst->size, m = in->mnemonic, bits = size * 8;
    const uint64_t a = guest_cpu_read(cpu, in, &in->op[1]) & guest_mask(size);
    uint64_t r;
    uint64_t f = cpu->flags;
    switch (m) {
    case M(BSF): case M(BSR):
        if (!a) { cpu->flags = f | F_ZF; return; }
        r = m == M(BSF) ? (uint64_t)__builtin_ctzll(a) : (uint64_t)(63 - __builtin_clzll(a));
        cpu->flags = f & ~(uint64_t)F_ZF;
        break;
    case M(TZCNT): case M(LZCNT):
        r = !a ? (uint64_t)bits : m == M(TZCNT) ? (uint64_t)__builtin_ctzll(a)
                                                 : (uint64_t)(__builtin_clzll(a) - (64 - bits));
        f &= ~(uint64_t)(F_CF | F_ZF);
        if (!a) f |= F_CF;
        if (!r) f |= F_ZF;
        cpu->flags = f;
        break;
    default: /* popcnt */
        r = (uint64_t)__builtin_popcountll(a);
        cpu->flags = (f & ~(uint64_t)F_ARITH) | (a ? 0 : F_ZF);
        break;
    }
    guest_cpu_write(cpu, in, dst, r);
}

static void bmi(GuestCpu *cpu, const GuestInsn *in) {
    const GuestOp *dst = &in->op[0];
    const int size = dst->size, m = in->mnemonic;
    const uint64_t mask = guest_mask(size);
    uint64_t r, f = cpu->flags & ~(uint64_t)F_ARITH;
    if (m == M(ANDN)) {
        r = ~guest_cpu_read(cpu, in, &in->op[1]) & guest_cpu_read(cpu, in, &in->op[2]) & mask;
    } else if (m == M(BEXTR)) {
        const uint64_t src = guest_cpu_read(cpu, in, &in->op[1]), ctl = guest_cpu_read(cpu, in, &in->op[2]);
        const unsigned start = ctl & 0xff, len = (ctl >> 8) & 0xff;
        r = start >= (unsigned)size * 8 ? 0 : src >> start;
        if (len < 64) r &= (UINT64_C(1) << len) - 1;
        r &= mask;
    } else {
        const uint64_t src = guest_cpu_read(cpu, in, &in->op[1]) & mask;
        if (m == M(BLSR)) { r = src & (src - 1); if (!src) f |= F_CF; }
        else if (m == M(BLSI)) { r = src & (0 - src); if (src) f |= F_CF; }
        else { r = src ^ (src - 1); if (!src) f |= F_CF; } /* blsmsk */
        r &= mask;
    }
    if (!r) f |= F_ZF;
    if (r & guest_sign(size)) f |= F_SF;
    cpu->flags = f;
    guest_cpu_write(cpu, in, dst, r);
}

/* Executes one instruction at cpu->rip; returns the next rip. */
static uint64_t step(GuestCpu *cpu, const GuestInsn *in) {
    const uint64_t next = cpu->rip + in->length;
    const int m = in->mnemonic;
    const int8_t cc = cc_table[m];
    if (cc >= 0) {
        switch (kind_table[m]) {
        case 1: return condition(cpu, cc) ? cpu->rip + (uint64_t)in->op[0].disp : next;
        case 2: guest_cpu_write(cpu, in, &in->op[0], (uint64_t)condition(cpu, cc)); return next;
        default: {
            const GuestOp *dst = &in->op[0];
            const uint64_t v = guest_cpu_read(cpu, in, &in->op[1]);
            if (condition(cpu, cc)) write_gpr(cpu, dst, dst->size, v);
            else if (dst->size == 4) cpu->r[dst->reg] = (uint32_t)cpu->r[dst->reg];
            return next;
        }
        }
    }
    if (is_string(in)) { string_op(cpu, in); return next; }
    switch (m) {
    case M(MOV): case M(MOVNTI): {
        const GuestOp *dst = &in->op[0];
        if (dst->type == OP_REG && dst->kind == RK_OTHER) return next; /* segment registers */
        guest_cpu_write(cpu, in, dst, read_src(cpu, in, &in->op[1], dst->size));
        return next;
    }
    case M(MOVZX):
        guest_cpu_write(cpu, in, &in->op[0], guest_cpu_read(cpu, in, &in->op[1]));
        return next;
    case M(MOVSX): case M(MOVSXD):
        guest_cpu_write(cpu, in, &in->op[0],
                    (uint64_t)guest_sext(guest_cpu_read(cpu, in, &in->op[1]), in->op[1].size));
        return next;
    case M(LEA):
        guest_cpu_write(cpu, in, &in->op[0], guest_cpu_addr(cpu, in, &in->op[1]) & guest_mask(in->op[0].size));
        return next;
    case M(MOVBE): {
        const uint64_t v = guest_cpu_read(cpu, in, &in->op[1]);
        const int size = in->op[0].size;
        uint64_t r = size == 2 ? __builtin_bswap16((uint16_t)v) : size == 4 ? __builtin_bswap32((uint32_t)v)
                                                                         : __builtin_bswap64(v);
        guest_cpu_write(cpu, in, &in->op[0], r);
        return next;
    }
    case M(BSWAP): {
        const GuestOp *dst = &in->op[0];
        const uint64_t v = guest_cpu_read(cpu, in, dst);
        guest_cpu_write(cpu, in, dst, dst->size == 8 ? __builtin_bswap64(v) : __builtin_bswap32((uint32_t)v));
        return next;
    }
    case M(XCHG): {
        const GuestOp *a = &in->op[0], *b = &in->op[1];
        if (a->type == OP_MEM || b->type == OP_MEM) {
            const GuestOp *mem = a->type == OP_MEM ? a : b, *reg = a->type == OP_MEM ? b : a;
            const uint64_t address = guest_cpu_addr(cpu, in, mem), v = guest_cpu_read(cpu, in, reg);
            uint64_t old;
            switch (mem->size) {
            case 1: old = __atomic_exchange_n((uint8_t *)address, (uint8_t)v, __ATOMIC_SEQ_CST); break;
            case 2: old = __atomic_exchange_n((uint16_t *)address, (uint16_t)v, __ATOMIC_SEQ_CST); break;
            case 4: old = __atomic_exchange_n((uint32_t *)address, (uint32_t)v, __ATOMIC_SEQ_CST); break;
            default: old = __atomic_exchange_n((uint64_t *)address, v, __ATOMIC_SEQ_CST); break;
            }
            guest_cpu_write(cpu, in, reg, old);
        } else {
            const uint64_t va = guest_cpu_read(cpu, in, a), vb = guest_cpu_read(cpu, in, b);
            guest_cpu_write(cpu, in, a, vb);
            guest_cpu_write(cpu, in, b, va);
        }
        return next;
    }
    case M(XADD): {
        const GuestOp *dst = &in->op[0], *src = &in->op[1];
        const int size = dst->size;
        const uint64_t b = guest_cpu_read(cpu, in, src);
        uint64_t a;
        if (dst->type == OP_MEM) {
            const uint64_t address = guest_cpu_addr(cpu, in, dst);
            ATOMIC_RMW(size, address, a, (a + b) & guest_mask(size));
        } else {
            a = guest_cpu_read(cpu, in, dst);
            guest_cpu_write(cpu, in, dst, (a + b) & guest_mask(size));
        }
        flags_add(cpu, a, b, 0, a + b, size);
        guest_cpu_write(cpu, in, src, a);
        return next;
    }
    case M(CMPXCHG): cmpxchg(cpu, in); return next;
    case M(CMPXCHG16B): cmpxchg16b(cpu, in); return next;
    case M(ADD): case M(OR): case M(ADC): case M(SBB): case M(AND): case M(SUB): case M(XOR):
    case M(CMP): case M(TEST):
        alu(cpu, in);
        return next;
    case M(INC): case M(DEC): case M(NEG): case M(NOT): unary(cpu, in); return next;
    case M(SHL): case M(SHR): case M(SAR): case M(ROL): case M(ROR): case M(RCL):
    case M(RCR):
        shift(cpu, in);
        return next;
    case M(SHLD): case M(SHRD): double_shift(cpu, in); return next;
    case M(MUL): case M(IMUL): multiply(cpu, in); return next;
    case M(DIV): case M(IDIV): divide(cpu, in); return next;
    case M(BT): case M(BTS): case M(BTR): case M(BTC): bit_test(cpu, in); return next;
    case M(BSF): case M(BSR): case M(TZCNT): case M(LZCNT): case M(POPCNT): bit_scan(cpu, in); return next;
    case M(ANDN): case M(BEXTR): case M(BLSR): case M(BLSI): case M(BLSMSK): bmi(cpu, in); return next;
    case M(CBW): cpu->r[RAX] = (cpu->r[RAX] & ~UINT64_C(0xffff)) | ((uint64_t)(int16_t)(int8_t)cpu->r[RAX] & 0xffff); return next;
    case M(CWDE): cpu->r[RAX] = (uint32_t)(int32_t)(int16_t)cpu->r[RAX]; return next;
    case M(CDQE): cpu->r[RAX] = (uint64_t)(int64_t)(int32_t)cpu->r[RAX]; return next;
    case M(CWD): cpu->r[RDX] = (cpu->r[RDX] & ~UINT64_C(0xffff)) | (((int16_t)cpu->r[RAX] < 0) ? 0xffff : 0); return next;
    case M(CDQ): cpu->r[RDX] = ((int32_t)cpu->r[RAX] < 0) ? 0xffffffffu : 0; return next;
    case M(CQO): cpu->r[RDX] = ((int64_t)cpu->r[RAX] < 0) ? ~UINT64_C(0) : 0; return next;
    case M(PUSH): {
        const GuestOp *op = &in->op[0];
        uint64_t v = op->type == OP_IMM ? (uint64_t)op->disp : guest_cpu_read(cpu, in, op);
        if (op->size == 2) { cpu->r[RSP] -= 2; guest_store(cpu->r[RSP], 2, v); return next; }
        push(cpu, v);
        return next;
    }
    case M(POP): {
        const GuestOp *op = &in->op[0];
        if (op->size == 2) { const uint64_t v = guest_load(cpu->r[RSP], 2); cpu->r[RSP] += 2; guest_cpu_write(cpu, in, op, v); return next; }
        const uint64_t v = pop(cpu);
        guest_cpu_write(cpu, in, op, v); /* pop [rsp+x] addresses with the incremented rsp */
        return next;
    }
    case M(PUSHFQ): push(cpu, cpu->flags | 2); return next;
    case M(POPFQ): cpu->flags = pop(cpu) & 0x3f7fd5; return next;
    case M(LAHF): cpu->r[RAX] = (cpu->r[RAX] & ~UINT64_C(0xff00)) | (((cpu->flags & 0xd5) | 2) << 8); return next;
    case M(SAHF): cpu->flags = (cpu->flags & ~UINT64_C(0xd5)) | ((cpu->r[RAX] >> 8) & 0xd5); return next;
    case M(LEAVE): cpu->r[RSP] = cpu->r[RBP]; cpu->r[RBP] = pop(cpu); return next;
    case M(CLC): cpu->flags &= ~(uint64_t)F_CF; return next;
    case M(STC): cpu->flags |= F_CF; return next;
    case M(CMC): cpu->flags ^= F_CF; return next;
    case M(CLD): cpu->flags &= ~(uint64_t)F_DF; return next;
    case M(STD): cpu->flags |= F_DF; return next;
    case M(NOP): case M(PAUSE): case M(LFENCE): case M(SFENCE): case M(PREFETCHNTA):
    case M(PREFETCHT0): case M(PREFETCHT1): case M(PREFETCHT2): case M(PREFETCHW):
    case M(PREFETCH): case M(ENDBR64): case M(CLFLUSH): case M(CLDEMOTE):
        return next;
    case M(MFENCE): __atomic_thread_fence(__ATOMIC_SEQ_CST); return next;
    case M(CPUID): cpuid(cpu); return next;
    case M(RDTSC): { const uint64_t t = rdtsc(); cpu->r[RAX] = (uint32_t)t; cpu->r[RDX] = t >> 32; return next; }
    case M(RDTSCP): { const uint64_t t = rdtsc(); cpu->r[RAX] = (uint32_t)t; cpu->r[RDX] = t >> 32; cpu->r[RCX] = 0; return next; }
    case M(XGETBV): cpu->r[RAX] = 7; cpu->r[RDX] = 0; return next; /* x87, SSE, AVX state on */
    case M(JMP): {
        const GuestOp *op = &in->op[0];
        const uint64_t target = op->type == OP_IMM ? cpu->rip + (uint64_t)op->disp : guest_cpu_read(cpu, in, op);
        if (!guest_cpu_is_guest_code(target)) { /* tail call of a host function */
            call_host(cpu, target, cpu->r[RSP] + 8);
            return pop(cpu);
        }
        return target;
    }
    case M(CALL): {
        const GuestOp *op = &in->op[0];
        const uint64_t target = op->type == OP_IMM ? cpu->rip + (uint64_t)op->disp : guest_cpu_read(cpu, in, op);
        if (!guest_cpu_is_guest_code(target)) {
            call_host(cpu, target, cpu->r[RSP]);
            return next;
        }
        push(cpu, next);
        return target;
    }
    case M(RET): {
        const uint64_t target = pop(cpu);
        if (in->count && in->op[0].type == OP_IMM) cpu->r[RSP] += (uint64_t)in->op[0].disp;
        return target;
    }
    case M(INT3): {
        if (!trap_handler_count) guest_cpu_fatal(cpu, in, "int3 without a trap handler");
        GuestRegs regs;
        memcpy(regs.r, cpu->r, sizeof(regs.r));
        regs.rip = next;
        if (!guest_cpu_trap(cpu, &regs)) guest_cpu_fatal(cpu, in, "unhandled int3");
        memcpy(cpu->r, regs.r, sizeof(regs.r));
        return regs.rip;
    }
    case M(JRCXZ): return cpu->r[RCX] == 0 ? cpu->rip + (uint64_t)in->op[0].disp : next;
    case M(JECXZ): return (uint32_t)cpu->r[RCX] == 0 ? cpu->rip + (uint64_t)in->op[0].disp : next;
    case M(LOOP): return --cpu->r[RCX] ? cpu->rip + (uint64_t)in->op[0].disp : next;
    case M(LOOPE): return (--cpu->r[RCX] && (cpu->flags & F_ZF)) ? cpu->rip + (uint64_t)in->op[0].disp : next;
    case M(LOOPNE): return (--cpu->r[RCX] && !(cpu->flags & F_ZF)) ? cpu->rip + (uint64_t)in->op[0].disp : next;
    default:
        break;
    }
    if (guest_cpu_exec_vec(cpu, in) || guest_cpu_exec_x87(cpu, in)) return next;
    guest_cpu_fatal(cpu, in, "unsupported instruction");
}

/* x86 locked instructions (and xchg with memory) are full barriers; arm64's acquire/release
 * atomics are not (a failed CAS only acquires), so the barriers are explicit. */
uint64_t guest_cpu_step(GuestCpu *cpu, const GuestInsn *in) {
    if (__builtin_expect(guest_cpu_probe_count, 0)) guest_cpu_run_probe(cpu);
    /* GUEST_RECORD: a call of a recorded function runs in the recorder (record.c). */
    if (__builtin_expect(guest_cpu_record_armed, 0) && guest_cpu_record_wants(cpu->rip))
        return guest_cpu_record_call(cpu);
    /* A native version of the function (native.c), unless the function is being recorded. */
    if (__builtin_expect(guest_cpu_native_count, 0) && !(guest_cpu_record_armed && guest_cpu_record_target(cpu->rip))) {
        const void *fn = guest_cpu_native_at(cpu->rip);
        if (fn) {
            __atomic_add_fetch(&guest_cpu_native_calls, 1, __ATOMIC_RELAXED);
            call_host(cpu, (uint64_t)(uintptr_t)fn, cpu->r[RSP] + 8);
            return pop(cpu);
        }
    }
    /* A recompiled version of the function (recomp.c), unless it is being recorded. */
    if (__builtin_expect(guest_cpu_recomp_count, 0) && !(guest_cpu_record_armed && guest_cpu_record_target(cpu->rip))) {
        const RcFn fn = guest_cpu_recomp_at(cpu->rip);
        if (fn && guest_cpu_recomp_thread_ok()) {
            const uint64_t back = guest_load(cpu->r[RSP], 8);
            guest_cpu_recomp_counted(cpu);
            fn(cpu);
            if (cpu->rip != back) guest_cpu_recomp_landed(cpu); /* a longjmp: to compiled code's setjmp? */
            return cpu->rip;
        }
    }
    /* The program's hook code from here on as its recompiled C (recomp.c), until the code leaves
     * its function. */
    if (__builtin_expect(guest_cpu_recomp_hook_count, 0)) {
        const RcFn fn = guest_cpu_recomp_hook_at(cpu->rip);
        if (fn && guest_cpu_recomp_thread_ok()) {
            fn(cpu);
            return cpu->rip;
        }
    }
    return guest_cpu_step_insn(cpu, in);
}

/* An instruction after a block's first: no native or recompiled function and no probe starts
 * there (decode.c ends blocks before them), so only recording is checked. */
static uint64_t step_inside(GuestCpu *cpu, const GuestInsn *in) {
    if (__builtin_expect(guest_cpu_record_armed, 0) && guest_cpu_record_wants(cpu->rip))
        return guest_cpu_record_call(cpu);
    return guest_cpu_step_insn(cpu, in);
}

uint64_t guest_cpu_step_insn(GuestCpu *cpu, const GuestInsn *in) {
    if (__builtin_expect(guest_cpu_recomp_census, 0)) guest_cpu_recomp_note_interpreted(cpu->rip);
    const int locked = in->lock || (in->mnemonic == M(XCHG) &&
                                    (in->op[0].type == OP_MEM || in->op[1].type == OP_MEM));
    if (!locked) return step(cpu, in);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    const uint64_t next = step(cpu, in);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return next;
}

void guest_cpu_call_native(GuestCpu *cpu, const void *fn) {
    __atomic_add_fetch(&guest_cpu_native_calls, 1, __ATOMIC_RELAXED);
    call_host(cpu, (uint64_t)(uintptr_t)fn, cpu->r[RSP]); /* a call: no return address pushed */
}

void guest_cpu_call_host_at_rip(GuestCpu *cpu) {
    call_host(cpu, cpu->rip, cpu->r[RSP] + 8);
    cpu->rip = pop(cpu);
}

void (*guest_cpu_return_hook)(GuestCpu *cpu);

void guest_cpu_run(GuestCpu *cpu, uint64_t stop) {
    static int initialized;
    if (!__atomic_load_n(&initialized, __ATOMIC_ACQUIRE)) {
        init_tables();
        __atomic_store_n(&initialized, 1, __ATOMIC_RELEASE);
    }
    /* A host call of guest code (run_call) starts at a recompiled function, hook code's or an
     * import stub: it runs from here as guest_cpu_step runs it, without the block decode and the
     * checks before that. */
    if (__builtin_expect(guest_cpu_recomp_count, 0) && !guest_cpu_probe_count && !guest_cpu_record_armed &&
        !guest_cpu_native_count && guest_cpu_recomp_thread_ok()) {
        const RcFn fn = guest_cpu_recomp_at(cpu->rip);
        if (fn) {
            const uint64_t back = guest_load(cpu->r[RSP], 8);
            guest_cpu_recomp_counted(cpu);
            fn(cpu);
            if (cpu->rip != back) guest_cpu_recomp_landed(cpu); /* a longjmp: to compiled code's setjmp? */
        } else {
            const RcFn hook = guest_cpu_recomp_hook_count ? guest_cpu_recomp_hook_at(cpu->rip) : NULL;
            if (hook) {
                hook(cpu);
            } else {
                const uint64_t back = guest_load(cpu->r[RSP], 8);
                if (guest_cpu_recomp_call_import(cpu) && cpu->rip != back) guest_cpu_recomp_landed(cpu);
            }
        }
        if (cpu->rip == stop) return;
    }
    if (guest_cpu_jit_run(cpu, stop)) return;
    while (cpu->rip != stop) {
        if (__builtin_expect(cpu->rip == GUEST_GUARD_RETURN, 0)) {
            guest_cpu_guard_return(cpu);
            continue;
        }
        if (!guest_cpu_is_guest_code(cpu->rip)) {
            if (guest_cpu_return_hook) guest_cpu_return_hook(cpu);
            /* Reached a host function through a return or a computed branch (a host function
             * pointer the guest stored): its return address is on the guest stack. */
            const uint64_t target = cpu->rip;
            call_host(cpu, target, cpu->r[RSP] + 8);
            cpu->rip = pop(cpu);
            continue;
        }
        const GuestBlock *block = guest_cpu_block(cpu->rip);
        for (uint32_t i = 0; i < block->count; ++i) {
            const GuestInsn *in = &block->insn[i];
            const uint64_t next = i ? step_inside(cpu, in) : guest_cpu_step(cpu, in);
            ++cpu->instructions;
            if (in->branch || next != cpu->rip + in->length) {
                cpu->rip = next;
                break;
            }
            cpu->rip = next;
        }
    }
}
