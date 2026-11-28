/* tools/recomp/recompile.c ELF FUNCTIONS_TSV OUT.c OFFSET...: the recompiler (docs/RECOMPILATION.md). Writes
 * one C function per game function (image offsets; sizes from tools/recomp/scan.c's functions.tsv)
 * against src/recomp/rc.h, in files OUT_<n>.c, and the library's table and entry point in OUT.c.
 * Build: tools/recomp/recomp.sh.
 *
 * Generated code keeps the guest registers in local arrays (of the vector ones, those it uses)
 * and the flags lazily; every instruction gets a label. Instructions the generator does not
 * translate run in the interpreter (rc->step) with the state spilled around them; a branch to an
 * unknown place goes back to the translator (rc->bail) or, out of the function, is a tail call
 * (rc->tail). */
#include "cpu/trace.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#define M(name) ZYDIS_MNEMONIC_##name

void *runtime_low_map(size_t size, int prot) {
    void *p = mmap(NULL, size, prot, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}
uint64_t guest_image_base;

/* ---- the image ---- */

typedef struct { unsigned char e_ident[16]; uint16_t e_type, e_machine; uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff; uint32_t e_flags; uint16_t e_ehsize, e_phentsize, e_phnum,
    e_shentsize, e_shnum, e_shstrndx; } Elf64_Ehdr;
typedef struct { uint32_t p_type, p_flags; uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz,
    p_align; } Elf64_Phdr;

static uint64_t base, image_size; /* where the image is mapped here, and its size */

static void map_image(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)size);
    if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) exit(1);
    fclose(f);
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)data;
    /* Segments are mapped to their alignment, as the loader maps them: code of patches can lie past
     * the code segment's end in its last page. */
#define SEGMENT_SIZE(ph) (((ph)->p_memsz + (ph)->p_align - 1) & ~((ph)->p_align - 1))
    uint64_t end = 0;
    for (int i = 0; i < eh->e_phnum; ++i) {
        const Elf64_Phdr *ph = (const Elf64_Phdr *)(data + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type == 1 && ph->p_vaddr + SEGMENT_SIZE(ph) > end) end = ph->p_vaddr + SEGMENT_SIZE(ph);
    }
    uint8_t *image = mmap(NULL, (end + 16383) & ~UINT64_C(16383), PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANON, -1, 0);
    if (image == MAP_FAILED) exit(1);
    base = (uint64_t)(uintptr_t)image;
    for (int i = 0; i < eh->e_phnum; ++i) {
        const Elf64_Phdr *ph = (const Elf64_Phdr *)(data + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type != 1) continue;
        memcpy(image + ph->p_vaddr, data + ph->p_offset, ph->p_filesz);
        if (ph->p_flags & 1) guest_cpu_add_guest_code(base + ph->p_vaddr, SEGMENT_SIZE(ph));
    }
#undef SEGMENT_SIZE
    image_size = end;
    free(data);
}

/* --hook-code: the image is the file's bytes, the program's hook code as a run wrote it. */
static void map_dump(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *image = mmap(NULL, ((uint64_t)size + 16383 + 4096) & ~UINT64_C(16383), PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANON, -1, 0);
    if (image == MAP_FAILED || fread(image, 1, (size_t)size, f) != (size_t)size) exit(1);
    fclose(f);
    base = (uint64_t)(uintptr_t)image;
    image_size = (uint64_t)size;
    guest_cpu_add_guest_code(base, image_size);
}

/* ---- functions ---- */

typedef struct { uint64_t offset, size; } Function;
static Function *functions;
static size_t function_count;

static uint64_t size_of(const char *tsv, uint64_t offset) {
    static uint64_t *starts, *sizes;
    static size_t count;
    if (!starts) {
        FILE *f = fopen(tsv, "r");
        if (!f) { perror(tsv); exit(1); }
        char line[256];
        size_t capacity = 0;
        while (fgets(line, sizeof(line), f)) {
            if (line[0] != '0') continue;
            if (count == capacity) {
                capacity = capacity ? capacity * 2 : 1 << 18;
                starts = realloc(starts, capacity * 8);
                sizes = realloc(sizes, capacity * 8);
            }
            char *end;
            starts[count] = strtoull(line, &end, 16);
            sizes[count++] = strtoull(end, NULL, 10);
        }
        fclose(f);
    }
    for (size_t lo = 0, hi = count; lo < hi;) { /* functions.tsv is sorted */
        const size_t mid = (lo + hi) / 2;
        if (starts[mid] == offset) return sizes[mid];
        if (starts[mid] < offset) lo = mid + 1; else hi = mid;
    }
    return 0;
}

/* ---- output ---- */

static FILE *out;
static void emit(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void emit(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(out, format, args);
    va_end(args);
}

/* Closes `out` where writing ended: a translation that gave up may have written past it. */
static void note_output(const char *path, int version);
static void close_output(void) {
    fflush(out);
    ftruncate(fileno(out), ftell(out));
    fclose(out);
}

/* The guest registers to and from the GuestCpu around what reads or writes them there (calls, the
 * interpreter, returns): markers that generate replaces with the copies each place needs (see
 * sync_scan). SPILL before what reads them, RELOAD after what may change them, RELOAD_IF there on
 * a path that may not be taken (a slow path inside a translation); EXIT_SPILL as SPILL where the
 * function does not go on after (the translator runs the rest, a tail call); FAULT_SPILL before a
 * fault, for its report alone (no code runs after it). */
#define SPILL "@S"
#define RELOAD "@R"
#define RELOAD_IF "@r"
#define EXIT_SPILL "@E"
#define FAULT_SPILL "@F"
/* --abi: calls between the game's functions as the System V ABI has them, for code a compiler
 * wrote. A call stores the argument registers and RSP (CALL_SPILL) and loads the return registers
 * after (CALL_RELOAD); a return stores those and RSP (RET_SPILL), a tail call the arguments
 * (TAIL_SPILL). The callee-saved registers stay in locals across a call, and the GuestCpu keeps
 * the values the function's entry had there (what its caller stored, or a frame of code that
 * keeps all registers there): code a compiler wrote only saves and restores them, and returns or
 * tail-calls with them as they were. Past a SPILL the code goes on from (a hook point, setjmp, the
 * interpreter), which stores them with the rest, the return and tail calls store them back. A call
 * of a function that holds a hook site, and one through a register or memory (its callee unknown),
 * stores the callee-saved registers too (HOOKED_CALL_SPILL): the hook's code reads them as the
 * caller left them (FieldOfView reads its caller's frame through rbp); after the call the GuestCpu
 * gets the entry's values back (CALLEE_SAVED_RESTORE). In a function in the register convention, a
 * call through a register or memory stores them only for a callee that holds a hook site, and for
 * the translator (CALLEE_SAVED_SPILL). Code the patches wrote (caves) and calls into it keep SPILL
 * and RELOAD: it may read and write any register. */
#define CALL_SPILL "@C"
#define CALL_RELOAD "@A"
#define RET_SPILL "@T"
#define TAIL_SPILL "@J"
#define HOOKED_CALL_SPILL "@K"
/* The register convention of ABI functions (a_<offset>, generate): RSP, RDI, RSI, RDX and RCX are
 * the C function's arguments and RAX and RDX its result; the other registers go as above, and
 * neither side stores RSP (the caller keeps its own). A call stores the other arguments
 * (REGISTER_CALL_SPILL, with the callee-saved registers REGISTER_HOOKED_CALL_SPILL) and loads xmm0
 * and xmm1 after (REGISTER_CALL_RELOAD); a return stores those and gives back RAX and RDX
 * (REGISTER_RET), a tail call stores the other arguments (REGISTER_TAIL_SPILL). */
#define REGISTER_CALL_SPILL "@P"
#define REGISTER_HOOKED_CALL_SPILL "@Q"
#define REGISTER_CALL_RELOAD "@B"
#define REGISTER_RET "@U"
#define REGISTER_TAIL_SPILL "@V"
/* The callee-saved registers on one path of an indirect call: for a callee that holds a hook site
 * (RcEntry.hooks) and for the translator. They stay dirty past the call: the other path leaves
 * them. */
#define CALLEE_SAVED_SPILL "@W"
/* After a call that a CALLEE_SAVED_SPILL or a HOOKED_CALL_SPILL stored callee-saved registers for:
 * the GuestCpu gets back the values it had for them (the entry's), which the marker kept. */
#define CALLEE_SAVED_RESTORE "@X"
/* A prologue's push of a callee-saved register (PUSH_SLOT and the register as two digits: the
 * store) and a pop that restores it to return (POP_SLOT: the load). In a function that keeps the
 * entry's values in the GuestCpu throughout (elide_mask) the push stores nothing and the pop takes
 * the GuestCpu's value. */
#define PUSH_SLOT "@Y"
#define POP_SLOT "@Z"
/* The declarations of other files' functions and slots, as the library defines them: no GOT. */
#define HIDDEN "__attribute__((visibility(\"hidden\")))"
#define ABI_ARGUMENTS 0x00ff03d7u    /* rax rcx rdx rsp rsi rdi r8 r9, xmm0-7 */
#define ABI_RETURNS 0x00030005u      /* rax rdx, xmm0 xmm1 */
#define ABI_CALLER_SAVED 0xffff0fc7u /* rax rcx rdx rsi rdi r8-r11, xmm0-15 */
#define ABI_CALLEE_SAVED 0x0000f028u /* rbx rbp r12-r15 */
#define ABI_REGISTER_ARGUMENTS 0x000000d6u /* rcx rdx rsp rsi rdi: a_<offset>'s arguments */
#define ABI_REGISTER_RETURNS 0x00000005u   /* rax rdx: its result */
/* a_<offset>'s parameters after the GuestCpu (RSP, RDI, RSI, RDX, RCX), a call's arguments after
 * RSP, and their stores for a function in the slot that takes them in the GuestCpu. */
#define REGISTER_PARAMETERS "uint64_t a4, uint64_t a7, uint64_t a6, uint64_t a2, uint64_t a1"
#define REGISTER_ARGUMENTS "r[RDI], r[RSI], r[RDX], r[RCX]"
#define REGISTER_STORES "c->r[RDI] = r[RDI]; c->r[RSI] = r[RSI]; c->r[RDX] = r[RDX]; c->r[RCX] = r[RCX];"
/* The library's function at an offset for indirect calls (the table's file defines it). */
#define ENTRY_FIND "const RcEntry *rc_find(uint64_t offset) " HIDDEN ";\nextern const RcEntry rc_none " HIDDEN ";\n"
static int abi_mode, abi_function, writing_patch_code;
/* The callee-saved registers the function's prologue pushes with the entry's values (elide_push:
 * the push's offset, elide_depth: its slot's bytes below entry_rsp) and pops only on the way to a
 * return or a jump (elide_candidates); those whose pushes and pops are elided (elide_mask);
 * the ones the last CALLEE_SAVED_SPILL or HOOKED_CALL_SPILL kept for CALLEE_SAVED_RESTORE. */
static uint32_t elide_candidates, elide_mask, restore_mask;
static uint64_t elide_push[16];
static uint32_t elide_depth[16];
/* The function being written is a_<offset> (the register convention), and how it returns where
 * the translator or another function ran the rest (its registers then in the GuestCpu). */
static int register_function;
/* Whether the function generate wrote last was, and by index the functions that were: RcApi.find
 * gives other libraries those (guest_recomp_find), whose calls end at the caller's next. */
static int generated_register;
static unsigned char *register_functions;
static const char *exit_return = "return;";
/* --image-base: where the program maps the image (the executable's fixed address), a constant in
 * the code instead of RcApi.image_base; 0 for the latter (modules). */
static uint64_t fixed_base;
/* The flags as RFLAGS has them are in the local ef, c->flags's copy (with the registers at the
 * markers); FLAGS_OUT stores them there itself. */
#define COND "rc_cond(%d, fk, fa, fb, fr, fz, fcf, ef)"
#define FLAGS_OUT "ef = rc_flags(fk, fa, fb, fr, fz, fcf, ef); c->flags = ef; fk = FK_CPU;"
#define FLAGS_NOW "rc_flags(fk, fa, fb, fr, fz, fcf, ef)"

typedef struct { char s[1024]; } Str;
static Str str(const char *format, ...) __attribute__((format(printf, 1, 2)));
static Str str(const char *format, ...) {
    Str v;
    va_list args;
    va_start(args, format);
    vsnprintf(v.s, sizeof(v.s), format, args);
    va_end(args);
    return v;
}
static uint64_t mask(int size) { return size >= 8 ? ~UINT64_C(0) : (UINT64_C(1) << (size * 8)) - 1; }

/* Operand fields that the game's patches set to other values (--patches): the code reads them
 * from the image as it runs (rc_code), so one version of a function serves every value. `insn`:
 * the instruction's offset, `field`: the field's; `kind` 0: the immediate, 1: the displacement. */
typedef struct { uint64_t insn, field; int size, kind; } LiveField;
static const LiveField *live;      /* the function being written's */
static size_t live_count;
static unsigned char *live_read;   /* for each of them: the translation read it */
static const char *const sint[9] = {0, "int8_t", "int16_t", 0, "int32_t", 0, 0, 0, "int64_t"};
/* --hook-code: the image is the program's hook code as a run wrote it (GUEST_RECOMP_HOOK_DUMP), at
 * hook_base in that run. Each function is the code from one of its places until that code leaves
 * the function's bytes, for any address (B: where the function is as it runs, from cpu->rip at
 * its entry): the hook code of another run or of other settings holds it elsewhere. hook_next:
 * the end of the instruction being translated. */
static int hook_code;
static uint64_t hook_base, hook_next;
static int hook_read; /* the fields the translation of the instruction read: 1 imm, 2 disp, 4 rel */

/* --hook-code: the fields of the instruction at `insn` that differ from run to run or with the
 * place of the code, which the code reads as it runs and the function's hash leaves out: a 64-bit
 * immediate (a host address), and the 32-bit distance to a place outside the hook code (the
 * game's code and data) of a rip-relative operand (`disp`) or of a jump or call (`rel`). A size of
 * 0: no such field. */
static void hook_fields(uint64_t insn, LiveField *imm, LiveField *disp, LiveField *rel) {
    static ZydisDecoder zd;
    static int ready;
    if (!ready) {
        ZydisDecoderInit(&zd, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
        ready = 1;
    }
    *imm = *disp = *rel = (LiveField){insn, 0, 0, 0};
    ZydisDecodedInstruction zi;
    if (!ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(&zd, NULL, (const void *)(uintptr_t)(base + insn), 15, &zi))) return;
    for (int k = 0; k < 2; ++k) {
        if (zi.raw.imm[k].size == 64 && !zi.raw.imm[k].is_relative) *imm = (LiveField){insn, insn + zi.raw.imm[k].offset, 8, 0};
        if (zi.raw.imm[k].size == 32 && zi.raw.imm[k].is_relative &&
            insn + zi.length + (uint64_t)zi.raw.imm[k].value.s >= image_size)
            *rel = (LiveField){insn, insn + zi.raw.imm[k].offset, 4, 0};
    }
    if (zi.raw.disp.size == 32 && zi.raw.modrm.mod == 0 && zi.raw.modrm.rm == 5 &&
        insn + zi.length + (uint64_t)zi.raw.disp.value >= image_size)
        *disp = (LiveField){insn, insn + zi.raw.disp.offset, 4, 1};
}

static const LiveField *live_at(uint64_t insn, int kind) {
    if (hook_code) {
        static LiveField field[2];
        LiveField imm, disp, rel;
        hook_fields(insn, &imm, &disp, &rel);
        field[kind] = kind ? disp : imm;
        if (field[kind].size) hook_read |= 1 << kind;
        return field[kind].size ? &field[kind] : NULL;
    }
    for (size_t i = 0; i < live_count; ++i)
        if (live[i].insn == insn && live[i].kind == kind) {
            if (live_read) live_read[i] = 1;
            return &live[i];
        }
    return NULL;
}

/* The address of a memory operand (image offsets for rip-relative); 0: not translatable. */
static int address(uint64_t off, const GuestInsn *in, const GuestOp *op, Str *s, int *stack) {
    if (op->segment) return 0;
    char text[200] = "";
    size_t n = 0;
    const LiveField *l = live_at(off, 1);
    if (op->base == 0xfe && l) {
        n += (size_t)snprintf(text + n, sizeof(text) - n, "(B + 0x%" PRIx64 "ull + (uint64_t)(int64_t)(%s)rc_code(B + 0x%" PRIx64 "ull, %d))",
                              off + in->length, sint[l->size], l->field, l->size);
    } else if (op->base == 0xfe) {
        n += (size_t)snprintf(text + n, sizeof(text) - n, "(B + 0x%" PRIx64 "ull)", off + in->length + (uint64_t)op->disp);
    } else {
        if (l) n += (size_t)snprintf(text + n, sizeof(text) - n, "((uint64_t)(int64_t)(%s)rc_code(B + 0x%" PRIx64 "ull, %d)", sint[l->size], l->field, l->size);
        else n += (size_t)snprintf(text + n, sizeof(text) - n, "(0x%" PRIx64 "ull", (uint64_t)op->disp);
        if (op->base != 0xff) n += (size_t)snprintf(text + n, sizeof(text) - n, " + r[%d]", op->base);
        if (op->index != 0xff) n += (size_t)snprintf(text + n, sizeof(text) - n, " + r[%d] * %d", op->index, op->scale);
        n += (size_t)snprintf(text + n, sizeof(text) - n, ")");
    }
    *stack = op->base == RSP && op->index == 0xff;
    *s = str("%s", text);
    return 1;
}

static int gpr(const GuestOp *op) { return op->type == OP_REG && op->kind == RK_GPR; }

/* An operand's value, zero-extended (immediates: sign-extended to `size`, then masked). */
static int read_op(uint64_t off, const GuestInsn *in, const GuestOp *op, int size, Str *v) {
    if (gpr(op)) {
        if (op->size == 1 && op->high8) *v = str("((r[%d] >> 8) & 0xff)", op->reg);
        else if (op->size == 8) *v = str("r[%d]", op->reg);
        else *v = str("(r[%d] & 0x%" PRIx64 "ull)", op->reg, mask(op->size));
        return 1;
    }
    if (op->type == OP_IMM) {
        const LiveField *l = live_at(off, 0);
        if (l) *v = str("((uint64_t)(int64_t)(%s)rc_code(B + 0x%" PRIx64 "ull, %d) & 0x%" PRIx64 "ull)", sint[l->size], l->field, l->size, mask(size));
        else *v = str("0x%" PRIx64 "ull", (uint64_t)op->disp & mask(size));
        return 1;
    }
    if (op->type == OP_MEM) {
        Str a;
        int stack;
        if (!address(off, in, op, &a, &stack)) return 0;
        *v = str("%s(%s, %d)", stack ? "rc_lds" : "rc_ld", a.s, op->size);
        return 1;
    }
    return 0;
}

/* A statement writing `v` to a register or memory operand. */
static int write_op(uint64_t off, const GuestInsn *in, const GuestOp *op, const char *v, Str *s) {
    if (gpr(op)) {
        const int x = op->reg;
        switch (op->size) {
        case 8: *s = str("r[%d] = (%s);", x, v); break;
        case 4: *s = str("r[%d] = (uint32_t)(%s);", x, v); break;
        case 2: *s = str("r[%d] = (r[%d] & ~0xffffull) | ((%s) & 0xffff);", x, x, v); break;
        default:
            if (op->high8) *s = str("r[%d] = (r[%d] & ~0xff00ull) | (((%s) & 0xff) << 8);", x, x, v);
            else *s = str("r[%d] = (r[%d] & ~0xffull) | ((%s) & 0xff);", x, x, v);
        }
        return 1;
    }
    if (op->type == OP_MEM) {
        Str a;
        int stack;
        if (!address(off, in, op, &a, &stack)) return 0;
        *s = str("%s(%s, %d, (%s));", stack ? "rc_sts" : "rc_st", a.s, op->size, v);
        return 1;
    }
    return 0;
}

static int cc_of(int m) {
    static const int jcc[16] = {M(JO), M(JNO), M(JB), M(JNB), M(JZ), M(JNZ), M(JBE), M(JNBE),
                                M(JS), M(JNS), M(JP), M(JNP), M(JL), M(JNL), M(JLE), M(JNLE)};
    static const int setcc[16] = {M(SETO), M(SETNO), M(SETB), M(SETNB), M(SETZ), M(SETNZ), M(SETBE),
                                  M(SETNBE), M(SETS), M(SETNS), M(SETP), M(SETNP), M(SETL), M(SETNL),
                                  M(SETLE), M(SETNLE)};
    static const int cmovcc[16] = {M(CMOVO), M(CMOVNO), M(CMOVB), M(CMOVNB), M(CMOVZ), M(CMOVNZ),
                                   M(CMOVBE), M(CMOVNBE), M(CMOVS), M(CMOVNS), M(CMOVP), M(CMOVNP),
                                   M(CMOVL), M(CMOVNL), M(CMOVLE), M(CMOVNLE)};
    for (int i = 0; i < 16; ++i) {
        if (jcc[i] == m) return i;
        if (setcc[i] == m) return 16 + i;
        if (cmovcc[i] == m) return 32 + i;
    }
    return -1;
}

typedef struct { uint64_t start, end; } Range;

/* Code of the game's patches (code caves): the bytes the patches being applied write, outside
 * functions or over the body of a function they leave unused, by offset. A function's jumps into
 * them are gotos (decode enters them), and they are part of the version's hash
 * (RcFunction.spans). A function's entry is not a cave: a jump there is a tail call. */
static Range *caves;
static size_t cave_count;
enum { MAX_ENTERED = 64 };
static Range entered[MAX_ENTERED]; /* the caves the function being written jumps into */
static size_t entered_count;
/* Set while patches are on the image (apply_patches): decode reads it without the block cache,
 * which holds the unpatched code. */
static int patches_on;
/* While a version is written (write_version): an instruction that stops the game in the bytes the
 * patches wrote makes the version data, not code (a patch's table over a function it leaves
 * unused, a combination of patches that go together with no others). */
static int writing_version, version_broken;
static int in_patched_bytes(uint64_t off, uint64_t size);
static long function_at(uint64_t off);
static const Range *cave_at(uint64_t t) {
    const long f = cave_count ? function_at(t) : -1;
    if (f >= 0 && functions[f].offset == t) return NULL;
    for (size_t lo = 0, hi = cave_count; lo < hi;) {
        const size_t mid = (lo + hi) / 2;
        if (t < caves[mid].start) hi = mid;
        else if (t >= caves[mid].end) lo = mid + 1;
        else return &caves[mid];
    }
    return NULL;
}
/* Of the function decoded last: its direct calls into caves (functions of the patches), and the
 * cave code it runs, coalesced (its version's RcFunction.spans). */
static uint64_t *cave_calls;
static size_t cave_call_count;
static Range *spans;
static size_t span_count;
/* Whether `t` is code of the function being written: its own or a cave it jumps into. */
static int inside(Range f, uint64_t t) {
    if (t >= f.start && t < f.end) return 1;
    for (size_t i = 0; i < entered_count; ++i)
        if (t >= entered[i].start && t < entered[i].end) return 1;
    return 0;
}

/* The functions being recompiled, by offset: calls of them go through their direct-call slot
 * (d_<offset>, rc.h) when the program filled it. */
static uint64_t *recompiled;
/* Import stubs by name (imports.tsv): calls of them are labelled. */
typedef struct { uint64_t stub; char name[64]; } Import;
static Import *imports;
static size_t import_count;
static const char *import_name(uint64_t t) {
    for (size_t lo = 0, hi = import_count; lo < hi;) {
        const size_t mid = (lo + hi) / 2;
        if (imports[mid].stub == t) return imports[mid].name;
        if (imports[mid].stub < t) lo = mid + 1; else hi = mid;
    }
    return NULL;
}
/* Whether `t` is the stub of setjmp, _setjmp or sigsetjmp: a call of it returns twice, the second
 * time from a longjmp (RcResume). */
static int is_setjmp(uint64_t t) {
    const char *name = import_name(t);
    return name && (!strcmp(name, "setjmp") || !strcmp(name, "_setjmp") || !strcmp(name, "sigsetjmp"));
}
/* The integer arguments of an import that takes only integers and pointers and calls no guest
 * code (GR2port's host functions behind the libc and kernel imports), or -1: its call goes to the
 * host function the slot holds as a C call (ABI functions). */
static int host_arguments(const char *name) {
    static const struct { const char *name; int count; } known[] = {
        {"memcpy", 3}, {"memmove", 3}, {"memset", 3}, {"memcmp", 3}, {"memchr", 3}, {"strlen", 1},
        {"strcmp", 2}, {"strncmp", 3}, {"strncpy", 3}, {"strchr", 2}, {"strrchr", 2}, {"strstr", 2},
        {"scePthreadMutexLock", 1}, {"scePthreadMutexUnlock", 1}, {"scePthreadMutexTrylock", 1},
        {"pthread_mutex_lock", 1}, {"pthread_mutex_unlock", 1}, {"pthread_mutex_trylock", 1},
        {"scePthreadRwlockRdlock", 1}, {"scePthreadRwlockWrlock", 1}, {"scePthreadRwlockUnlock", 1},
        {"pthread_rwlock_rdlock", 1}, {"pthread_rwlock_wrlock", 1}, {"pthread_rwlock_unlock", 1},
        {"sceKernelWaitSema", 3}, {"sceKernelSignalSema", 2}, {"sceKernelPollSema", 2},
        {"scePthreadSelf", 0}, {"pthread_self", 0}, {"scePthreadEqual", 2}, {"pthread_equal", 2},
        {"sceKernelReadTsc", 0}, {"_sigprocmask", 3}, {"sigprocmask", 3},
        {"sceKernelGetProcessTime", 0}, {"sceKernelGetProcessTimeCounter", 0},
        {"sceKernelWaitEventFlag", 5}, {"sceKernelPollEventFlag", 5}, {"sceKernelSetEventFlag", 2},
        {"sceKernelClearEventFlag", 2}, {"sceNgs2RackGetVoiceHandle", 3}, {"sceNgs2VoiceGetState", 3},
        {"sceLibcMspaceFree", 2},
    };
    for (size_t k = 0; name && k < sizeof(known) / sizeof(known[0]); ++k)
        if (!strcmp(name, known[k].name)) return known[k].count;
    return -1;
}
/* The calls of setjmp in the function being generated so far (its rs[]), and in all. */
static int setjmp_calls;
static size_t setjmp_calls_total;
/* The project's names for the game's functions (annotations/functions.tsv, given by
 * GUEST_RECOMP_ANNOTATIONS): written at each function and at calls of it ("?" when a guess). */
typedef struct { uint64_t offset; char name[256]; } Annotation;
static Annotation *annotations;
static size_t annotation_count;
static const char *annotation(uint64_t offset) {
    for (size_t lo = 0, hi = annotation_count; lo < hi;) {
        const size_t mid = (lo + hi) / 2;
        if (annotations[mid].offset == offset) return annotations[mid].name;
        if (annotations[mid].offset < offset) lo = mid + 1; else hi = mid;
    }
    return NULL;
}
static void load_annotations(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); exit(1); }
    char line[1024];
    for (int number = 1; fgets(line, sizeof(line), f); ++number) {
        if (line[0] == '#' || line[0] == '\n') continue;
        unsigned long long offset;
        char name[250], confidence[16];
        if (sscanf(line, "%llx\t%249[^\t\n]\t%15[^\t\n]", &offset, name, confidence) != 3 || strstr(name, "*/") ||
            (strcmp(confidence, "sure") && strcmp(confidence, "guess")) ||
            (annotation_count && offset <= annotations[annotation_count - 1].offset)) {
            fprintf(stderr, "%s:%d: not offset, name, sure or guess, in offset order\n", path, number);
            exit(1);
        }
        annotations = realloc(annotations, (annotation_count + 1) * sizeof(*annotations));
        annotations[annotation_count].offset = offset;
        snprintf(annotations[annotation_count++].name, sizeof(annotations->name), "%s%s", name,
                 strcmp(confidence, "guess") ? "" : "?");
    }
    fclose(f);
}
static int by_offset(const void *a, const void *b);
static int holds_hook_site(uint64_t t);
static int is_recompiled(uint64_t t) {
    return recompiled && bsearch(&t, recompiled, function_count, sizeof(*recompiled), by_offset) != NULL;
}
/* The slot of the import stub at `t` (jmp [rip+disp], the loader fills the slot), 0 when `t` is
 * other code: calls of a stub go to RcApi.import. */
static uint64_t import_slot(uint64_t t) {
    if (cave_at(t) || is_recompiled(t) || t + 6 > image_size) return 0;
    const uint8_t *code = (const uint8_t *)(uintptr_t)(base + t);
    if (code[0] != 0xff || code[1] != 0x25) return 0;
    int32_t disp;
    memcpy(&disp, code + 2, 4);
    return t + 6 + (uint64_t)(int64_t)disp;
}
/* A call of an import that never returns: nothing after it is this call's fall-through (a
 * function can end with one, data such as a jump table right after it). */
static int no_return(uint64_t off, const GuestInsn *in) {
    if (in->mnemonic != M(CALL) || in->op[0].type != OP_IMM) return 0;
    const char *name = import_name(off + (uint64_t)in->op[0].disp);
    return name && (!strcmp(name, "__stack_chk_fail") || !strcmp(name, "abort") || !strcmp(name, "exit") ||
                    !strcmp(name, "_exit"));
}
/* Whether execution can go on to the instruction after this one. */
static int falls_through(uint64_t off, const GuestInsn *in) {
    const int m = in->mnemonic;
    return !(m == M(JMP) || m == M(RET) || m == M(INVALID) || m == M(UD2) || m == M(HLT) || no_return(off, in));
}

/* The call of recompiled function `t` through its slot (RcFunction.direct, which the program
 * fills): f_t itself while the slot holds NULL (the unpatched version without an entry hook),
 * which the compiler may inline; else what it holds, with cpu->rip at t (a slot only goes from
 * NULL to a function as the code runs). */
static Str direct_call(uint64_t t) {
    return str("{ if (__builtin_expect(!d_%" PRIx64 ", 1)) f_%" PRIx64 "(c); else { c->rip = B + 0x%" PRIx64
               "ull; d_%" PRIx64 "(c); } }", t, t, t, t);
}

/* A jump to image offset `t`: a goto inside the function. Out of it: a tail call when the stack is
 * back at the entry (the return address on top), else code of this function elsewhere (a part the
 * compiler moved out, a shared block): the translator runs the rest, the function's return address
 * at entry_rsp (rc->bail). */
/* What a call or tail call of recompiled function `t` names: its slot, f_t and a_t. */
static Str callee_declarations(uint64_t t) {
    return str("extern RcFn d_%" PRIx64 " " HIDDEN "; void f_%" PRIx64 "(GuestCpu *c) " HIDDEN "; RcRet a_%" PRIx64
               "(GuestCpu *c, " REGISTER_PARAMETERS ") " HIDDEN ";", t, t, t);
}

/* --hook-code: where a direct jump or call of the instruction being translated goes: a place in the
 * hook code, or one outside it by the distance the instruction holds as the code runs. */
static Str hook_target(uint64_t t) {
    if (t < image_size) return str("B + 0x%" PRIx64 "ull", t);
    hook_read |= 4;
    return str("(B + 0x%" PRIx64 "ull + (uint64_t)(int64_t)(int32_t)rc_code(B + 0x%" PRIx64 "ull, 4))", hook_next,
               hook_next - 4);
}

static void jump(Range f, uint64_t t) {
    if (inside(f, t)) emit("goto L_%" PRIx64 ";", t);
    else if (hook_code) /* out of the function: what runs it goes on from there */
        emit("{ " EXIT_SPILL " " FLAGS_OUT " c->rip = %s; return; }", hook_target(t).s);
    else if (register_function && !cave_at(t) && is_recompiled(t)) /* a_t's result is this function's */
        emit("{ %s if (__builtin_expect(r[RSP] == entry_rsp, 1)) { " REGISTER_TAIL_SPILL " if (__builtin_expect(!d_%" PRIx64
             ", 1)) return a_%" PRIx64 "(c, r[RSP], " REGISTER_ARGUMENTS "); c->r[RSP] = r[RSP]; " REGISTER_STORES
             " c->rip = B + 0x%" PRIx64 "ull; d_%" PRIx64 "(c); } else { " EXIT_SPILL " " FLAGS_OUT
             " c->rip = B + 0x%" PRIx64 "ull; rc->bail(c, entry_rsp); } %s }", callee_declarations(t).s, t, t, t, t, t,
             exit_return);
    else if (abi_function && !cave_at(t) && is_recompiled(t))
        emit("{ extern RcFn d_%" PRIx64 " " HIDDEN "; void f_%" PRIx64 "(GuestCpu *c) " HIDDEN "; if (__builtin_expect(r[RSP] == entry_rsp, 1)) { " TAIL_SPILL " %s } else { " EXIT_SPILL " " FLAGS_OUT " c->rip = B + 0x%" PRIx64 "ull; rc->bail(c, entry_rsp); } %s }", t, t, direct_call(t).s, t, exit_return);
    else if (register_function && import_slot(t) && host_arguments(import_name(t)) >= 0) {
        /* the host function the slot held the last time, as C (a call of it does the same); its
         * result this function's */
        static const char *const names[] = {"r[RDI]", "r[RSI]", "r[RDX]", "r[RCX]", "r[R8]", "r[R9]"};
        const int count = host_arguments(import_name(t));
        Str types = str("%s", count ? "" : "void"), values = str("%s", "");
        for (int k = 0; k < count; ++k) {
            types = str("%s%suint64_t", types.s, k ? ", " : "");
            values = str("%s%s%s", values.s, k ? ", " : "", names[k]);
        }
        emit("{ if (__builtin_expect(r[RSP] == entry_rsp, 1)) { static uint64_t h; const uint64_t s = rc_lds(B + 0x%" PRIx64
             "ull, 8); if (__builtin_expect(s && s == __atomic_load_n(&h, __ATOMIC_RELAXED), 1)) { c->r[RSP] = r[RSP]; "
             "r[RAX] = ((uint64_t (*)(%s))s)(%s); " REGISTER_RET " } " TAIL_SPILL " rc->import(c, &h, B + 0x%" PRIx64
             "ull, B + 0x%" PRIx64 "ull, 0); } else { " EXIT_SPILL " " FLAGS_OUT " c->rip = B + 0x%" PRIx64 "ull; rc->bail(c, "
             "entry_rsp); } %s }", import_slot(t), types.s, values.s, t, import_slot(t), t, exit_return);
    } else if (register_function && import_slot(t))
        /* the function the slot held the last time, as an indirect tail call has it: a module's in the
         * register convention (RcApi.find); rc->import for a host function */
        emit("{ if (__builtin_expect(r[RSP] == entry_rsp, 1)) { const uint64_t s = rc_lds(B + 0x%" PRIx64 "ull, 8); static "
             "const RcEntry *ic = &rc_none; const RcEntry *e = __atomic_load_n(&ic, __ATOMIC_RELAXED); if "
             "(__builtin_expect(e->offset != s - B, 0)) e = rc_miss(&ic, s, B); if (__builtin_expect(!*e->slot, 1)) { "
             REGISTER_TAIL_SPILL " return e->abi(c, r[RSP], " REGISTER_ARGUMENTS "); } static uint64_t h; " TAIL_SPILL
             " rc->import(c, &h, B + 0x%" PRIx64 "ull, B + 0x%" PRIx64 "ull, 0); } else { " EXIT_SPILL " " FLAGS_OUT
             " c->rip = B + 0x%" PRIx64 "ull; rc->bail(c, entry_rsp); } %s }",
             import_slot(t), t, import_slot(t), t, exit_return);
    else if (abi_function && import_slot(t))
        emit("{ if (__builtin_expect(r[RSP] == entry_rsp, 1)) { static uint64_t h; " TAIL_SPILL " rc->import(c, &h, B + 0x%" PRIx64 "ull, B + 0x%" PRIx64 "ull, 0); } else { " EXIT_SPILL " " FLAGS_OUT " c->rip = B + 0x%" PRIx64 "ull; rc->bail(c, entry_rsp); } %s }", t, import_slot(t), t, exit_return);
    else if (is_recompiled(t))
        emit("{ extern RcFn d_%" PRIx64 " " HIDDEN "; void f_%" PRIx64 "(GuestCpu *c) " HIDDEN "; " EXIT_SPILL " " FLAGS_OUT " c->rip = B + 0x%" PRIx64 "ull; if (__builtin_expect(r[RSP] == entry_rsp, 1)) %s else rc->bail(c, entry_rsp); %s }", t, t, t, direct_call(t).s, exit_return);
    else if (import_slot(t))
        emit("{ static uint64_t h; " EXIT_SPILL " " FLAGS_OUT " c->rip = B + 0x%" PRIx64 "ull; if (__builtin_expect(r[RSP] == entry_rsp, 1)) rc->import(c, &h, B + 0x%" PRIx64 "ull, B + 0x%" PRIx64 "ull, 0); else rc->bail(c, entry_rsp); %s }", t, t, import_slot(t), exit_return);
    else emit("{ " EXIT_SPILL " " FLAGS_OUT " c->rip = B + 0x%" PRIx64 "ull; if (__builtin_expect(r[RSP] == entry_rsp, 1)) rc->tail(c); else rc->bail(c, entry_rsp); %s }", t, exit_return);
}

/* Snippet mode (fuzzing, tests/fuzz_recomp.c): one instruction, then a return at its end. */
static int snippet_mode;

/* Where the code goes at dn, a guest address only known as it runs (an indirect jump, the
 * interpreter's next rip). With dispatch_cases (a function with a computed jump and a label at every
 * instruction): L_dispatch, a switch over the labels that leaves the function for any other address.
 * Without: out of the function right there (dispatch_exit, set by generate), so that each place
 * stores only the registers dirty at it and no label takes on the dirty and live registers of all
 * of them. */
static int dispatch_cases = 1;
static char dispatch_exit[8192]; /* up to MAX_ENTERED caves */
static const char *dispatch(void) { return dispatch_cases ? "goto L_dispatch;" : dispatch_exit; }

/* The status flags the instruction being translated writes are read by nothing (flags_dead_after):
 * a shift's, imul's, shld's and shrd's then go unstored. */
static int flags_dead;

/* The jump tables decode found, by their jmp: the table load's offset and index register, and the
 * targets by index. generate captures the index at the load (jt_<load>) and switches on it at the
 * jmp, so each table's jmp goes only to its own targets. */
typedef struct { uint64_t jmp, load, entries, *targets; int index; } JumpTable;
static JumpTable *jump_tables;
static size_t jump_table_count;
static const JumpTable *jump_table_at(uint64_t jmp) {
    for (size_t i = 0; i < jump_table_count; ++i)
        if (jump_tables[i].jmp == jmp) return &jump_tables[i];
    return NULL;
}

/* The instruction in the interpreter, in snippets only (the fuzzer compares the two): state
 * spilled, flags in cpu->flags; an unexpected next rip dispatches. Also the slow path of
 * translations (division faults). */
static void emit_interp(uint64_t off, const GuestInsn *in, int slow_path) {
    emit("/* %s */ { " SPILL " " FLAGS_OUT " c->rip = B + 0x%" PRIx64 "ull; const uint64_t n = rc->step(c); %s"
         " if (n != B + 0x%" PRIx64 "ull) { dn = n; %s } }",
         ZydisMnemonicGetString((ZydisMnemonic)in->mnemonic), off, slow_path ? RELOAD_IF : RELOAD, off + in->length, dispatch());
}

/* The instruction stops the game (RcApi.fault, kind one of rc.h's RC_FAULT_*), the state spilled
 * for its report. */
static void emit_fault(uint64_t off, const GuestInsn *in, const char *kind) {
    emit("/* %s */ { " FAULT_SPILL " " FLAGS_OUT " rc->fault(c, B + 0x%" PRIx64 "ull, %s); }",
         ZydisMnemonicGetString((ZydisMnemonic)in->mnemonic), off, kind);
}

/* Instructions without C, listed in the report (OUT.not-compiled.tsv: offset, mnemonic). */
static FILE *not_compiled_report;
static uint64_t not_compiled;

/* An instruction translate has no C for: int3 runs the trap handlers (RcApi.trap), and in a
 * function in the register convention the translator goes on from where they say (the function's
 * other paths then keep the entry's callee-saved values in the GuestCpu); the ones that stop a
 * program on the hardware (ud2, int N, hlt, privileged ones, undecodable bytes) stop the game
 * (RcApi.fault); any other is a gap of the generator, listed in the report, and stops the game if
 * it is reached. */
static void emit_untranslated(uint64_t off, const GuestInsn *in) {
    const int m = in->mnemonic;
    if (m == M(INT3) && register_function) {
        emit("/* int3 */ { " EXIT_SPILL " " FLAGS_OUT " c->rip = rc->trap(c, B + 0x%" PRIx64 "ull); rc->bail(c, entry_rsp); %s }",
             off + in->length, exit_return);
        return;
    }
    if (m == M(INT3)) {
        emit("/* int3 */ { " SPILL " " FLAGS_OUT " const uint64_t n = rc->trap(c, B + 0x%" PRIx64 "ull); " RELOAD
             " if (n != B + 0x%" PRIx64 "ull) { dn = n; %s } }",
             off + in->length, off + in->length, dispatch());
        return;
    }
    const char *kind;
    switch (m) {
    case M(UD0): case M(UD1): case M(UD2): case M(INVALID):
        kind = "RC_FAULT_UNDEFINED";
        break;
    case M(INT): case M(INT1): case M(INTO):
        kind = "RC_FAULT_INTERRUPT";
        break;
    case M(HLT): case M(CLI): case M(STI): case M(IN): case M(OUT): case M(INSB): case M(INSW): case M(INSD):
    case M(OUTSB): case M(OUTSW): case M(OUTSD): case M(IRET): case M(IRETD): case M(IRETQ): case M(LGDT):
    case M(LIDT): case M(LLDT): case M(LTR): case M(RDMSR): case M(WRMSR): case M(INVD): case M(WBINVD):
    case M(INVLPG): case M(SWAPGS): case M(SYSRET): case M(CLTS):
        kind = "RC_FAULT_PRIVILEGED";
        break;
    default:
        kind = "RC_FAULT_NOT_COMPILED";
        break;
    }
    if (writing_version && in_patched_bytes(off, in->length)) {
        version_broken = 1;
    } else if (!strcmp(kind, "RC_FAULT_NOT_COMPILED")) {
        ++not_compiled;
        if (not_compiled_report)
            fprintf(not_compiled_report, "%#" PRIx64 "\t%s\n", off, ZydisMnemonicGetString((ZydisMnemonic)m));
    }
    emit_fault(off, in, kind);
}

/* movs/stos/lods/cmps/scas (movsd and cmpsd without a vector operand), with or without rep, repe
 * and repne (the interpreter's string_op): rsi, rdi, rcx and the direction flag. */
static int string_size(int m) {
    switch (m) {
    case M(MOVSB): case M(STOSB): case M(LODSB): case M(CMPSB): case M(SCASB): return 1;
    case M(MOVSW): case M(STOSW): case M(LODSW): case M(CMPSW): case M(SCASW): return 2;
    case M(MOVSD): case M(STOSD): case M(LODSD): case M(CMPSD): case M(SCASD): return 4;
    case M(MOVSQ): case M(STOSQ): case M(LODSQ): case M(CMPSQ): case M(SCASQ): return 8;
    default: return 0;
    }
}
/* A string instruction: movsd and cmpsd are SSE ones with a vector operand. */
static int is_string(const GuestInsn *in) {
    if (!string_size(in->mnemonic)) return 0;
    for (int k = 0; k < in->count; ++k)
        if (in->op[k].type == OP_REG && in->op[k].kind == RK_VEC) return 0;
    return 1;
}
static int translate_string(const GuestInsn *in) {
    const int m = in->mnemonic, size = string_size(m);
    const int repeat = in->rep || in->repne;
    const int compares = m == M(CMPSB) || m == M(CMPSW) || m == M(CMPSD) || m == M(CMPSQ) || m == M(SCASB) ||
                         m == M(SCASW) || m == M(SCASD) || m == M(SCASQ);
    const int moves = m == M(MOVSB) || m == M(MOVSW) || m == M(MOVSD) || m == M(MOVSQ);
    const int stores = m == M(STOSB) || m == M(STOSW) || m == M(STOSD) || m == M(STOSQ);
    const int loads = m == M(LODSB) || m == M(LODSW) || m == M(LODSD) || m == M(LODSQ);
    emit("{ const int64_t st = (ef & F_DF) ? -%d : %d; ", size, size);
    if (repeat) emit("for (;;) { if (!r[RCX]) break; ");
    if (moves) {
        if (repeat && size == 1) /* the interpreter's forward copy */
            emit("if (st > 0 && r[RCX] > 16 && (r[RDI] + r[RCX] <= r[RSI] || r[RSI] + r[RCX] <= r[RDI] || r[RDI] < r[RSI])) { "
                 "guest_fence_store(); memmove((void *)r[RDI], (const void *)r[RSI], r[RCX]); guest_fence_load(); "
                 "r[RSI] += r[RCX]; r[RDI] += r[RCX]; r[RCX] = 0; break; } ");
        emit("rc_st(r[RDI], %d, rc_ld(r[RSI], %d)); r[RSI] += st; r[RDI] += st; ", size, size);
    } else if (stores) {
        if (repeat && size == 1)
            emit("if (st > 0 && r[RCX] > 16) { guest_fence_store(); memset((void *)r[RDI], (int)(r[RAX] & 0xff), r[RCX]); "
                 "r[RDI] += r[RCX]; r[RCX] = 0; break; } ");
        emit("rc_st(r[RDI], %d, r[RAX]); r[RDI] += st; ", size);
    } else if (loads) {
        if (size == 8) emit("r[RAX] = rc_ld(r[RSI], 8); ");
        else if (size == 4) emit("r[RAX] = (uint32_t)rc_ld(r[RSI], 4); ");
        else emit("r[RAX] = (r[RAX] & ~0x%" PRIx64 "ull) | rc_ld(r[RSI], %d); ", mask(size), size);
        emit("r[RSI] += st; ");
    } else if (m == M(CMPSB) || m == M(CMPSW) || m == M(CMPSD) || m == M(CMPSQ)) {
        emit("{ const uint64_t a = rc_ld(r[RSI], %d), b = rc_ld(r[RDI], %d); fk = FK_SUB; fa = a; fb = b; "
             "fr = (a - b) & 0x%" PRIx64 "ull; fz = %d; } r[RSI] += st; r[RDI] += st; ", size, size, mask(size), size);
    } else {
        emit("{ const uint64_t a = r[RAX] & 0x%" PRIx64 "ull, b = rc_ld(r[RDI], %d); fk = FK_SUB; fa = a; fb = b; "
             "fr = (a - b) & 0x%" PRIx64 "ull; fz = %d; } r[RDI] += st; ", mask(size), size, mask(size), size);
    }
    if (repeat) {
        emit("--r[RCX]; ");
        if (compares && in->rep) emit("if (!" COND ") break; ", 4);
        if (compares && in->repne) emit("if (" COND ") break; ", 4);
        emit("} ");
    }
    emit("}\n");
    return 1;
}

static int translate_vector(uint64_t off, const GuestInsn *in);
static int translate_bits(uint64_t off, const GuestInsn *in);
static int translate_shift_mul(uint64_t off, const GuestInsn *in);
static int translate_extra(uint64_t off, const GuestInsn *in);
static int translate_atomic(uint64_t off, const GuestInsn *in);
static int translate_x87(uint64_t off, const GuestInsn *in);

/* One instruction natively; 0: it goes to the interpreter. */
static int translate(Range f, uint64_t off, const GuestInsn *in, uint64_t *stats) {
    const uint64_t next = off + in->length;
    const int m = in->mnemonic;
    hook_next = next;
    const GuestOp *d = &in->op[0], *s = &in->op[1];
    Str a, b, w;
    if (is_string(in)) return translate_string(in);
    if (in->rep || in->repne) return 0;
    if ((in->lock && m != M(XCHG)) || m == M(XADD) || m == M(CMPXCHG) || m == M(CMPXCHG16B))
        return translate_atomic(off, in);
    {
        const int x87 = translate_x87(off, in);
        if (x87 >= 0) return x87;
    }
    const int cc = cc_of(m);
    if (cc >= 0 && cc < 16) { /* jcc */
        if (d->type != OP_IMM) return 0;
        emit("if (" COND ") ", cc);
        jump(f, off + (uint64_t)d->disp);
        emit("\n");
        return 1;
    }
    if (cc >= 16 && cc < 32) { /* setcc */
        if (!write_op(off, in, d, str(COND, cc - 16).s, &w)) return 0;
        emit("%s\n", w.s);
        return 1;
    }
    if (cc >= 32) { /* cmovcc */
        if (!gpr(d) || !read_op(off, in, s, d->size, &b) || !write_op(off, in, d, b.s, &w)) return 0;
        emit("if (" COND ") { %s }", cc - 32, w.s);
        if (d->size == 4) emit(" else { r[%d] = (uint32_t)r[%d]; }", d->reg, d->reg);
        emit("\n");
        return 1;
    }
    if (guest_cpu_is_tcb_load(in)) { /* mov reg, fs:[0] (the loader makes it a gs load): the TCB */
        emit("r[%d] = c->tcb;\n", d->reg);
        return 1;
    }
    switch (m) {
    case M(PAUSE):
        emit("rc_pause();\n");
        return 1;
    case M(PREFETCHNTA): case M(PREFETCHT0): case M(PREFETCHT1): case M(PREFETCHT2): case M(PREFETCHW):
    case M(PREFETCH): { /* the host's prefetch of the line (it never faults) */
        int stack;
        if (d->type == OP_MEM && address(off, in, d, &a, &stack)) {
            emit("__builtin_prefetch((const void *)(uintptr_t)%s, %d, %d);\n", a.s, m == M(PREFETCHW) ? 1 : 0,
                 m == M(PREFETCHNTA) ? 0 : m == M(PREFETCHT2) ? 1 : m == M(PREFETCHT1) ? 2 : 3);
            return 1;
        }
        emit("/* %s */\n", ZydisMnemonicGetString((ZydisMnemonic)m));
        return 1;
    }
    case M(NOP): case M(ENDBR64): case M(LFENCE): case M(SFENCE): case M(CLFLUSH):
    case M(CLDEMOTE): /* as the interpreter runs them */
        emit("/* %s */\n", ZydisMnemonicGetString((ZydisMnemonic)m));
        return 1;
    case M(MFENCE):
        emit("__atomic_thread_fence(__ATOMIC_SEQ_CST);\n");
        return 1;
    /* Flags (the interpreter's): CF and the arithmetic ones from the lazy state, DF in cpu->flags. */
    case M(CLC): emit("ef = " FLAGS_NOW " & ~(uint64_t)F_CF; fk = FK_CPU;\n"); return 1;
    case M(STC): emit("ef = " FLAGS_NOW " | F_CF; fk = FK_CPU;\n"); return 1;
    case M(CMC): emit("ef = " FLAGS_NOW " ^ F_CF; fk = FK_CPU;\n"); return 1;
    case M(CLD): emit("ef &= ~(uint64_t)F_DF;\n"); return 1;
    case M(STD): emit("ef |= F_DF;\n"); return 1;
    case M(PUSHFQ): emit("{ const uint64_t v = " FLAGS_NOW " | 2; r[RSP] -= 8; rc_sts(r[RSP], 8, v); }\n"); return 1;
    case M(POPFQ): emit("{ ef = rc_lds(r[RSP], 8) & 0x3f7fd5; r[RSP] += 8; fk = FK_CPU; }\n"); return 1;
    case M(LAHF): emit("r[RAX] = (r[RAX] & ~0xff00ull) | (((" FLAGS_NOW " & 0xd5) | 2) << 8);\n"); return 1;
    case M(SAHF): emit("ef = (" FLAGS_NOW " & ~(uint64_t)0xd5) | ((r[RAX] >> 8) & 0xd5); fk = FK_CPU;\n"); return 1;
    case M(CPUID): emit("rc_cpuid(r);\n"); return 1;
    case M(RDTSCP): emit("{ const uint64_t t = rc_tsc(); r[RAX] = (uint32_t)t; r[RDX] = t >> 32; r[RCX] = 0; }\n"); return 1;
    case M(XGETBV): emit("r[RAX] = 7; r[RDX] = 0; /* x87, SSE, AVX state on */\n"); return 1;
    case M(JRCXZ): case M(JECXZ): case M(LOOP): case M(LOOPE): case M(LOOPNE):
        if (d->type != OP_IMM) return 0;
        if (m == M(JRCXZ)) emit("if (!r[RCX]) ");
        else if (m == M(JECXZ)) emit("if (!(uint32_t)r[RCX]) ");
        else if (m == M(LOOP)) emit("if (--r[RCX]) ");
        else emit("if (--r[RCX] && %s" COND ") ", m == M(LOOPNE) ? "!" : "", 4);
        jump(f, off + (uint64_t)d->disp);
        emit("\n");
        return 1;
    case M(MOV): case M(MOVNTI):
        if (d->type == OP_REG && d->kind != RK_GPR) return 0;
        if (s->type == OP_REG && s->kind != RK_GPR) return 0;
        if (!read_op(off, in, s, d->size, &b) || !write_op(off, in, d, b.s, &w)) return 0;
        emit("%s\n", w.s);
        return 1;
    case M(MOVZX):
        if (!gpr(d) || !read_op(off, in, s, s->size, &b) || !write_op(off, in, d, b.s, &w)) return 0;
        emit("%s\n", w.s);
        return 1;
    case M(MOVSX): case M(MOVSXD): {
        if (!gpr(d) || !read_op(off, in, s, s->size, &b)) return 0;
        static const char *const ext[9] = {0, "int8_t", "int16_t", 0, "int32_t", 0, 0, 0, "int64_t"};
        if (!ext[s->size]) return 0;
        const Str v = str("(uint64_t)(int64_t)(%s)(%s)", ext[s->size], b.s);
        if (!write_op(off, in, d, v.s, &w)) return 0;
        emit("%s\n", w.s);
        return 1;
    }
    case M(LEA): {
        int stack;
        if (!gpr(d) || s->type != OP_AGEN || !address(off, in, s, &a, &stack)) return 0;
        if (!write_op(off, in, d, str("%s & 0x%" PRIx64 "ull", a.s, mask(d->size)).s, &w)) return 0;
        emit("%s\n", w.s);
        return 1;
    }
    case M(ADD): case M(SUB): case M(CMP): case M(AND): case M(OR): case M(XOR): case M(TEST): {
        const int size = d->size;
        if (!(gpr(d) || d->type == OP_MEM)) return 0;
        if ((m == M(XOR) || m == M(SUB)) && gpr(d) && gpr(s) && s->reg == d->reg && s->size == size) {
            /* a register with itself: zero and its flags, without reading it */
            if (!write_op(off, in, d, "0", &w)) return 0;
            emit("{ fk = %s; fa = 0; fb = 0; fr = 0; fz = %d; %s }\n", m == M(XOR) ? "FK_LOGIC" : "FK_SUB", size, w.s);
            return 1;
        }
        if (!read_op(off, in, s, size, &b)) return 0;
        const int kind = m == M(ADD) ? 1 : (m == M(SUB) || m == M(CMP)) ? 2 : 3;
        const char *op = m == M(ADD) ? "+" : (m == M(SUB) || m == M(CMP)) ? "-" : (m == M(OR)) ? "|" : (m == M(XOR)) ? "^" : "&";
        emit("{ ");
        Str da;
        int stack = 0;
        if (d->type == OP_MEM) { /* read-modify-write: the address once */
            if (!address(off, in, d, &da, &stack)) return 0;
            emit("const uint64_t ea = %s; const uint64_t a = %s(ea, %d); ", da.s, stack ? "rc_lds" : "rc_ld", size);
        } else {
            if (!read_op(off, in, d, size, &a)) return 0;
            emit("const uint64_t a = %s; ", a.s);
        }
        emit("const uint64_t b = %s; const uint64_t v = (a %s b) & 0x%" PRIx64 "ull; ", b.s, op, mask(size));
        emit("fk = %s; fa = a; fb = b; fr = v; fz = %d; ", kind == 1 ? "FK_ADD" : kind == 2 ? "FK_SUB" : "FK_LOGIC", size);
        if (m != M(CMP) && m != M(TEST)) {
            if (d->type == OP_MEM) emit("%s(ea, %d, v);", stack ? "rc_sts" : "rc_st", size);
            else {
                if (!write_op(off, in, d, "v", &w)) return 0;
                emit("%s", w.s);
            }
        }
        emit(" }\n");
        return 1;
    }
    case M(INC): case M(DEC): case M(NEG): case M(NOT): {
        const int size = d->size;
        emit("{ ");
        Str da;
        int stack = 0;
        if (d->type == OP_MEM) {
            if (!address(off, in, d, &da, &stack)) return 0;
            emit("const uint64_t ea = %s; const uint64_t a = %s(ea, %d); ", da.s, stack ? "rc_lds" : "rc_ld", size);
        } else if (gpr(d)) {
            if (!read_op(off, in, d, size, &a)) return 0;
            emit("const uint64_t a = %s; ", a.s);
        } else {
            return 0;
        }
        const char *calc = m == M(INC) ? "a + 1" : m == M(DEC) ? "a - 1" : m == M(NEG) ? "0 - a" : "~a";
        emit("const uint64_t v = (%s) & 0x%" PRIx64 "ull; ", calc, mask(size));
        if (m == M(INC) || m == M(DEC)) emit("fcf = " COND "; fk = %s; fa = a; fb = 1; fr = v; fz = %d; ", 2, m == M(INC) ? "FK_INC" : "FK_DEC", size);
        if (m == M(NEG)) emit("fk = FK_SUB; fa = 0; fb = a; fr = v; fz = %d; ", size);
        if (d->type == OP_MEM) emit("%s(ea, %d, v);", stack ? "rc_sts" : "rc_st", size);
        else {
            if (!write_op(off, in, d, "v", &w)) return 0;
            emit("%s", w.s);
        }
        emit(" }\n");
        return 1;
    }
    case M(PUSH):
        if (d->size == 2) return 0;
        if (d->type == OP_REG && d->kind != RK_GPR) return 0;
        if (gpr(d) && (elide_candidates >> d->reg & 1) && off == elide_push[d->reg]) { /* the prologue's */
            emit("{ r[RSP] -= 8; " PUSH_SLOT "%02d }\n", d->reg);
            return 1;
        }
        if (!read_op(off, in, d, 8, &b)) return 0;
        emit("{ const uint64_t v = %s; r[RSP] -= 8; rc_sts(r[RSP], 8, v); }\n", b.s);
        return 1;
    case M(POP):
        if (d->type == OP_MEM && d->size == 8) {
            Str ad;
            int stack;
            if (!address(off, in, d, &ad, &stack)) return 0;
            emit("{ const uint64_t v = rc_lds(r[RSP], 8); r[RSP] += 8; %s(%s, 8, v); }\n", stack ? "rc_sts" : "rc_st", ad.s);
            return 1;
        }
        if (!gpr(d) || d->size != 8) return 0;
        if (elide_candidates >> d->reg & 1) { /* on the way to a return: the prologue's slot */
            emit("{ " POP_SLOT "%02d r[RSP] += 8; }\n", d->reg);
            return 1;
        }
        emit("{ const uint64_t v = rc_lds(r[RSP], 8); r[RSP] += 8; r[%d] = v; }\n", d->reg);
        return 1;
    case M(LEAVE):
        if (elide_candidates >> RBP & 1) emit("r[RSP] = r[RBP]; { " POP_SLOT "%02d r[RSP] += 8; }\n", RBP);
        else emit("r[RSP] = r[RBP]; r[RBP] = rc_lds(r[RSP], 8); r[RSP] += 8;\n");
        return 1;
    case M(JMP):
        if (d->type == OP_IMM) {
            jump(f, off + (uint64_t)d->disp);
            emit("\n");
            return 1;
        }
        if (!read_op(off, in, d, 8, &b)) return 0;
        const JumpTable *jt = jump_table_at(off);
        if (jt) { /* its own targets, by the index the table load took */
            emit("{ dn = %s; switch (jt_%" PRIx64 ") {", b.s, jt->load);
            for (uint64_t e = 0; e < jt->entries; ++e)
                emit(" case %" PRIu64 ": if (dn == B + 0x%" PRIx64 "ull) goto L_%" PRIx64 "; break;", e, jt->targets[e], jt->targets[e]);
            emit(" default: break; } %s }\n", dispatch());
            return 1;
        }
        if (register_function) { /* out of the function, the stack as at its entry: a tail call of the function
                                   * the jump last went to, else rc_find's or RcApi.find's (another library's),
                                   * as an indirect call has them */
            emit("{ dn = %s; if (__builtin_expect(r[RSP] == entry_rsp && (dn - B < 0x%" PRIx64 "ull || dn - B >= 0x%" PRIx64
                 "ull), 1)) { static const RcEntry *ic = &rc_none; const RcEntry *e = __atomic_load_n(&ic, __ATOMIC_RELAXED); if "
                 "(__builtin_expect(e->offset != dn - B, 0)) e = rc_miss(&ic, dn, B); if (__builtin_expect(!*e->slot, 1)) { "
                 REGISTER_TAIL_SPILL " return e->abi(c, r[RSP], " REGISTER_ARGUMENTS "); } } %s }\n",
                 b.s, f.start, f.end, dispatch());
            ++stats[1];
            return 1;
        }
        emit("{ dn = %s; %s }\n", b.s, dispatch());
        ++stats[1];
        return 1;
    case M(CALL): {
        if (d->type == OP_IMM) {
            /* A recompiled callee through its slot (the game: the function, a version or a hook's
             * wrapper, or what runs one it does not; the replay answers each call from the record). */
            const uint64_t t = off + (uint64_t)d->disp;
            const int abi = abi_function && !cave_at(t);
            if (annotation(t)) emit("/* %s */ ", annotation(t));
            if (is_setjmp(t)) { /* a longjmp to it comes back to the _setjmp, the registers in c */
                emit("/* %s */ { " SPILL " if (!_setjmp(rs[%d].jump)) { rc->resume(c, &rs[%d], B + 0x%" PRIx64 "ull); "
                     "rc->call(c, B + 0x%" PRIx64 "ull, B + 0x%" PRIx64 "ull); } " RELOAD " fk = FK_CPU; }\n",
                     import_name(t), setjmp_calls, setjmp_calls, next, t, next);
                ++setjmp_calls;
                return 1;
            }
            if (is_recompiled(t) && abi) { /* a_t while the slot holds NULL, else the slot's function */
                emit("{ %s %s rc_st(r[RSP] - 8, 8, B + 0x%" PRIx64 "ull); { RcRet q; "
                     "if (__builtin_expect(!d_%" PRIx64 ", 1)) q = a_%" PRIx64 "(c, r[RSP] - 8, " REGISTER_ARGUMENTS
                     "); else { c->r[RSP] = r[RSP] - 8; " REGISTER_STORES " c->rip = B + 0x%" PRIx64 "ull; d_%" PRIx64
                     "(c); q = rc_back(c); } " REGISTER_CALL_RELOAD " r[RAX] = q.rax; r[RDX] = q.rdx; } %s fk = FK_CPU; }\n",
                     callee_declarations(t).s, holds_hook_site(t) ? REGISTER_HOOKED_CALL_SPILL : REGISTER_CALL_SPILL,
                     next, t, t, t, t, holds_hook_site(t) ? CALLEE_SAVED_RESTORE : "");
                return 1;
            }
            if (is_recompiled(t)) {
                emit("{ extern RcFn d_%" PRIx64 " " HIDDEN "; void f_%" PRIx64 "(GuestCpu *c) " HIDDEN "; " SPILL " c->r[RSP] = r[RSP] - 8; "
                     "rc_st(r[RSP] - 8, 8, B + 0x%" PRIx64 "ull); %s " RELOAD " fk = FK_CPU; }\n", t, t, next,
                     direct_call(t).s);
                return 1;
            }
            b = hook_code ? hook_target(t) : str("B + 0x%" PRIx64 "ull", t);
            if (import_name(t)) emit("/* %s */ ", import_name(t));
            const uint64_t slot = hook_code ? 0 : import_slot(t);
            const int count = slot ? host_arguments(import_name(t)) : -1;
            if (count >= 0) { /* the host function rc->import found in the slot the last time, called as C (RSP
                                * in the GuestCpu: below it, guest code the host function runs). The sync takes
                                * the statement as one, in its order: outside the ABI every register goes to the
                                * GuestCpu before either path (a store on rc->import's path alone leaves the
                                * other's registers dirty with the sync counting them stored), and the host
                                * function's arguments are read before rc->import's path, whose reload would
                                * otherwise count as giving them their values */
                static const char *const names[] = {"r[RDI]", "r[RSI]", "r[RDX]", "r[RCX]", "r[R8]", "r[R9]"};
                Str types = str("%s", count ? "" : "void"), values = str("%s", ""), reads = str("%s", "");
                for (int k = 0; k < count; ++k) {
                    types = str("%s%suint64_t", types.s, k ? ", " : "");
                    values = str("%s%sia%d", values.s, k ? ", " : "", k);
                    reads = str("%s const uint64_t ia%d = %s;", reads.s, k, names[k]);
                }
                emit("{ %sstatic uint64_t h; const uint64_t s = rc_lds(B + 0x%" PRIx64 "ull, 8);%s if (__builtin_expect(!s || "
                     "s != __atomic_load_n(&h, __ATOMIC_RELAXED), 0)) { %s rc->import(c, &h, %s, B + 0x%" PRIx64 "ull, B + 0x%"
                     PRIx64 "ull); %s } else { c->r[RSP] = r[RSP]; r[RAX] = ((uint64_t (*)(%s))s)(%s); } fk = FK_CPU; }\n",
                     abi ? "" : SPILL " ", slot, reads.s, abi ? CALL_SPILL : "", b.s, slot, next, abi ? CALL_RELOAD : RELOAD,
                     types.s, values.s);
                return 1;
            }
            if (slot && abi) { /* the function the slot held the last time, as an indirect call has it: a module's
                                * in the register convention (RcApi.find); else rc->import (a host function,
                                * a module function that holds a hook site or runs another way) */
                emit("{ const uint64_t t = rc_lds(B + 0x%" PRIx64 "ull, 8); " REGISTER_CALL_SPILL " rc_st(r[RSP] - 8, 8, B + 0x%"
                     PRIx64 "ull); { static const RcEntry *ic = &rc_none; const RcEntry *e = __atomic_load_n(&ic, "
                     "__ATOMIC_RELAXED); RcRet q; if (__builtin_expect(e->offset != t - B, 0)) e = rc_miss(&ic, t, B); if "
                     "(__builtin_expect(!*e->check, 1)) q = e->abi(c, r[RSP] - 8, " REGISTER_ARGUMENTS "); else { static "
                     "uint64_t h; " CALLEE_SAVED_SPILL " c->r[RSP] = r[RSP]; " REGISTER_STORES " rc->import(c, &h, %s, B + 0x%"
                     PRIx64 "ull, B + 0x%" PRIx64 "ull); q = rc_back(c); " CALLEE_SAVED_RESTORE " } " REGISTER_CALL_RELOAD
                     " r[RAX] = q.rax; r[RDX] = q.rdx; } fk = FK_CPU; }\n",
                     slot, next, b.s, slot, next);
                return 1;
            }
            if (slot) {
                emit("{ static uint64_t h; " SPILL " rc->import(c, &h, %s, B + 0x%" PRIx64 "ull, B + 0x%" PRIx64 "ull); "
                     RELOAD " fk = FK_CPU; }\n", b.s, slot, next);
                return 1;
            }
            emit("{ const uint64_t t = %s; %s rc->call(c, t, B + 0x%" PRIx64 "ull); %s fk = FK_CPU; }\n", b.s,
                 abi ? HOOKED_CALL_SPILL : SPILL, next, abi ? CALL_RELOAD " " CALLEE_SAVED_RESTORE : RELOAD);
            return 1;
        } else if (!read_op(off, in, d, 8, &b)) {
            return 0;
        }
        if (abi_function) { /* the function the call last went to, else rc_find's or RcApi.find's (another
                             * library's): a_<offset> while its slot holds NULL, the callee-saved registers
                             * stored for one that holds a hook site */
            emit("{ const uint64_t t = %s; " REGISTER_CALL_SPILL " rc_st(r[RSP] - 8, 8, B + 0x%" PRIx64 "ull); { static "
                 "const RcEntry *ic = &rc_none; const RcEntry *e = __atomic_load_n(&ic, __ATOMIC_RELAXED); RcRet q; if "
                 "(__builtin_expect(e->offset != t - B, 0)) e = rc_miss(&ic, t, B); if (__builtin_expect(!*e->check, "
                 "1)) q = e->abi(c, r[RSP] - 8, " REGISTER_ARGUMENTS "); else { " CALLEE_SAVED_SPILL
                 " if (!*e->slot) q = e->abi(c, r[RSP] - 8, " REGISTER_ARGUMENTS "); else { c->r[RSP] = "
                 "r[RSP]; " REGISTER_STORES " rc->call(c, t, B + 0x%" PRIx64 "ull); q = rc_back(c); } "
                 CALLEE_SAVED_RESTORE " } " REGISTER_CALL_RELOAD " r[RAX] = q.rax; r[RDX] = q.rdx; } fk = "
                 "FK_CPU; }\n",
                 b.s, next, next);
            return 1;
        }
        emit("{ const uint64_t t = %s; %s rc->call(c, t, B + 0x%" PRIx64 "ull); %s fk = FK_CPU; }\n", b.s, SPILL, next,
             RELOAD);
        return 1;
    }
    case M(RET): {
        if (register_function) { /* the caller goes on past its call, RSP its own */
            emit("{ " REGISTER_RET " }\n");
            return 1;
        }
        const uint64_t pop = 8 + (in->count && d->type == OP_IMM ? (uint64_t)d->disp : 0);
        emit("{ const uint64_t t = rc_lds(r[RSP], 8); r[RSP] += %" PRIu64 "; %s c->rip = t; return; }\n", pop,
             abi_function ? RET_SPILL : SPILL);
        return 1;
    }
    default:
        return translate_shift_mul(off, in) || translate_extra(off, in) || translate_bits(off, in) || translate_vector(off, in);
    }
}


/* ---- vector instructions (as src/cpu/interp_vec.c does them) ---- */

/* The vector registers the function being generated keeps in locals (xmm[], see generate): the
 * ones its instructions name. A translation that reaches another is a generator bug. */
static uint32_t vused;
static int vlocal(int reg) {
    if (reg < 0 || reg > 15 || !(vused >> reg & 1)) {
        fprintf(stderr, "recompile: xmm%d is not kept in a local\n", reg);
        exit(1);
    }
    return reg;
}
static int vreg(const GuestOp *op) { return op->type == OP_REG && op->kind == RK_VEC && vlocal(op->reg) >= 0; }
static int vwidth(const GuestInsn *in) { return in->vex && in->vl ? in->vl : 16; }

/* A vector operand's value (a GuestVec expression). */
static int vread(uint64_t off, const GuestInsn *in, const GuestOp *op, Str *v) {
    if (vreg(op)) { *v = str("rc_gv(xmm[%d])", op->reg); return 1; }
    if (op->type == OP_MEM) {
        Str a;
        int stack;
        if (!address(off, in, op, &a, &stack)) return 0;
        *v = str("rc_vld(%s, %d)", a.s, op->size);
        return 1;
    }
    if (gpr(op)) {
        Str g;
        if (!read_op(off, in, op, 8, &g)) return 0;
        *v = str("rc_vgpr(%s)", g.s);
        return 1;
    }
    return 0;
}

/* A statement writing GuestVec `v` (`bytes` of it to a register). */
static int vwrite(uint64_t off, const GuestInsn *in, const GuestOp *op, const char *v, int bytes, Str *s) {
    if (vreg(op)) { *s = str("rc_vset(&xmm[%d], %s, %d, %d);", op->reg, v, bytes, in->vex ? 1 : 0); return 1; }
    if (op->type == OP_MEM) {
        Str a;
        int stack;
        if (!address(off, in, op, &a, &stack)) return 0;
        *s = str("rc_vst(%s, %d, %s);", a.s, op->size, v);
        return 1;
    }
    return 0;
}

typedef struct { const GuestOp *dst, *s1, *s2, *imm; } VOps;
static VOps vops3(const GuestInsn *in) {
    VOps o = {&in->op[0], NULL, NULL, NULL};
    if (in->vex && in->count >= 3 && in->op[2].type != OP_IMM) {
        o.s1 = &in->op[1]; o.s2 = &in->op[2];
        if (in->count >= 4 && in->op[3].type == OP_IMM) o.imm = &in->op[3];
    } else {
        o.s1 = &in->op[0]; o.s2 = &in->op[1];
        if (in->count >= 3 && in->op[2].type == OP_IMM) o.imm = &in->op[2];
    }
    return o;
}
static VOps vops1(const GuestInsn *in) {
    VOps o = {&in->op[0], NULL, NULL, NULL};
    int last = in->count - 1;
    if (in->op[last].type == OP_IMM) { o.imm = &in->op[last]; --last; }
    o.s2 = &in->op[last];
    o.s1 = &in->op[0];
    return o;
}

/* Two sources, a lane loop, the destination: `body` sets rr from a and b (i: the lane). */
static int vbinary(uint64_t off, const GuestInsn *in, int lanes, int bytes, int zero, const char *body) {
    const VOps o = vops3(in);
    Str a, b, w;
    if (!vread(off, in, o.s1, &a) || !vread(off, in, o.s2, &b) || !vwrite(off, in, o.dst, "rr", bytes, &w)) return 0;
    emit("{ const GuestVec a = %s, b = %s; GuestVec rr = a; %s for (int i = 0; i < %d; ++i) { %s } %s }\n", a.s, b.s,
         zero ? "memset(&rr, 0, sizeof(rr));" : "", lanes, body, w.s);
    return 1;
}

static int translate_vector(uint64_t off, const GuestInsn *in) {
    const int m = in->mnemonic, w = vwidth(in);
    Str a, b, s;
    switch (m) {
    case M(MOVAPS): case M(MOVUPS): case M(MOVAPD): case M(MOVUPD): case M(MOVDQA): case M(MOVDQU):
    case M(VMOVAPS): case M(VMOVUPS): case M(VMOVAPD): case M(VMOVUPD): case M(VMOVDQA): case M(VMOVDQU):
    case M(LDDQU): case M(VLDDQU): case M(MOVNTPS): case M(VMOVNTPS): case M(MOVNTDQ): case M(VMOVNTDQ):
    case M(MOVNTPD): case M(VMOVNTPD): case M(MOVNTDQA): case M(VMOVNTDQA):
        if (!vread(off, in, &in->op[1], &a) || !vwrite(off, in, &in->op[0], a.s, in->op[0].size, &s)) return 0;
        emit("%s\n", s.s);
        return 1;
    case M(PCMPESTRI): case M(PCMPESTRM): case M(PCMPISTRI): case M(PCMPISTRM):
    case M(VPCMPESTRI): case M(VPCMPESTRM): case M(VPCMPISTRI): case M(VPCMPISTRM): {
        /* rc_pcmpstr (the interpreter's pcmpstr): lengths from rax and rdx (explicit) or the first
         * null element; the index to rcx or the mask to xmm0. */
        if (in->count != 3 || in->op[2].type != OP_IMM || !vread(off, in, &in->op[0], &a) || !vread(off, in, &in->op[1], &b))
            return 0;
        const int imm = (int)(in->op[2].disp & 0xff), count = imm & 1 ? 8 : 16;
        const int explicit_length = m == M(PCMPESTRI) || m == M(PCMPESTRM) || m == M(VPCMPESTRI) || m == M(VPCMPESTRM);
        const int index = m == M(PCMPESTRI) || m == M(PCMPISTRI) || m == M(VPCMPESTRI) || m == M(VPCMPISTRI);
        Str out = {0};
        if (!index) {
            const GuestOp x0 = {.type = OP_REG, .kind = RK_VEC, .reg = 0, .size = 16};
            const Str mask_value = str("rc_pcmpstr_mask(res, %d)", imm);
            if (!vwrite(off, in, &x0, mask_value.s, 16, &out)) return 0;
        }
        emit("{ const GuestVec va = %s, vb = %s; ", a.s, b.s);
        if (explicit_length)
            emit("const int la = rc_pcmpstr_explicit(%s, %d), lb = rc_pcmpstr_explicit(%s, %d); ",
                 in->opsize == 8 ? "(int64_t)r[RAX]" : "(int64_t)(int32_t)r[RAX]", count,
                 in->opsize == 8 ? "(int64_t)r[RDX]" : "(int64_t)(int32_t)r[RDX]", count);
        else
            emit("const int la = rc_pcmpstr_length(&va, %d, %d), lb = rc_pcmpstr_length(&vb, %d, %d); ", imm, count, imm, count);
        emit("const uint32_t res = rc_pcmpstr(&va, &vb, %d, la, lb); ef = (rc_flags(fk, fa, fb, fr, fz, fcf, ef) & "
             "~(uint64_t)F_ARITH) | (res ? F_CF : 0) | (lb < %d ? F_ZF : 0) | (la < %d ? F_SF : 0) | ((res & 1) ? F_OF : 0); "
             "fk = FK_CPU; ", imm, count, count);
        if (index) emit("r[RCX] = res ? (uint32_t)(%s) : %d; }\n", imm & 0x40 ? "31 - __builtin_clz(res)" : "__builtin_ctz(res)", count);
        else emit("%s }\n", out.s);
        return 1;
    }
    case M(MOVSS): case M(VMOVSS): case M(MOVSD): case M(VMOVSD): {
        if (m == M(MOVSD) && !(in->count && vreg(&in->op[0])) && !(in->count >= 2 && vreg(&in->op[1]))) return 0;
        const int bytes = (m == M(MOVSS) || m == M(VMOVSS)) ? 4 : 8;
        const GuestOp *d = &in->op[0];
        if (d->type == OP_MEM) {
            Str ad;
            int stack;
            if (!vread(off, in, &in->op[in->count - 1], &a) || !address(off, in, d, &ad, &stack)) return 0;
            emit("rc_vst(%s, %d, %s);\n", ad.s, bytes, a.s);
            return 1;
        }
        if (!vreg(d)) return 0;
        if (in->count == 3) {
            if (!vread(off, in, &in->op[1], &a) || !vread(off, in, &in->op[2], &b)) return 0;
            emit("{ GuestVec rr = %s; const GuestVec b = %s; memcpy(rr.b, b.b, %d); rc_vset(&xmm[%d], rr, 16, %d); }\n", a.s, b.s,
                 bytes, d->reg, in->vex ? 1 : 0);
            return 1;
        }
        const GuestOp *src = &in->op[1];
        if (src->type == OP_MEM) {
            Str ad;
            int stack;
            if (!address(off, in, src, &ad, &stack)) return 0;
            if (in->vex) emit("rc_vset(&xmm[%d], rc_vld(%s, %d), 16, 1);\n", d->reg, ad.s, bytes);
            else emit("rc_vset(&xmm[%d], rc_vld(%s, %d), 16, 0);\n", d->reg, ad.s, bytes);
            return 1;
        }
        if (!vreg(src)) return 0;
        emit("{ GuestVec rr = rc_gv(xmm[%d]); const GuestVec s = rc_gv(xmm[%d]); memcpy(rr.b, s.b, %d); "
             "rc_vset(&xmm[%d], rr, 16, %d); }\n", d->reg, src->reg, bytes, d->reg, in->vex ? 1 : 0);
        return 1;
    }
    case M(ADDPS): case M(VADDPS): case M(SUBPS): case M(VSUBPS): case M(MULPS): case M(VMULPS):
    case M(DIVPS): case M(VDIVPS): case M(MINPS): case M(VMINPS): case M(MAXPS): case M(VMAXPS):
    case M(ADDSS): case M(VADDSS): case M(SUBSS): case M(VSUBSS): case M(MULSS): case M(VMULSS):
    case M(DIVSS): case M(VDIVSS): case M(MINSS): case M(VMINSS): case M(MAXSS): case M(VMAXSS):
    case M(ADDPD): case M(VADDPD): case M(SUBPD): case M(VSUBPD): case M(MULPD): case M(VMULPD):
    case M(DIVPD): case M(VDIVPD): case M(ADDSD): case M(VADDSD): case M(SUBSD): case M(VSUBSD):
    case M(MULSD): case M(VMULSD): case M(DIVSD): case M(VDIVSD): case M(MINPD): case M(VMINPD): case M(MAXPD):
    case M(VMAXPD): case M(MINSD): case M(VMINSD): case M(MAXSD): case M(VMAXSD): {
        const char *name = ZydisMnemonicGetString((ZydisMnemonic)m);
        const char *base = name[0] == 'v' ? name + 1 : name;
        const int dbl = base[strlen(base) - 1] == 'd', scalar = base[strlen(base) - 2] == 's';
        const char *f = dbl ? "fd" : "f", *t = dbl ? "double" : "float";
        const char *op = !strncmp(base, "add", 3) ? "x + y" : !strncmp(base, "sub", 3) ? "x - y"
                       : !strncmp(base, "mul", 3) ? "x * y" : !strncmp(base, "div", 3) ? "x / y"
                       : !strncmp(base, "min", 3) ? "x < y ? x : y" : "x > y ? x : y";
        const Str body = str("const %s x = a.%s[i], y = b.%s[i]; rr.%s[i] = %s;", t, f, f, f, op);
        return vbinary(off, in, scalar ? 1 : w / (dbl ? 8 : 4), scalar ? 16 : w, 0, body.s);
    }
    case M(ANDPS): case M(VANDPS): case M(ANDPD): case M(VANDPD): case M(PAND): case M(VPAND):
        return vbinary(off, in, 4, w, 0, "rr.q[i] = a.q[i] & b.q[i];");
    case M(ANDNPS): case M(VANDNPS): case M(ANDNPD): case M(VANDNPD): case M(PANDN): case M(VPANDN):
        return vbinary(off, in, 4, w, 0, "rr.q[i] = ~a.q[i] & b.q[i];");
    case M(ORPS): case M(VORPS): case M(ORPD): case M(VORPD): case M(POR): case M(VPOR):
        return vbinary(off, in, 4, w, 0, "rr.q[i] = a.q[i] | b.q[i];");
    case M(XORPS): case M(VXORPS): case M(XORPD): case M(VXORPD): case M(PXOR): case M(VPXOR): {
        const VOps o = vops3(in);
        if (o.s1->type == OP_REG && o.s2->type == OP_REG && o.s1->kind == RK_VEC && o.s2->kind == RK_VEC &&
            o.s1->reg == o.s2->reg) { /* a register with itself: zero, without reading it */
            if (!vwrite(off, in, o.dst, "rr", w, &s)) return 0;
            emit("{ GuestVec rr; memset(&rr, 0, sizeof(rr)); %s }\n", s.s);
            return 1;
        }
        return vbinary(off, in, 4, w, 0, "rr.q[i] = a.q[i] ^ b.q[i];");
    }
    case M(MOVSHDUP): case M(VMOVSHDUP): case M(MOVSLDUP): case M(VMOVSLDUP): {
        if (!vread(off, in, &in->op[1], &a) || !vwrite(off, in, &in->op[0], "rr", w, &s)) return 0;
        const int odd = m == M(MOVSHDUP) || m == M(VMOVSHDUP);
        emit("{ const GuestVec a = %s; GuestVec rr = a; for (int i = 0; i < %d; ++i) rr.d[i] = a.d[(i & ~1) + %d]; %s }\n",
             a.s, w / 4, odd, s.s);
        return 1;
    }
    case M(PADDD): case M(VPADDD): return vbinary(off, in, w / 4, w, 1, "rr.d[i] = a.d[i] + b.d[i];");
    case M(PSUBD): case M(VPSUBD): return vbinary(off, in, w / 4, w, 1, "rr.d[i] = a.d[i] - b.d[i];");
    case M(PADDQ): case M(VPADDQ): return vbinary(off, in, w / 8, w, 1, "rr.q[i] = a.q[i] + b.q[i];");
    case M(PSUBQ): case M(VPSUBQ): return vbinary(off, in, w / 8, w, 1, "rr.q[i] = a.q[i] - b.q[i];");
    case M(PCMPEQB): case M(VPCMPEQB): return vbinary(off, in, w, w, 1, "rr.b[i] = a.b[i] == b.b[i] ? 0xff : 0;");
    case M(PCMPEQD): case M(VPCMPEQD): return vbinary(off, in, w / 4, w, 1, "rr.d[i] = a.d[i] == b.d[i] ? ~0u : 0;");
    case M(PCMPEQQ): case M(VPCMPEQQ): return vbinary(off, in, w / 8, w, 1, "rr.q[i] = a.q[i] == b.q[i] ? ~0ull : 0;");
    case M(HADDPS): case M(VHADDPS): case M(HSUBPS): case M(VHSUBPS): {
        const char *op = (m == M(HADDPS) || m == M(VHADDPS)) ? "+" : "-";
        const Str body = str("const float *x = a.f + i * 4, *y = b.f + i * 4; rr.f[i * 4] = x[0] %s x[1]; "
                             "rr.f[i * 4 + 1] = x[2] %s x[3]; rr.f[i * 4 + 2] = y[0] %s y[1]; rr.f[i * 4 + 3] = y[2] %s y[3];",
                             op, op, op, op);
        return vbinary(off, in, w / 16, w, 1, body.s);
    }
    case M(CMPPS): case M(VCMPPS): case M(CMPPD): case M(VCMPPD): case M(CMPSS): case M(VCMPSS): case M(CMPSD): case M(VCMPSD): {
        const VOps o = vops3(in);
        if (!o.imm) return 0;
        const int predicate = (int)o.imm->disp & 31;
        const int dbl = m == M(CMPPD) || m == M(VCMPPD) || m == M(CMPSD) || m == M(VCMPSD);
        const int scalar = m == M(CMPSS) || m == M(VCMPSS) || m == M(CMPSD) || m == M(VCMPSD);
        const Str body = dbl ? str("rr.q[i] = rc_fcompare(a.fd[i], b.fd[i], %d) ? ~0ull : 0;", predicate)
                             : str("rr.d[i] = rc_fcompare(a.f[i], b.f[i], %d) ? ~0u : 0;", predicate);
        return vbinary(off, in, scalar ? 1 : w / (dbl ? 8 : 4), scalar ? 16 : w, 0, body.s);
    }
    case M(COMISS): case M(VCOMISS): case M(UCOMISS): case M(VUCOMISS): case M(COMISD): case M(VCOMISD):
    case M(UCOMISD): case M(VUCOMISD): {
        const int dbl = m == M(COMISD) || m == M(VCOMISD) || m == M(UCOMISD) || m == M(VUCOMISD);
        if (!vread(off, in, &in->op[0], &a) || !vread(off, in, &in->op[1], &b)) return 0;
        emit("{ const GuestVec a = %s, b = %s; fk = FK_COMIS; fa = rc_dbits(a.%s[0]); fb = rc_dbits(b.%s[0]); }\n",
             a.s, b.s, dbl ? "fd" : "f", dbl ? "fd" : "f");
        return 1;
    }
    case M(SHUFPS): case M(VSHUFPS): {
        const VOps o = vops3(in);
        if (!o.imm) return 0;
        const int imm = (int)o.imm->disp;
        const Str body = str("rr.d[i * 4] = a.d[i * 4 + %d]; rr.d[i * 4 + 1] = a.d[i * 4 + %d]; rr.d[i * 4 + 2] = b.d[i * 4 + %d]; "
                             "rr.d[i * 4 + 3] = b.d[i * 4 + %d];", imm & 3, (imm >> 2) & 3, (imm >> 4) & 3, (imm >> 6) & 3);
        return vbinary(off, in, w / 16, w, 1, body.s);
    }
    case M(PSHUFD): case M(VPSHUFD): case M(VPERMILPS): {
        if (m == M(VPERMILPS) && in->op[2].type != OP_IMM) /* the variable form */
            return vbinary(off, in, w / 16, w, 1, "for (int k = 0; k < 4; ++k) rr.d[i * 4 + k] = a.d[i * 4 + (b.d[i * 4 + k] & 3)];");
        const VOps o = vops1(in);
        if (!o.imm || !vread(off, in, o.s2, &a) || !vwrite(off, in, o.dst, "rr", w, &s)) return 0;
        const int imm = (int)o.imm->disp;
        emit("{ const GuestVec a = %s; GuestVec rr = a; for (int h = 0; h < %d; ++h) { rr.d[h * 4] = a.d[h * 4 + %d]; "
             "rr.d[h * 4 + 1] = a.d[h * 4 + %d]; rr.d[h * 4 + 2] = a.d[h * 4 + %d]; rr.d[h * 4 + 3] = a.d[h * 4 + %d]; } %s }\n",
             a.s, w / 16, imm & 3, (imm >> 2) & 3, (imm >> 4) & 3, (imm >> 6) & 3, s.s);
        return 1;
    }
    case M(MOVMSKPS): case M(VMOVMSKPS): case M(MOVMSKPD): case M(VMOVMSKPD): case M(PMOVMSKB): case M(VPMOVMSKB): {
        if (!vread(off, in, &in->op[1], &a)) return 0;
        Str wr;
        if (!write_op(off, in, &in->op[0], "mask", &wr)) return 0;
        const char *bit = (m == M(MOVMSKPS) || m == M(VMOVMSKPS)) ? "(uint64_t)(a.d[i] >> 31)"
                        : (m == M(MOVMSKPD) || m == M(VMOVMSKPD)) ? "(a.q[i] >> 63)" : "(uint64_t)(a.b[i] >> 7)";
        const int lanes = (m == M(MOVMSKPS) || m == M(VMOVMSKPS)) ? w / 4 : (m == M(MOVMSKPD) || m == M(VMOVMSKPD)) ? w / 8 : w;
        emit("{ const GuestVec a = %s; uint64_t mask = 0; for (int i = 0; i < %d; ++i) mask |= %s << i; %s }\n", a.s, lanes, bit, wr.s);
        return 1;
    }
    case M(MOVHLPS): case M(VMOVHLPS): case M(MOVLHPS): case M(VMOVLHPS): {
        const VOps o = vops3(in);
        if (!vread(off, in, o.s1, &a) || !vread(off, in, o.s2, &b) || !vwrite(off, in, o.dst, "rr", 16, &s)) return 0;
        emit("{ GuestVec rr = %s; const GuestVec b = %s; %s %s }\n", a.s, b.s,
             (m == M(MOVHLPS) || m == M(VMOVHLPS)) ? "rr.q[0] = b.q[1];" : "rr.q[1] = b.q[0];", s.s);
        return 1;
    }
    case M(MOVD): case M(VMOVD): case M(MOVQ): case M(VMOVQ): {
        const GuestOp *d = &in->op[0], *src = &in->op[1];
        const int bytes = (m == M(MOVD) || m == M(VMOVD)) ? 4 : 8;
        if (!vread(off, in, src, &a)) return 0;
        if (vreg(d)) {
            emit("{ GuestVec rr; memset(&rr, 0, sizeof(rr)); const GuestVec s = %s; memcpy(rr.b, s.b, %d); rc_vset(&xmm[%d], rr, 16, %d); }\n",
                 a.s, bytes, d->reg, in->vex ? 1 : 0);
            return 1;
        }
        if (d->type == OP_MEM) {
            Str ad;
            int stack;
            if (!address(off, in, d, &ad, &stack)) return 0;
            emit("rc_vst(%s, %d, %s);\n", ad.s, bytes, a.s);
            return 1;
        }
        Str wr;
        if (!write_op(off, in, d, bytes == 4 ? "s.d[0]" : "s.q[0]", &wr)) return 0;
        emit("{ const GuestVec s = %s; %s }\n", a.s, wr.s);
        return 1;
    }
    case M(DPPS): case M(VDPPS): { /* the hardware's pairwise order: (p0 + p1) + (p2 + p3) */
        const VOps o = vops3(in);
        if (!o.imm) return 0;
        const int imm = (int)o.imm->disp & 0xff;
        const Str body = str("float p[4]; for (int k = 0; k < 4; ++k) p[k] = (%d & (0x10 << k)) ? a.f[i * 4 + k] * b.f[i * 4 + k] : 0.0f; "
                             "const float sum = (p[0] + p[1]) + (p[2] + p[3]); "
                             "for (int k = 0; k < 4; ++k) rr.f[i * 4 + k] = (%d & (1 << k)) ? sum : 0.0f;", imm, imm);
        return vbinary(off, in, w / 16, w, 1, body.s);
    }
    case M(INSERTPS): case M(VINSERTPS): {
        const VOps o = vops3(in);
        if (!o.imm || !vread(off, in, o.s1, &a) || !vread(off, in, o.s2, &b) || !vwrite(off, in, o.dst, "rr", 16, &s)) return 0;
        const int imm = (int)o.imm->disp & 0xff;
        emit("{ GuestVec rr = %s; const GuestVec b = %s; rr.d[%d] = b.d[%d];", a.s, b.s, (imm >> 4) & 3,
             o.s2->type == OP_MEM ? 0 : (imm >> 6) & 3);
        for (int i = 0; i < 4; ++i) if (imm & (1 << i)) emit(" rr.d[%d] = 0;", i);
        emit(" %s }\n", s.s);
        return 1;
    }
    case M(BLENDVPS): case M(VBLENDVPS): case M(BLENDVPD): case M(VBLENDVPD): case M(PBLENDVB): case M(VPBLENDVB): {
        Str mk;
        if (in->vex) {
            if (in->count < 4 || !vread(off, in, &in->op[1], &a) || !vread(off, in, &in->op[2], &b) ||
                !vread(off, in, &in->op[3], &mk)) return 0;
        } else {
            if (!vread(off, in, &in->op[0], &a) || !vread(off, in, &in->op[1], &b)) return 0;
            mk = str("rc_gv(xmm[%d])", vlocal(0));
        }
        if (!vwrite(off, in, &in->op[0], "rr", w, &s)) return 0;
        const int es = (m == M(BLENDVPS) || m == M(VBLENDVPS)) ? 4 : (m == M(BLENDVPD) || m == M(VBLENDVPD)) ? 8 : 1;
        emit("{ const GuestVec rr = rc_blendv(%s, %s, %s, %d); %s }\n", a.s, b.s, mk.s, es, s.s);
        return 1;
    }
    case M(BLENDPS): case M(VBLENDPS): case M(BLENDPD): case M(VBLENDPD): case M(PBLENDW): case M(VPBLENDW): case M(VPBLENDD): {
        const VOps o = vops3(in);
        if (!o.imm || !vread(off, in, o.s1, &a) || !vread(off, in, o.s2, &b) || !vwrite(off, in, o.dst, "rr", w, &s)) return 0;
        const int imm = (int)o.imm->disp & 0xff;
        const int es = (m == M(BLENDPS) || m == M(VBLENDPS) || m == M(VPBLENDD)) ? 4 : (m == M(BLENDPD) || m == M(VBLENDPD)) ? 8 : 2;
        emit("{ GuestVec rr = %s; const GuestVec b = %s;", a.s, b.s);
        for (int i = 0; i < w / es; ++i)
            if (imm & (1 << (es == 2 ? (i & 7) : i))) emit(" memcpy(rr.b + %d, b.b + %d, %d);", i * es, i * es, es);
        emit(" %s }\n", s.s);
        return 1;
    }
    case M(SQRTPS): case M(VSQRTPS): case M(RSQRTPS): case M(VRSQRTPS): case M(RCPPS): case M(VRCPPS): case M(SQRTPD): case M(VSQRTPD): {
        const VOps o = vops1(in);
        if (o.imm || !vread(off, in, o.s2, &a) || !vwrite(off, in, o.dst, "rr", w, &s)) return 0;
        if (m == M(RCPPS) || m == M(VRCPPS) || m == M(RSQRTPS) || m == M(VRSQRTPS)) {
            emit("{ const GuestVec rr = rc_rcpps(%s, %d, %d); %s }\n", a.s, m == M(RSQRTPS) || m == M(VRSQRTPS), w, s.s);
            return 1;
        }
        const char *body = (m == M(SQRTPD) || m == M(VSQRTPD)) ? "for (int i = 0; i < %d; ++i) rr.fd[i] = rc_sqrt(x.fd[i]);"
                         : (m == M(SQRTPS) || m == M(VSQRTPS)) ? "for (int i = 0; i < %d; ++i) rr.f[i] = rc_sqrtf(x.f[i]);"
                         : (m == M(RCPPS) || m == M(VRCPPS)) ? "for (int i = 0; i < %d; ++i) rr.f[i] = 1.0f / x.f[i];"
                         : "for (int i = 0; i < %d; ++i) rr.f[i] = 1.0f / rc_sqrtf(x.f[i]);";
        emit("{ const GuestVec x = %s; GuestVec rr; memset(&rr, 0, sizeof(rr)); ", a.s);
        emit(body, (m == M(SQRTPD) || m == M(VSQRTPD)) ? w / 8 : w / 4);
        emit(" %s }\n", s.s);
        return 1;
    }
    case M(SQRTSS): case M(VSQRTSS): case M(RSQRTSS): case M(VRSQRTSS): case M(RCPSS): case M(VRCPSS): case M(SQRTSD): case M(VSQRTSD): {
        const VOps o = vops3(in);
        if (o.imm || !vread(off, in, o.s1, &a) || !vread(off, in, o.s2, &b) || !vwrite(off, in, o.dst, "rr", 16, &s)) return 0;
        const char *calc = (m == M(SQRTSD) || m == M(VSQRTSD)) ? "rr.fd[0] = rc_sqrt(x.fd[0]);"
                         : (m == M(SQRTSS) || m == M(VSQRTSS)) ? "rr.f[0] = rc_sqrtf(x.f[0]);"
                         : (m == M(RCPSS) || m == M(VRCPSS)) ? "rr.f[0] = rc_rcpf(x.f[0]);" : "rr.f[0] = rc_rsqrtf(x.f[0]);";
        emit("{ GuestVec rr = %s; const GuestVec x = %s; %s %s }\n", a.s, b.s, calc, s.s);
        return 1;
    }
    case M(CVTSI2SS): case M(VCVTSI2SS): case M(CVTSI2SD): case M(VCVTSI2SD): {
        const VOps o = vops3(in);
        Str g;
        if (o.imm || (o.s2->size != 4 && o.s2->size != 8) || !vread(off, in, o.s1, &a) || !read_op(off, in, o.s2, o.s2->size, &g) ||
            !vwrite(off, in, o.dst, "rr", 16, &s)) return 0;
        emit("{ GuestVec rr = %s; const int64_t x = (int64_t)(%s)(%s); %s %s }\n", a.s, o.s2->size == 8 ? "int64_t" : "int32_t", g.s,
             (m == M(CVTSI2SS) || m == M(VCVTSI2SS)) ? "rr.f[0] = (float)x;" : "rr.fd[0] = (double)x;", s.s);
        return 1;
    }
    case M(CVTTSS2SI): case M(VCVTTSS2SI): case M(CVTSS2SI): case M(VCVTSS2SI): case M(CVTTSD2SI): case M(VCVTTSD2SI):
    case M(CVTSD2SI): case M(VCVTSD2SI): {
        const GuestOp *d = &in->op[0];
        Str wr;
        if (!gpr(d) || (d->size != 4 && d->size != 8) || !vread(off, in, &in->op[1], &a)) return 0;
        const int truncate = m == M(CVTTSS2SI) || m == M(VCVTTSS2SI) || m == M(CVTTSD2SI) || m == M(VCVTTSD2SI);
        const int dbl = m == M(CVTTSD2SI) || m == M(VCVTTSD2SI) || m == M(CVTSD2SI) || m == M(VCVTSD2SI);
        if (!write_op(off, in, d, d->size == 8 ? str("(uint64_t)rc_f2i64(x, %d, c->mxcsr)", truncate).s
                                               : str("(uint32_t)rc_f2i32(x, %d, c->mxcsr)", truncate).s, &wr)) return 0;
        emit("{ const GuestVec sv = %s; const double x = sv.%s[0]; %s }\n", a.s, dbl ? "fd" : "f", wr.s);
        return 1;
    }
    case M(CVTDQ2PS): case M(VCVTDQ2PS): case M(CVTPS2DQ): case M(VCVTPS2DQ): case M(CVTTPS2DQ): case M(VCVTTPS2DQ): {
        if (!vread(off, in, &in->op[1], &a) || !vwrite(off, in, &in->op[0], "rr", w, &s)) return 0;
        if (m != M(CVTDQ2PS) && m != M(VCVTDQ2PS)) {
            emit("{ const GuestVec x = %s; const GuestVec rr = rc_cvtps2dq(x, %d, %d, c->mxcsr); %s }\n", a.s, w,
                 m == M(CVTTPS2DQ) || m == M(VCVTTPS2DQ), s.s);
            return 1;
        }
        const char *calc = "rr.f[i] = (float)x.sd[i];";
        emit("{ const GuestVec x = %s; GuestVec rr; memset(&rr, 0, sizeof(rr)); for (int i = 0; i < %d; ++i) %s %s }\n", a.s, w / 4, calc, s.s);
        return 1;
    }
    case M(VBROADCASTSS): case M(VBROADCASTSD): case M(VBROADCASTF128): case M(VPBROADCASTD): case M(VPBROADCASTQ):
    case M(VPBROADCASTB): case M(VPBROADCASTW): {
        if (!vread(off, in, &in->op[1], &a) || !vwrite(off, in, &in->op[0], "rr", w, &s)) return 0;
        const int es = (m == M(VBROADCASTSS) || m == M(VPBROADCASTD)) ? 4 : (m == M(VBROADCASTSD) || m == M(VPBROADCASTQ)) ? 8
                     : m == M(VPBROADCASTB) ? 1 : m == M(VPBROADCASTW) ? 2 : 16;
        emit("{ const GuestVec x = %s; GuestVec rr; memset(&rr, 0, sizeof(rr)); for (int i = 0; i < %d; i += %d) memcpy(rr.b + i, x.b, %d); %s }\n",
             a.s, w, es, es, s.s);
        return 1;
    }
    case M(UNPCKLPS): case M(VUNPCKLPS): case M(UNPCKHPS): case M(VUNPCKHPS):
        return vbinary(off, in, w / 16, w, 1, (m == M(UNPCKLPS) || m == M(VUNPCKLPS))
            ? "rr.d[i * 4] = a.d[i * 4]; rr.d[i * 4 + 1] = b.d[i * 4]; rr.d[i * 4 + 2] = a.d[i * 4 + 1]; rr.d[i * 4 + 3] = b.d[i * 4 + 1];"
            : "rr.d[i * 4] = a.d[i * 4 + 2]; rr.d[i * 4 + 1] = b.d[i * 4 + 2]; rr.d[i * 4 + 2] = a.d[i * 4 + 3]; rr.d[i * 4 + 3] = b.d[i * 4 + 3];");
    case M(UNPCKLPD): case M(VUNPCKLPD): case M(UNPCKHPD): case M(VUNPCKHPD):
        return vbinary(off, in, w / 16, w, 1, (m == M(UNPCKLPD) || m == M(VUNPCKLPD))
            ? "rr.q[i * 2] = a.q[i * 2]; rr.q[i * 2 + 1] = b.q[i * 2];" : "rr.q[i * 2] = a.q[i * 2 + 1]; rr.q[i * 2 + 1] = b.q[i * 2 + 1];");
    case M(PSLLW): case M(VPSLLW): case M(PSLLD): case M(VPSLLD): case M(PSLLQ): case M(VPSLLQ): case M(PSRLW): case M(VPSRLW):
    case M(PSRLD): case M(VPSRLD): case M(PSRLQ): case M(VPSRLQ): case M(PSRAW): case M(VPSRAW): case M(PSRAD): case M(VPSRAD):
    case M(PSLLDQ): case M(VPSLLDQ): case M(PSRLDQ): case M(VPSRLDQ): {
        const GuestOp *src = in->vex ? &in->op[1] : &in->op[0], *cnt = in->vex ? &in->op[2] : &in->op[1];
        Str n;
        if (!vread(off, in, src, &a) || !vwrite(off, in, &in->op[0], "rr", w, &s)) return 0;
        if (cnt->type == OP_IMM) n = str("%" PRIu64 "ull", (uint64_t)cnt->disp & 0xff);
        else if (!vread(off, in, cnt, &b)) return 0;
        else {
            /* A count from a register or memory: rc_vshift, one instruction per half on x86-64
             * (the byte shifts take only an immediate). */
            const int op = (m == M(PSLLW) || m == M(VPSLLW) || m == M(PSLLD) || m == M(VPSLLD) || m == M(PSLLQ) || m == M(VPSLLQ)) ? 0
                         : (m == M(PSRAW) || m == M(VPSRAW) || m == M(PSRAD) || m == M(VPSRAD)) ? 2 : 1;
            const int lane = (m == M(PSLLW) || m == M(VPSLLW) || m == M(PSRLW) || m == M(VPSRLW) || m == M(PSRAW) || m == M(VPSRAW)) ? 2
                           : (m == M(PSLLQ) || m == M(VPSLLQ) || m == M(PSRLQ) || m == M(VPSRLQ)) ? 8 : 4;
            emit("{ const GuestVec rr = rc_vshift(%s, %s, %d, %d, %d); %s }\n", a.s, b.s, op, lane, w, s.s);
            return 1;
        }
        const char *body;
        switch (m) {
        case M(PSLLW): case M(VPSLLW): body = "for (int i = 0; i < %d / 2; ++i) rr.w[i] = n > 15 ? 0 : (uint16_t)(x.w[i] << n);"; break;
        case M(PSLLD): case M(VPSLLD): body = "for (int i = 0; i < %d / 4; ++i) rr.d[i] = n > 31 ? 0 : x.d[i] << n;"; break;
        case M(PSLLQ): case M(VPSLLQ): body = "for (int i = 0; i < %d / 8; ++i) rr.q[i] = n > 63 ? 0 : x.q[i] << n;"; break;
        case M(PSRLW): case M(VPSRLW): body = "for (int i = 0; i < %d / 2; ++i) rr.w[i] = n > 15 ? 0 : (uint16_t)(x.w[i] >> n);"; break;
        case M(PSRLD): case M(VPSRLD): body = "for (int i = 0; i < %d / 4; ++i) rr.d[i] = n > 31 ? 0 : x.d[i] >> n;"; break;
        case M(PSRLQ): case M(VPSRLQ): body = "for (int i = 0; i < %d / 8; ++i) rr.q[i] = n > 63 ? 0 : x.q[i] >> n;"; break;
        case M(PSRAW): case M(VPSRAW): body = "for (int i = 0; i < %d / 2; ++i) rr.sw[i] = (int16_t)(x.sw[i] >> (n > 15 ? 15 : n));"; break;
        case M(PSRAD): case M(VPSRAD): body = "for (int i = 0; i < %d / 4; ++i) rr.sd[i] = x.sd[i] >> (n > 31 ? 31 : n);"; break;
        case M(PSLLDQ): case M(VPSLLDQ):
            body = "for (int h = 0; h < %d / 16; ++h) for (int i = 0; i < 16; ++i) rr.b[h * 16 + i] = (uint64_t)i >= n ? x.b[h * 16 + i - n] : 0;"; break;
        default:
            body = "for (int h = 0; h < %d / 16; ++h) for (int i = 0; i < 16; ++i) rr.b[h * 16 + i] = i + n < 16 ? x.b[h * 16 + i + n] : 0;"; break;
        }
        emit("{ const GuestVec x = %s; const uint64_t n = %s; GuestVec rr; memset(&rr, 0, sizeof(rr)); ", a.s, n.s);
        emit(body, w);
        emit(" %s }\n", s.s);
        return 1;
    }
    case M(EXTRACTPS): case M(VEXTRACTPS): case M(PEXTRD): case M(VPEXTRD): case M(PEXTRQ): case M(VPEXTRQ):
    case M(PEXTRW): case M(VPEXTRW): case M(PEXTRB): case M(VPEXTRB): {
        if (in->count < 3 || in->op[2].type != OP_IMM || !vread(off, in, &in->op[1], &a)) return 0;
        const int imm = (int)in->op[2].disp;
        const Str v = (m == M(PEXTRQ) || m == M(VPEXTRQ)) ? str("x.q[%d]", imm & 1) : (m == M(PEXTRW) || m == M(VPEXTRW)) ? str("x.w[%d]", imm & 7)
                    : (m == M(PEXTRB) || m == M(VPEXTRB)) ? str("x.b[%d]", imm & 15) : str("x.d[%d]", imm & 3);
        GuestOp dst = in->op[0];
        if (dst.type == OP_REG && dst.size < 4) dst.size = 4;
        Str wr;
        if (!write_op(off, in, &dst, v.s, &wr)) return 0;
        emit("{ const GuestVec x = %s; %s }\n", a.s, wr.s);
        return 1;
    }
    case M(PINSRD): case M(VPINSRD): case M(PINSRQ): case M(VPINSRQ): case M(PINSRW): case M(VPINSRW): case M(PINSRB): case M(VPINSRB): {
        const VOps o = vops3(in);
        Str g;
        if (!o.imm || !vread(off, in, o.s1, &a) || !read_op(off, in, o.s2, o.s2->size, &g) || !vwrite(off, in, o.dst, "rr", 16, &s)) return 0;
        const int imm = (int)o.imm->disp;
        const Str set = (m == M(PINSRQ) || m == M(VPINSRQ)) ? str("rr.q[%d] = v;", imm & 1)
                      : (m == M(PINSRW) || m == M(VPINSRW)) ? str("rr.w[%d] = (uint16_t)v;", imm & 7)
                      : (m == M(PINSRB) || m == M(VPINSRB)) ? str("rr.b[%d] = (uint8_t)v;", imm & 15) : str("rr.d[%d] = (uint32_t)v;", imm & 3);
        emit("{ GuestVec rr = %s; const uint64_t v = %s; %s %s }\n", a.s, g.s, set.s, s.s);
        return 1;
    }
    case M(PALIGNR): case M(VPALIGNR): {
        const VOps o = vops3(in);
        if (!o.imm) return 0;
        const Str body = str("uint8_t cat[32]; memcpy(cat, b.b + i * 16, 16); memcpy(cat + 16, a.b + i * 16, 16); "
                             "for (int k = 0; k < 16; ++k) rr.b[i * 16 + k] = k + %d < 32 ? cat[k + %d] : 0;",
                             (int)o.imm->disp & 0xff, (int)o.imm->disp & 0xff);
        return vbinary(off, in, w / 16, w, 1, body.s);
    }
    case M(PSHUFB): case M(VPSHUFB): { /* rc_pshufb: a table lookup per half */
        const VOps o = vops3(in);
        if (!vread(off, in, o.s1, &a) || !vread(off, in, o.s2, &b) || !vwrite(off, in, o.dst, "rr", w, &s)) return 0;
        emit("{ const GuestVec rr = rc_pshufb(%s, %s, %d); %s }\n", a.s, b.s, w, s.s);
        return 1;
    }
    case M(PUNPCKLBW): case M(VPUNPCKLBW): case M(PUNPCKHBW): case M(VPUNPCKHBW): case M(PUNPCKLWD): case M(VPUNPCKLWD):
    case M(PUNPCKHWD): case M(VPUNPCKHWD): case M(PUNPCKLDQ): case M(VPUNPCKLDQ): case M(PUNPCKHDQ): case M(VPUNPCKHDQ):
    case M(PUNPCKLQDQ): case M(VPUNPCKLQDQ): case M(PUNPCKHQDQ): case M(VPUNPCKHQDQ): {
        const char *name = ZydisMnemonicGetString((ZydisMnemonic)m);
        const char *kind = strstr(name, "unpck") + 5; /* lbw, hwd, ldq, hqdq... */
        const int high = kind[0] == 'h';
        const int es = !strcmp(kind + 1, "bw") ? 1 : !strcmp(kind + 1, "wd") ? 2 : !strcmp(kind + 1, "dq") ? 4 : 8;
        const Str body = str("for (int k = 0; k < %d; ++k) { memcpy(rr.b + i * 16 + k * %d, a.b + i * 16 + %d + k * %d, %d); "
                             "memcpy(rr.b + i * 16 + k * %d + %d, b.b + i * 16 + %d + k * %d, %d); }",
                             8 / es, 2 * es, high ? 8 : 0, es, es, 2 * es, es, high ? 8 : 0, es, es);
        return vbinary(off, in, w / 16, w, 1, body.s);
    }
    case M(PACKSSDW): case M(VPACKSSDW): case M(PACKUSDW): case M(VPACKUSDW):
    case M(PACKSSWB): case M(VPACKSSWB): case M(PACKUSWB): case M(VPACKUSWB): {
        const int dw = m == M(PACKSSDW) || m == M(VPACKSSDW) || m == M(PACKUSDW) || m == M(VPACKUSDW);
        const int us = m == M(PACKUSDW) || m == M(VPACKUSDW) || m == M(PACKUSWB) || m == M(VPACKUSWB);
        const Str body = dw ? str("for (int k = 0; k < 4; ++k) { rr.%s[i * 8 + k] = %s(a.sd[i * 4 + k]); rr.%s[i * 8 + 4 + k] = %s(b.sd[i * 4 + k]); }",
                                  us ? "w" : "sw", us ? "rc_usat16" : "rc_sat16", us ? "w" : "sw", us ? "rc_usat16" : "rc_sat16")
                            : str("for (int k = 0; k < 8; ++k) { rr.%s[i * 16 + k] = %s(a.sw[i * 8 + k]); rr.%s[i * 16 + 8 + k] = %s(b.sw[i * 8 + k]); }",
                                  us ? "b" : "sb", us ? "rc_usat8" : "rc_sat8", us ? "b" : "sb", us ? "rc_usat8" : "rc_sat8");
        return vbinary(off, in, w / 16, w, 1, body.s);
    }
    case M(MOVLPS): case M(VMOVLPS): case M(MOVLPD): case M(VMOVLPD): case M(MOVHPS): case M(VMOVHPS): case M(MOVHPD): case M(VMOVHPD): {
        const int high = m == M(MOVHPS) || m == M(VMOVHPS) || m == M(MOVHPD) || m == M(VMOVHPD);
        const GuestOp *d = &in->op[0], *last = &in->op[in->count - 1];
        Str ad;
        int stack;
        if (d->type == OP_MEM) {
            if (!vread(off, in, &in->op[1], &a) || !address(off, in, d, &ad, &stack)) return 0;
            emit("{ const GuestVec x = %s; rc_st(%s, 8, x.q[%d]); }\n", a.s, ad.s, high);
            return 1;
        }
        if (!vreg(d) || last->type != OP_MEM || !vread(off, in, in->count == 3 ? &in->op[1] : d, &a) ||
            !address(off, in, last, &ad, &stack) || !vwrite(off, in, d, "rr", 16, &s)) return 0;
        emit("{ GuestVec rr = %s; rr.q[%d] = rc_ld(%s, 8); %s }\n", a.s, high, ad.s, s.s);
        return 1;
    }
    case M(VINSERTF128): case M(VINSERTI128):
        if (in->count < 4 || in->op[3].type != OP_IMM || !vread(off, in, &in->op[1], &a) || !vread(off, in, &in->op[2], &b) ||
            !vwrite(off, in, &in->op[0], "rr", 32, &s)) return 0;
        emit("{ GuestVec rr = %s; const GuestVec x = %s; memcpy(rr.b + %d, x.b, 16); %s }\n", a.s, b.s, (in->op[3].disp & 1) ? 16 : 0, s.s);
        return 1;
    case M(VEXTRACTF128): case M(VEXTRACTI128):
        if (in->count < 3 || in->op[2].type != OP_IMM || !vread(off, in, &in->op[1], &a) || !vwrite(off, in, &in->op[0], "rr", 16, &s)) return 0;
        emit("{ const GuestVec x = %s; GuestVec rr; memset(&rr, 0, sizeof(rr)); memcpy(rr.b, x.b + %d, 16); %s }\n", a.s,
             (in->op[2].disp & 1) ? 16 : 0, s.s);
        return 1;
    case M(VPERM2F128): case M(VPERM2I128): {
        if (in->count < 4 || in->op[3].type != OP_IMM || !vread(off, in, &in->op[1], &a) || !vread(off, in, &in->op[2], &b) ||
            !vwrite(off, in, &in->op[0], "rr", 32, &s)) return 0;
        const int imm = (int)in->op[3].disp;
        emit("{ const GuestVec a = %s, b = %s; GuestVec rr;", a.s, b.s);
        for (int h = 0; h < 2; ++h) {
            const int sel = (imm >> (h * 4)) & 0xf;
            if (sel & 8) emit(" memset(rr.b + %d, 0, 16);", h * 16);
            else emit(" memcpy(rr.b + %d, %s.b + %d, 16);", h * 16, (sel & 2) ? "b" : "a", (sel & 1) ? 16 : 0);
        }
        emit(" %s }\n", s.s);
        return 1;
    }
    case M(PTEST): case M(VPTEST): case M(VTESTPS): {
        if (!vread(off, in, &in->op[0], &a) || !vread(off, in, &in->op[1], &b)) return 0;
        const uint64_t lane = m == M(VTESTPS) ? UINT64_C(0x8000000080000000) : ~UINT64_C(0);
        emit("{ const GuestVec a = %s, b = %s; uint64_t and_ = 0, andn = 0; for (int i = 0; i < %d; ++i) { and_ |= a.q[i] & b.q[i] & 0x%" PRIx64 "ull; "
             "andn |= ~a.q[i] & b.q[i] & 0x%" PRIx64 "ull; } ef = (" FLAGS_NOW " & ~(uint64_t)F_ARITH) | (and_ ? 0 : F_ZF) | (andn ? 0 : F_CF); "
             "fk = FK_CPU; }\n", a.s, b.s, w / 8, lane, lane);
        return 1;
    }
    case M(CVTSS2SD): case M(VCVTSS2SD): case M(CVTSD2SS): case M(VCVTSD2SS): {
        const VOps o = vops3(in);
        if (o.imm || !vread(off, in, o.s1, &a) || !vread(off, in, o.s2, &b) || !vwrite(off, in, o.dst, "rr", 16, &s)) return 0;
        emit("{ GuestVec rr = %s; const GuestVec x = %s; %s %s }\n", a.s, b.s,
             (m == M(CVTSS2SD) || m == M(VCVTSS2SD)) ? "rr.fd[0] = x.f[0];" : "rr.f[0] = (float)x.fd[0];", s.s);
        return 1;
    }
    case M(ROUNDSS): case M(VROUNDSS): case M(ROUNDSD): case M(VROUNDSD): {
        const VOps o = vops3(in);
        if (!o.imm || !vread(off, in, o.s1, &a) || !vread(off, in, o.s2, &b) || !vwrite(off, in, o.dst, "rr", 16, &s)) return 0;
        const int imm = (int)o.imm->disp;
        emit("{ GuestVec rr = %s; const GuestVec x = %s; %s %s }\n", a.s, b.s, (m == M(ROUNDSD) || m == M(VROUNDSD))
             ? str("rr.fd[0] = rc_round_mode(x.fd[0], %d, c->mxcsr);", imm).s : str("rr.f[0] = rc_round_modef(x.f[0], %d, c->mxcsr);", imm).s, s.s);
        return 1;
    }
    case M(ROUNDPS): case M(VROUNDPS): case M(ROUNDPD): case M(VROUNDPD): {
        const VOps o = vops1(in);
        if (!o.imm || !vread(off, in, o.s2, &a) || !vwrite(off, in, o.dst, "rr", w, &s)) return 0;
        const int imm = (int)o.imm->disp, dbl = m == M(ROUNDPD) || m == M(VROUNDPD);
        emit("{ const GuestVec x = %s; const GuestVec rr = rc_round_packed(x, %d, %d, %d, c->mxcsr); %s }\n", a.s, dbl, w, imm, s.s);
        return 1;
    }
    case M(PMOVZXBW): case M(VPMOVZXBW): case M(PMOVZXBD): case M(VPMOVZXBD): case M(PMOVZXWD): case M(VPMOVZXWD):
    case M(PMOVZXDQ): case M(VPMOVZXDQ): case M(PMOVSXBW): case M(VPMOVSXBW): case M(PMOVSXWD): case M(VPMOVSXWD):
    case M(PMOVSXDQ): case M(VPMOVSXDQ): case M(PMOVSXBD): case M(VPMOVSXBD): {
        if (!vread(off, in, &in->op[1], &a) || !vwrite(off, in, &in->op[0], "rr", w, &s)) return 0;
        const char *body;
        int lanes;
        switch (m) {
        case M(PMOVZXBW): case M(VPMOVZXBW): body = "rr.w[i] = x.b[i];"; lanes = w / 2; break;
        case M(PMOVZXBD): case M(VPMOVZXBD): body = "rr.d[i] = x.b[i];"; lanes = w / 4; break;
        case M(PMOVZXWD): case M(VPMOVZXWD): body = "rr.d[i] = x.w[i];"; lanes = w / 4; break;
        case M(PMOVZXDQ): case M(VPMOVZXDQ): body = "rr.q[i] = x.d[i];"; lanes = w / 8; break;
        case M(PMOVSXBW): case M(VPMOVSXBW): body = "rr.sw[i] = x.sb[i];"; lanes = w / 2; break;
        case M(PMOVSXWD): case M(VPMOVSXWD): body = "rr.sd[i] = x.sw[i];"; lanes = w / 4; break;
        case M(PMOVSXDQ): case M(VPMOVSXDQ): body = "rr.sq[i] = x.sd[i];"; lanes = w / 8; break;
        default: body = "rr.sd[i] = x.sb[i];"; lanes = w / 4; break;
        }
        emit("{ const GuestVec x = %s; GuestVec rr; memset(&rr, 0, sizeof(rr)); for (int i = 0; i < %d; ++i) %s %s }\n", a.s, lanes, body, s.s);
        return 1;
    }
    case M(SHUFPD): case M(VSHUFPD): {
        const VOps o = vops3(in);
        if (!o.imm) return 0;
        const int imm = (int)o.imm->disp;
        const Str body = str("rr.q[i * 2] = a.q[i * 2 + ((%d >> (i * 2)) & 1)]; rr.q[i * 2 + 1] = b.q[i * 2 + ((%d >> (i * 2 + 1)) & 1)];", imm, imm);
        return vbinary(off, in, w / 16, w, 1, body.s);
    }
    case M(VPERMILPD): case M(PSHUFLW): case M(VPSHUFLW): case M(PSHUFHW): case M(VPSHUFHW): {
        if (m == M(VPERMILPD) && in->op[2].type != OP_IMM) /* the variable form */
            return vbinary(off, in, w / 16, w, 1, "for (int k = 0; k < 2; ++k) rr.q[i * 2 + k] = a.q[i * 2 + ((b.q[i * 2 + k] >> 1) & 1)];");
        const VOps o = vops1(in);
        if (!o.imm || !vread(off, in, o.s2, &a) || !vwrite(off, in, o.dst, "rr", w, &s)) return 0;
        const int imm = (int)o.imm->disp;
        const Str body = m == M(VPERMILPD) ? str("for (int k = 0; k < 2; ++k) rr.q[h * 2 + k] = a.q[h * 2 + ((%d >> (h * 2 + k)) & 1)];", imm)
                       : (m == M(PSHUFLW) || m == M(VPSHUFLW)) ? str("for (int k = 0; k < 4; ++k) rr.w[h * 8 + k] = a.w[h * 8 + ((%d >> (k * 2)) & 3)];", imm)
                       : str("for (int k = 0; k < 4; ++k) rr.w[h * 8 + 4 + k] = a.w[h * 8 + 4 + ((%d >> (k * 2)) & 3)];", imm);
        emit("{ const GuestVec a = %s; GuestVec rr = a; for (int h = 0; h < %d; ++h) { %s } %s }\n", a.s, w / 16, body.s, s.s);
        return 1;
    }
    case M(MOVDDUP): case M(VMOVDDUP):
        if (!vread(off, in, &in->op[1], &a) || !vwrite(off, in, &in->op[0], "rr", w, &s)) return 0;
        emit("{ const GuestVec x = %s; GuestVec rr; memset(&rr, 0, sizeof(rr)); for (int i = 0; i < %d; i += 2) rr.q[i] = rr.q[i + 1] = x.q[i]; %s }\n",
             a.s, w / 8, s.s);
        return 1;
    case M(MASKMOVDQU): case M(VMASKMOVDQU): /* bytes of op0 where op1's byte has its top bit, to [rdi] */
        if (!vread(off, in, &in->op[0], &a) || !vread(off, in, &in->op[1], &b)) return 0;
        emit("{ const GuestVec v = %s, mk = %s; uint8_t *dst = (uint8_t *)r[RDI]; for (int i = 0; i < 16; ++i) if (mk.b[i] & 0x80) dst[i] = v.b[i]; }\n",
             a.s, b.s);
        return 1;
    case M(HADDPD): case M(VHADDPD):
        return vbinary(off, in, w / 16, w, 1, "rr.fd[i * 2] = a.fd[i * 2] + a.fd[i * 2 + 1]; rr.fd[i * 2 + 1] = b.fd[i * 2] + b.fd[i * 2 + 1];");
    case M(PHADDD): case M(VPHADDD):
        return vbinary(off, in, w / 16, w, 1, "rr.d[i * 4] = a.d[i * 4] + a.d[i * 4 + 1]; rr.d[i * 4 + 1] = a.d[i * 4 + 2] + a.d[i * 4 + 3]; "
                                              "rr.d[i * 4 + 2] = b.d[i * 4] + b.d[i * 4 + 1]; rr.d[i * 4 + 3] = b.d[i * 4 + 2] + b.d[i * 4 + 3];");
    case M(PHADDW): case M(VPHADDW):
        return vbinary(off, in, w / 16, w, 1, "for (int k = 0; k < 4; ++k) { rr.w[i * 8 + k] = (uint16_t)(a.w[i * 8 + k * 2] + a.w[i * 8 + k * 2 + 1]); "
                                              "rr.w[i * 8 + 4 + k] = (uint16_t)(b.w[i * 8 + k * 2] + b.w[i * 8 + k * 2 + 1]); }");
    case M(PMOVZXWQ): case M(VPMOVZXWQ): case M(PMOVZXBQ): case M(VPMOVZXBQ): case M(PMOVSXWQ): case M(VPMOVSXWQ):
    case M(PMOVSXBQ): case M(VPMOVSXBQ): {
        if (!vread(off, in, &in->op[1], &a) || !vwrite(off, in, &in->op[0], "rr", w, &s)) return 0;
        const char *body = (m == M(PMOVZXWQ) || m == M(VPMOVZXWQ)) ? "rr.q[i] = x.w[i];"
                         : (m == M(PMOVZXBQ) || m == M(VPMOVZXBQ)) ? "rr.q[i] = x.b[i];"
                         : (m == M(PMOVSXWQ) || m == M(VPMOVSXWQ)) ? "rr.sq[i] = x.sw[i];" : "rr.sq[i] = x.sb[i];";
        emit("{ const GuestVec x = %s; GuestVec rr; memset(&rr, 0, sizeof(rr)); for (int i = 0; i < %d; ++i) %s %s }\n", a.s, w / 8, body, s.s);
        return 1;
    }
    case M(CVTPD2PS): case M(VCVTPD2PS): case M(CVTPD2DQ): case M(VCVTPD2DQ): case M(CVTTPD2DQ): case M(VCVTTPD2DQ): {
        /* the source's doubles into the low lanes of a 16-byte result (the interpreter's) */
        if (!vread(off, in, &in->op[1], &a) || !vwrite(off, in, &in->op[0], "rr", 16, &s)) return 0;
        const int lanes = in->op[1].size / 8;
        const Str body = (m == M(CVTPD2PS) || m == M(VCVTPD2PS)) ? str("rr.f[i] = (float)x.fd[i];")
                       : str("rr.sd[i] = rc_f2i32(x.fd[i], %d, c->mxcsr);", m == M(CVTTPD2DQ) || m == M(VCVTTPD2DQ));
        emit("{ const GuestVec x = %s; GuestVec rr; memset(&rr, 0, sizeof(rr)); for (int i = 0; i < %d; ++i) %s %s }\n", a.s, lanes, body.s, s.s);
        return 1;
    }
    case M(VCVTPH2PS):
        if (!vread(off, in, &in->op[1], &a) || !vwrite(off, in, &in->op[0], "rr", in->op[0].size, &s)) return 0;
        emit("{ const GuestVec x = %s; GuestVec rr; memset(&rr, 0, sizeof(rr)); for (int i = 0; i < %d; ++i) rr.f[i] = rc_f16_to_f32(x.w[i]); %s }\n",
             a.s, in->op[0].size / 4, s.s);
        return 1;
    case M(VCVTPS2PH):
        if (!vread(off, in, &in->op[1], &a) ||
            !vwrite(off, in, &in->op[0], "rr", in->op[0].type == OP_MEM ? in->op[0].size : 16, &s)) return 0;
        emit("{ const GuestVec x = %s; GuestVec rr; memset(&rr, 0, sizeof(rr)); for (int i = 0; i < %d; ++i) rr.w[i] = rc_f32_to_f16(x.f[i]); %s }\n",
             a.s, in->op[1].size / 4, s.s);
        return 1;
    case M(EXTRQ): case M(INSERTQ): {
        /* SSE4A (the interpreter's): a bit field of the low quadword extracted (zero-extended) or
         * inserted, the upper quadword zeroed; length 0 means 64. Immediate forms: length, then
         * index; register forms: the source's bits 5:0 and 13:8 (extrq) or its upper quadword's
         * (insertq). */
        const GuestOp *dst = &in->op[0], *src = &in->op[1];
        if (!vreg(dst)) return 0;
        Str ctl;
        if (m == M(EXTRQ) && src->type == OP_IMM) {
            if (in->count < 3 || in->op[2].type != OP_IMM) return 0;
            ctl = str("0x%" PRIx64 "ull", (uint64_t)(src->disp & 63) | (uint64_t)(in->op[2].disp & 63) << 8);
            emit("rc_vset(&xmm[%d], rc_extrq(rc_gv(xmm[%d]), %u, %u), 16, 0);\n", dst->reg, dst->reg,
                 (unsigned)(in->op[2].disp & 63), (unsigned)(src->disp & 63));
            return 1;
        } else {
            if (!vreg(src)) return 0;
            if (m == M(EXTRQ)) ctl = str("rc_gv(xmm[%d]).q[0]", src->reg);
            else if (in->count > 3 && in->op[2].type == OP_IMM && in->op[3].type == OP_IMM)
                ctl = str("0x%" PRIx64 "ull", (uint64_t)(in->op[2].disp & 63) | (uint64_t)(in->op[3].disp & 63) << 8);
            else ctl = str("rc_gv(xmm[%d]).q[1]", src->reg);
        }
        emit("{ GuestVec dd = rc_gv(xmm[%d]); const uint64_t ctl = %s; const unsigned len = ctl & 63, idx = ctl >> 8 & 63; "
             "const uint64_t mk = len ? (1ull << len) - 1 : ~0ull; ", dst->reg, ctl.s);
        if (m == M(EXTRQ)) emit("dd.q[0] = dd.q[0] >> idx & mk; ");
        else emit("const uint64_t field = mk << idx; dd.q[0] = (dd.q[0] & ~field) | (rc_gv(xmm[%d]).q[0] << idx & field); ", src->reg);
        emit("dd.q[1] = 0; rc_vset(&xmm[%d], dd, 16, 0); }\n", dst->reg);
        return 1;
    }
    case M(LDMXCSR): case M(VLDMXCSR): case M(STMXCSR): case M(VSTMXCSR): {
        Str ad;
        int stack;
        if (in->op[0].type != OP_MEM || !address(off, in, &in->op[0], &ad, &stack)) return 0;
        if (m == M(LDMXCSR) || m == M(VLDMXCSR)) emit("rc->set_mxcsr(c, (uint32_t)%s(%s, 4));\n", stack ? "rc_lds" : "rc_ld", ad.s);
        else emit("%s(%s, 4, c->mxcsr);\n", stack ? "rc_sts" : "rc_st", ad.s);
        return 1;
    }
    default: {
        /* Integer lanes (the interpreter's ibinary): rr.field[i] = (type)(expr). */
        static const struct { int m, vm, es; const char *field, *type, *expr; } lanes[] = {
            {M(PADDB), M(VPADDB), 1, "b", "uint8_t", "a.b[i] + b.b[i]"},
            {M(PADDW), M(VPADDW), 2, "w", "uint16_t", "a.w[i] + b.w[i]"},
            {M(PSUBB), M(VPSUBB), 1, "b", "uint8_t", "a.b[i] - b.b[i]"},
            {M(PSUBW), M(VPSUBW), 2, "w", "uint16_t", "a.w[i] - b.w[i]"},
            {M(PADDSW), M(VPADDSW), 2, "sw", "int16_t", "rc_sat16(a.sw[i] + b.sw[i])"},
            {M(PADDUSW), M(VPADDUSW), 2, "w", "uint16_t", "rc_usat16(a.w[i] + b.w[i])"},
            {M(PSUBSW), M(VPSUBSW), 2, "sw", "int16_t", "rc_sat16(a.sw[i] - b.sw[i])"},
            {M(PSUBUSW), M(VPSUBUSW), 2, "w", "uint16_t", "rc_usat16(a.w[i] - b.w[i])"},
            {M(PADDSB), M(VPADDSB), 1, "sb", "int8_t", "rc_sat8(a.sb[i] + b.sb[i])"},
            {M(PADDUSB), M(VPADDUSB), 1, "b", "uint8_t", "rc_usat8(a.b[i] + b.b[i])"},
            {M(PSUBSB), M(VPSUBSB), 1, "sb", "int8_t", "rc_sat8(a.sb[i] - b.sb[i])"},
            {M(PSUBUSB), M(VPSUBUSB), 1, "b", "uint8_t", "rc_usat8(a.b[i] - b.b[i])"},
            {M(PMULLW), M(VPMULLW), 2, "w", "uint16_t", "a.sw[i] * b.sw[i]"},
            {M(PMULHW), M(VPMULHW), 2, "w", "uint16_t", "(a.sw[i] * b.sw[i]) >> 16"},
            {M(PMULHUW), M(VPMULHUW), 2, "w", "uint16_t", "((uint32_t)a.w[i] * b.w[i]) >> 16"},
            {M(PMULLD), M(VPMULLD), 4, "d", "uint32_t", "(uint32_t)((int64_t)a.sd[i] * b.sd[i])"},
            {M(PMULUDQ), M(VPMULUDQ), 8, "q", "uint64_t", "(uint64_t)a.d[i * 2] * b.d[i * 2]"},
            {M(PMULDQ), M(VPMULDQ), 8, "q", "uint64_t", "(uint64_t)((int64_t)a.sd[i * 2] * b.sd[i * 2])"},
            {M(PAVGB), M(VPAVGB), 1, "b", "uint8_t", "(a.b[i] + b.b[i] + 1) >> 1"},
            {M(PAVGW), M(VPAVGW), 2, "w", "uint16_t", "(a.w[i] + b.w[i] + 1) >> 1"},
            {M(PMINSD), M(VPMINSD), 4, "sd", "int32_t", "a.sd[i] < b.sd[i] ? a.sd[i] : b.sd[i]"},
            {M(PMAXSD), M(VPMAXSD), 4, "sd", "int32_t", "a.sd[i] > b.sd[i] ? a.sd[i] : b.sd[i]"},
            {M(PMINUD), M(VPMINUD), 4, "d", "uint32_t", "a.d[i] < b.d[i] ? a.d[i] : b.d[i]"},
            {M(PMAXUD), M(VPMAXUD), 4, "d", "uint32_t", "a.d[i] > b.d[i] ? a.d[i] : b.d[i]"},
            {M(PMINSW), M(VPMINSW), 2, "sw", "int16_t", "a.sw[i] < b.sw[i] ? a.sw[i] : b.sw[i]"},
            {M(PMAXSW), M(VPMAXSW), 2, "sw", "int16_t", "a.sw[i] > b.sw[i] ? a.sw[i] : b.sw[i]"},
            {M(PMINUW), M(VPMINUW), 2, "w", "uint16_t", "a.w[i] < b.w[i] ? a.w[i] : b.w[i]"},
            {M(PMAXUW), M(VPMAXUW), 2, "w", "uint16_t", "a.w[i] > b.w[i] ? a.w[i] : b.w[i]"},
            {M(PMINUB), M(VPMINUB), 1, "b", "uint8_t", "a.b[i] < b.b[i] ? a.b[i] : b.b[i]"},
            {M(PMAXUB), M(VPMAXUB), 1, "b", "uint8_t", "a.b[i] > b.b[i] ? a.b[i] : b.b[i]"},
            {M(PMINSB), M(VPMINSB), 1, "sb", "int8_t", "a.sb[i] < b.sb[i] ? a.sb[i] : b.sb[i]"},
            {M(PMAXSB), M(VPMAXSB), 1, "sb", "int8_t", "a.sb[i] > b.sb[i] ? a.sb[i] : b.sb[i]"},
            {M(PCMPEQW), M(VPCMPEQW), 2, "w", "uint16_t", "a.w[i] == b.w[i] ? 0xffff : 0"},
            {M(PCMPGTB), M(VPCMPGTB), 1, "b", "uint8_t", "a.sb[i] > b.sb[i] ? 0xff : 0"},
            {M(PCMPGTW), M(VPCMPGTW), 2, "w", "uint16_t", "a.sw[i] > b.sw[i] ? 0xffff : 0"},
            {M(PCMPGTD), M(VPCMPGTD), 4, "d", "uint32_t", "a.sd[i] > b.sd[i] ? ~0u : 0"},
            {M(PCMPGTQ), M(VPCMPGTQ), 8, "q", "uint64_t", "a.sq[i] > b.sq[i] ? ~0ull : 0"},
            {M(PABSD), M(VPABSD), 4, "d", "uint32_t", "b.sd[i] < 0 ? -(uint32_t)b.sd[i] : (uint32_t)b.sd[i]"},
            {M(PABSW), M(VPABSW), 2, "w", "uint16_t", "b.sw[i] < 0 ? -b.sw[i] : b.sw[i]"},
            {M(PABSB), M(VPABSB), 1, "b", "uint8_t", "b.sb[i] < 0 ? -b.sb[i] : b.sb[i]"},
        };
        for (size_t k = 0; k < sizeof(lanes) / sizeof(lanes[0]); ++k)
            if (lanes[k].m == m || lanes[k].vm == m)
                return vbinary(off, in, w / lanes[k].es, w, 1, str("rr.%s[i] = (%s)(%s);", lanes[k].field, lanes[k].type, lanes[k].expr).s);
        return 0;
    }
    }
}

/* Locked read-modify-writes (and xadd, cmpxchg): a compare-and-exchange loop, as the interpreter. */
static int translate_atomic(uint64_t off, const GuestInsn *in) {
    const int m = in->mnemonic;
    const GuestOp *d = &in->op[0], *src = &in->op[1];
    if (d->type != OP_MEM) return 0;
    const int size = d->size;
    Str ad, b, w;
    int stack;
    if (m == M(CMPXCHG16B)) {
        if (!address(off, in, d, &ad, &stack)) return 0;
        emit("{ uint64_t lo = r[RAX], hi = r[RDX]; const int ok = rc_cas16(%s, &lo, &hi, r[RBX], r[RCX]); "
             "ef = (rc_flags(fk, fa, fb, fr, fz, fcf, ef) & ~(uint64_t)F_ZF) | (ok ? F_ZF : 0); fk = FK_CPU; "
             "if (!ok) { r[RAX] = lo; r[RDX] = hi; } }\n", ad.s);
        return 1;
    }
    if (size != 1 && size != 2 && size != 4 && size != 8) return 0;
    if (!address(off, in, d, &ad, &stack)) return 0;
    const uint64_t mk = mask(size);
    switch (m) {
    case M(ADD): case M(SUB): case M(AND): case M(OR): case M(XOR): {
        const char *op = m == M(ADD) ? "+" : m == M(SUB) ? "-" : m == M(AND) ? "&" : m == M(OR) ? "|" : "^";
        if (!read_op(off, in, src, size, &b)) return 0;
        emit("{ const uint64_t ea = %s; const uint64_t b = %s; uint64_t a, seen; do a = rc_ld(ea, %d); "
             "while (!rc_cas(ea, %d, a, (a %s b) & 0x%" PRIx64 "ull, &seen)); const uint64_t v = (a %s b) & 0x%" PRIx64 "ull; "
             "fk = %s; fa = a; fb = b; fr = v; fz = %d; }\n", ad.s, b.s, size, size, op, mk, op, mk,
             m == M(ADD) ? "FK_ADD" : m == M(SUB) ? "FK_SUB" : "FK_LOGIC", size);
        return 1;
    }
    case M(INC): case M(DEC): {
        const char *op = m == M(INC) ? "+" : "-";
        emit("{ const uint64_t ea = %s; fcf = " COND "; uint64_t a, seen; do a = rc_ld(ea, %d); "
             "while (!rc_cas(ea, %d, a, (a %s 1) & 0x%" PRIx64 "ull, &seen)); const uint64_t v = (a %s 1) & 0x%" PRIx64 "ull; "
             "fk = %s; fa = a; fb = 1; fr = v; fz = %d; }\n", ad.s, 2, size, size, op, mk, op, mk,
             m == M(INC) ? "FK_INC" : "FK_DEC", size);
        return 1;
    }
    case M(XADD): {
        if (!gpr(src) || !read_op(off, in, src, size, &b) || !write_op(off, in, src, "a", &w)) return 0;
        emit("{ const uint64_t ea = %s; const uint64_t b = %s; uint64_t a, seen; do a = rc_ld(ea, %d); "
             "while (!rc_cas(ea, %d, a, (a + b) & 0x%" PRIx64 "ull, &seen)); fk = FK_ADD; fa = a; fb = b; "
             "fr = (a + b) & 0x%" PRIx64 "ull; fz = %d; %s }\n", ad.s, b.s, size, size, mk, mk, size, w.s);
        return 1;
    }
    case M(CMPXCHG): {
        const GuestOp rax = {.type = OP_REG, .kind = RK_GPR, .reg = RAX, .size = (uint8_t)size};
        Str e;
        if (!gpr(src) || !read_op(off, in, src, size, &b) || !read_op(off, in, &rax, size, &e) ||
            !write_op(off, in, &rax, "seen", &w))
            return 0;
        emit("{ const uint64_t ea = %s; const uint64_t e = (%s) & 0x%" PRIx64 "ull; uint64_t seen; "
             "const int ok = rc_cas(ea, %d, e, %s, &seen); fk = FK_SUB; fa = e; fb = seen; fr = (e - seen) & 0x%" PRIx64 "ull; "
             "fz = %d; if (!ok) { %s } }\n", ad.s, e.s, mk, size, b.s, mk, size, w.s);
        return 1;
    }
    default:
        return 0;
    }
}

/* x87, as the interpreter runs it (interp_x87.c): values as doubles in the CPU state (rc_x87).
 * -1: not an x87 instruction; 0: not this form (fxsave, fxrstor). */
static int x87_arith_kind(int m) {
    switch (m) {
    case M(FADD): case M(FADDP): case M(FIADD): return 0;
    case M(FMUL): case M(FMULP): case M(FIMUL): return 1;
    case M(FSUB): case M(FSUBP): case M(FISUB): return 2;
    case M(FSUBR): case M(FSUBRP): case M(FISUBR): return 3;
    case M(FDIV): case M(FDIVP): case M(FIDIV): return 4;
    case M(FDIVR): case M(FDIVRP): case M(FIDIVR): return 5;
    default: return -1;
    }
}
static int x87_reg(const GuestOp *op) { return op->type == OP_REG && op->kind == RK_X87; }
/* A memory operand's address, or a stack register: ST(i) as rc_x87(c, i). */
static int x87_mem(uint64_t off, const GuestInsn *in, const GuestOp *op, Str *ad) {
    int stack;
    return op->type == OP_MEM && address(off, in, op, ad, &stack);
}
static int translate_x87(uint64_t off, const GuestInsn *in) {
    const int m = in->mnemonic;
    const GuestOp *last = in->count ? &in->op[in->count - 1] : NULL;
    Str ad;
    const int kind = x87_arith_kind(m);
    if (kind >= 0) {
        const int integer = m == M(FIADD) || m == M(FIMUL) || m == M(FISUB) || m == M(FISUBR) || m == M(FIDIV) || m == M(FIDIVR);
        const int pop = m == M(FADDP) || m == M(FMULP) || m == M(FSUBP) || m == M(FSUBRP) || m == M(FDIVP) || m == M(FDIVRP);
        if (in->count == 2 && in->op[0].type == OP_REG && in->op[1].type == OP_REG) {
            if (!x87_reg(&in->op[0]) || !x87_reg(&in->op[1])) return 0;
            emit("{ double *d = rc_x87(c, %d); *d = rc_farith(%d, *d, *rc_x87(c, %d)); }", in->op[0].reg, kind, in->op[1].reg);
        } else if (last && last->type == OP_MEM) {
            if (!x87_mem(off, in, last, &ad)) return 0;
            emit("*rc_x87(c, 0) = rc_farith(%d, *rc_x87(c, 0), rc_fmem(%s, %d, %d));", kind, ad.s, last->size, integer);
        } else if (in->count == 1) {
            if (!x87_reg(&in->op[0])) return 0;
            emit("*rc_x87(c, 0) = rc_farith(%d, *rc_x87(c, 0), *rc_x87(c, %d));", kind, in->op[0].reg);
        } else {
            emit("{ double *d = rc_x87(c, 1); *d = rc_farith(%d, *d, *rc_x87(c, 0)); }", kind);
        }
        emit(pop ? " rc_fpop(c);\n" : "\n");
        return 1;
    }
    static const struct { int m; const char *value; } constants[] = {
        {M(FLD1), "1.0"}, {M(FLDZ), "0.0"}, {M(FLDPI), "0x1.921fb54442d18p+1"}, {M(FLDL2E), "0x1.71547652b82fep+0"},
        {M(FLDLN2), "0x1.62e42fefa39efp-1"}, {M(FLDL2T), "__builtin_log2(10.0)"}, {M(FLDLG2), "__builtin_log10(2.0)"}};
    for (size_t i = 0; i < sizeof(constants) / sizeof(constants[0]); ++i)
        if (m == constants[i].m) {
            emit("rc_fpush(c, %s);\n", constants[i].value);
            return 1;
        }
    static const struct { int m, cc; } fcmov[] = {{M(FCMOVB), 2}, {M(FCMOVE), 4}, {M(FCMOVBE), 6}, {M(FCMOVU), 10},
                                                  {M(FCMOVNB), 3}, {M(FCMOVNE), 5}, {M(FCMOVNBE), 7}, {M(FCMOVNU), 11}};
    for (size_t i = 0; i < sizeof(fcmov) / sizeof(fcmov[0]); ++i)
        if (m == fcmov[i].m) {
            if (!last || !x87_reg(last)) return 0;
            emit("if (" COND ") *rc_x87(c, 0) = *rc_x87(c, %d);\n", fcmov[i].cc, last->reg);
            return 1;
        }
    switch (m) {
    case M(FLD):
        if (!last) return 0;
        if (x87_reg(last)) emit("{ const double v = *rc_x87(c, %d); rc_fpush(c, v); }\n", last->reg);
        else if (x87_mem(off, in, last, &ad)) emit("rc_fpush(c, rc_fmem(%s, %d, 0));\n", ad.s, last->size);
        else return 0;
        return 1;
    case M(FILD):
        if (!last || !x87_mem(off, in, last, &ad)) return 0;
        emit("rc_fpush(c, rc_fmem(%s, %d, 1));\n", ad.s, last->size);
        return 1;
    case M(FST): case M(FSTP): {
        const GuestOp *op = &in->op[0];
        if (x87_reg(op)) emit("*rc_x87(c, %d) = *rc_x87(c, 0);", op->reg);
        else if (x87_mem(off, in, op, &ad)) emit("rc_fstore(%s, %d, *rc_x87(c, 0));", ad.s, op->size);
        else return 0;
        emit(m == M(FSTP) ? " rc_fpop(c);\n" : "\n");
        return 1;
    }
    case M(FIST): case M(FISTP): case M(FISTTP): {
        const GuestOp *op = &in->op[0];
        if (!x87_mem(off, in, op, &ad)) return 0;
        emit("rc_st(%s, %d, (uint64_t)rc_fint(c, *rc_x87(c, 0), %d));%s\n", ad.s, op->size, m == M(FISTTP),
             m != M(FIST) ? " rc_fpop(c);" : "");
        return 1;
    }
    case M(FXCH): {
        const int i = last ? last->reg : 1;
        if (last && !x87_reg(last)) return 0;
        emit("{ const double t = *rc_x87(c, 0); *rc_x87(c, 0) = *rc_x87(c, %d); *rc_x87(c, %d) = t; }\n", i, i);
        return 1;
    }
    case M(FCHS): emit("*rc_x87(c, 0) = -*rc_x87(c, 0);\n"); return 1;
    case M(FABS): emit("*rc_x87(c, 0) = __builtin_fabs(*rc_x87(c, 0));\n"); return 1;
    case M(FSQRT): emit("*rc_x87(c, 0) = rc_sqrt(*rc_x87(c, 0));\n"); return 1;
    case M(FSIN): emit("*rc_x87(c, 0) = __builtin_sin(*rc_x87(c, 0)); c->fsw &= ~0x400;\n"); return 1;
    case M(FCOS): emit("*rc_x87(c, 0) = __builtin_cos(*rc_x87(c, 0)); c->fsw &= ~0x400;\n"); return 1;
    case M(FSINCOS):
        emit("{ const double v = *rc_x87(c, 0); *rc_x87(c, 0) = __builtin_sin(v); rc_fpush(c, __builtin_cos(v)); c->fsw &= ~0x400; }\n");
        return 1;
    case M(FPTAN): emit("*rc_x87(c, 0) = __builtin_tan(*rc_x87(c, 0)); rc_fpush(c, 1.0); c->fsw &= ~0x400;\n"); return 1;
    case M(FPATAN): emit("{ const double x = rc_fpop(c); *rc_x87(c, 0) = __builtin_atan2(*rc_x87(c, 0), x); }\n"); return 1;
    case M(FSCALE): emit("*rc_x87(c, 0) = __builtin_ldexp(*rc_x87(c, 0), (int)__builtin_trunc(*rc_x87(c, 1)));\n"); return 1;
    case M(FRNDINT): emit("*rc_x87(c, 0) = (double)rc_fint(c, *rc_x87(c, 0), 0);\n"); return 1;
    case M(F2XM1): emit("*rc_x87(c, 0) = __builtin_exp2(*rc_x87(c, 0)) - 1.0;\n"); return 1;
    case M(FYL2X): emit("{ const double x = rc_fpop(c); *rc_x87(c, 0) = *rc_x87(c, 0) * __builtin_log2(x); }\n"); return 1;
    case M(FYL2XP1): emit("{ const double x = rc_fpop(c); *rc_x87(c, 0) = *rc_x87(c, 0) * __builtin_log2(x + 1.0); }\n"); return 1;
    case M(FPREM): case M(FPREM1):
        emit("*rc_x87(c, 0) = __builtin_%s(*rc_x87(c, 0), *rc_x87(c, 1)); c->fsw &= ~0x400;\n", m == M(FPREM) ? "fmod" : "remainder");
        return 1;
    case M(FXTRACT):
        emit("{ const double v = *rc_x87(c, 0); int e; const double f = __builtin_frexp(v, &e); *rc_x87(c, 0) = (double)(e - 1); "
             "rc_fpush(c, f * 2.0); }\n");
        return 1;
    case M(FCOM): case M(FCOMP): case M(FUCOM): case M(FUCOMP): case M(FICOM): case M(FICOMP): {
        const int integer = m == M(FICOM) || m == M(FICOMP);
        Str b;
        if (!last) b = str("*rc_x87(c, 1)");
        else if (x87_reg(last)) b = str("*rc_x87(c, %d)", last->reg);
        else if (x87_mem(off, in, last, &ad)) b = str("rc_fmem(%s, %d, %d)", ad.s, last->size, integer);
        else return 0;
        emit("rc_fcom(c, *rc_x87(c, 0), %s);%s\n", b.s, m == M(FCOMP) || m == M(FUCOMP) || m == M(FICOMP) ? " rc_fpop(c);" : "");
        return 1;
    }
    case M(FCOMPP): case M(FUCOMPP): emit("rc_fcom(c, *rc_x87(c, 0), *rc_x87(c, 1)); rc_fpop(c); rc_fpop(c);\n"); return 1;
    case M(FCOMI): case M(FCOMIP): case M(FUCOMI): case M(FUCOMIP):
        if (!last || !x87_reg(last)) return 0;
        emit("ef = rc_fcomi(rc_flags(fk, fa, fb, fr, fz, fcf, ef), *rc_x87(c, 0), *rc_x87(c, %d)); fk = FK_CPU;%s\n",
             last->reg, m == M(FCOMIP) || m == M(FUCOMIP) ? " rc_fpop(c);" : "");
        return 1;
    case M(FTST): emit("rc_fcom(c, *rc_x87(c, 0), 0.0);\n"); return 1;
    case M(FXAM): emit("rc_fxam(c);\n"); return 1;
    case M(FNSTSW): {
        const GuestOp *op = &in->op[0];
        if (op->type == OP_REG && op->kind == RK_GPR) emit("r[RAX] = (r[RAX] & ~0xffffull) | rc_fsw(c);\n");
        else if (x87_mem(off, in, op, &ad)) emit("rc_st(%s, 2, rc_fsw(c));\n", ad.s);
        else return 0;
        return 1;
    }
    case M(FNSTCW):
        if (!x87_mem(off, in, &in->op[0], &ad)) return 0;
        emit("rc_st(%s, 2, c->fcw);\n", ad.s);
        return 1;
    case M(FLDCW):
        if (!x87_mem(off, in, &in->op[0], &ad)) return 0;
        emit("c->fcw = (uint16_t)rc_ld(%s, 2);\n", ad.s);
        return 1;
    case M(FNINIT): emit("c->fcw = 0x37f; c->fsw = 0; c->top = 0;\n"); return 1;
    case M(FNCLEX): emit("c->fsw &= 0x7f00;\n"); return 1;
    case M(FNSTENV):
        if (!x87_mem(off, in, &in->op[0], &ad)) return 0;
        emit("rc_fnstenv(c, %s);\n", ad.s);
        return 1;
    case M(FLDENV):
        if (!x87_mem(off, in, &in->op[0], &ad)) return 0;
        emit("rc_fldenv(c, %s);\n", ad.s);
        return 1;
    case M(FWAIT): case M(FNOP): case M(FFREE): case M(FDECSTP): case M(FINCSTP): emit("/* %s */\n", ZydisMnemonicGetString((ZydisMnemonic)m)); return 1;
    case M(FNSAVE): case M(FRSTOR): case M(FXSAVE): case M(FXSAVE64): case M(FXRSTOR): case M(FXRSTOR64): return 0;
    default: return -1;
    }
}

/* Sign extensions of rax; adc/sbb; bt/bts/btr/btc; shifts by cl. */
static int translate_extra(uint64_t off, const GuestInsn *in) {
    const int m = in->mnemonic;
    const GuestOp *d = &in->op[0], *src = &in->op[1];
    static const char *const sext[9] = {0, "int8_t", "int16_t", 0, "int32_t", 0, 0, 0, "int64_t"};
    Str a, b, w, ad;
    int stack = 0;
    switch (m) {
    case M(CBW): emit("r[RAX] = (r[RAX] & ~0xffffull) | ((uint64_t)(int16_t)(int8_t)r[RAX] & 0xffff);\n"); return 1;
    case M(CWDE): emit("r[RAX] = (uint32_t)(int32_t)(int16_t)r[RAX];\n"); return 1;
    case M(CDQE): emit("r[RAX] = (uint64_t)(int64_t)(int32_t)r[RAX];\n"); return 1;
    case M(CWD): emit("r[RDX] = (r[RDX] & ~0xffffull) | (((int16_t)r[RAX] < 0) ? 0xffff : 0);\n"); return 1;
    case M(CDQ): emit("r[RDX] = ((int32_t)r[RAX] < 0) ? 0xffffffffu : 0;\n"); return 1;
    case M(CQO): emit("r[RDX] = ((int64_t)r[RAX] < 0) ? ~0ull : 0;\n"); return 1;
    case M(ADC): case M(SBB): {
        const int size = d->size;
        if (!(gpr(d) || d->type == OP_MEM) || !read_op(off, in, src, size, &b)) return 0;
        emit("{ const uint64_t cy = " COND "; ", 2);
        if (d->type == OP_MEM) {
            if (!address(off, in, d, &ad, &stack)) return 0;
            emit("const uint64_t ea = %s; const uint64_t a = %s(ea, %d); ", ad.s, stack ? "rc_lds" : "rc_ld", size);
        } else {
            if (!read_op(off, in, d, size, &a)) return 0;
            emit("const uint64_t a = %s; ", a.s);
        }
        const char *op = m == M(ADC) ? "+" : "-";
        emit("const uint64_t b = %s; const uint64_t v = (a %s b %s cy) & 0x%" PRIx64 "ull; "
             "ef = rc_carry_flags(rc_flags(fk, fa, fb, fr, fz, fcf, ef), %d, a, b, cy, v, %d); fk = FK_CPU; ",
             b.s, op, op, mask(size), m == M(SBB), size);
        if (d->type == OP_MEM) emit("%s(ea, %d, v);", stack ? "rc_sts" : "rc_st", size);
        else {
            if (!write_op(off, in, d, "v", &w)) return 0;
            emit("%s", w.s);
        }
        emit(" }\n");
        return 1;
    }
    case M(BT): case M(BTS): case M(BTR): case M(BTC): {
        const int size = d->size, bits = size * 8;
        if (size < 2 || !read_op(off, in, src, size, &b)) return 0;
        const char *calc = m == M(BTS) ? "a | bm" : m == M(BTR) ? "a & ~bm" : m == M(BTC) ? "a ^ bm" : "a";
        emit("{ ");
        if (d->type == OP_MEM) { /* register bit offsets (reaching outside the operand): the interpreter */
            if (src->type != OP_IMM || !address(off, in, d, &ad, &stack)) return 0;
            emit("const uint64_t ea = %s; const unsigned bit = (unsigned)(%s) & %d; ", ad.s, b.s, bits - 1);
            emit("const uint64_t a = %s(ea, %d); ", stack ? "rc_lds" : "rc_ld", size);
        } else if (gpr(d)) {
            if (!read_op(off, in, d, size, &a)) return 0;
            emit("const uint64_t a = %s; const unsigned bit = (unsigned)(%s) & %d; ", a.s, b.s, bits - 1);
        } else {
            return 0;
        }
        emit("const uint64_t bm = 1ull << bit; ef = (rc_flags(fk, fa, fb, fr, fz, fcf, ef) & ~(uint64_t)F_CF) | "
             "((a & bm) ? F_CF : 0); fk = FK_CPU; ");
        if (m != M(BT)) {
            emit("const uint64_t v = %s; ", calc);
            if (d->type == OP_MEM) emit("%s(ea, %d, v);", stack ? "rc_sts" : "rc_st", size);
            else {
                if (!write_op(off, in, d, "v", &w)) return 0;
                emit("%s", w.s);
            }
        }
        emit(" }\n");
        return 1;
    }
    case M(SHLD): case M(SHRD): { /* the interpreter's double_shift */
        const GuestOp *cnt = &in->op[2];
        const int size = d->size, bits = size * 8;
        if (in->count != 3 || !gpr(src) || size < 2 || !sext[size] || !read_op(off, in, src, size, &b)) return 0;
        Str n;
        if (cnt->type == OP_IMM) n = str("%uu", (unsigned)cnt->disp & (size == 8 ? 63 : 31));
        else if (cnt->type == OP_REG && cnt->reg == RCX && cnt->size == 1) n = str("((unsigned)r[RCX] & %d)", size == 8 ? 63 : 31);
        else return 0;
        if (d->type == OP_MEM) {
            if (!address(off, in, d, &ad, &stack)) return 0;
        } else if (!gpr(d) || !read_op(off, in, d, size, &a)) {
            return 0;
        }
        emit("{ const unsigned n = %s; if (n) { ", n.s);
        if (d->type == OP_MEM) emit("const uint64_t ea = %s; const uint64_t a = %s(ea, %d) & 0x%" PRIx64 "ull; ", ad.s,
                                    stack ? "rc_lds" : "rc_ld", size, mask(size));
        else emit("const uint64_t a = (%s) & 0x%" PRIx64 "ull; ", a.s, mask(size));
        emit("const uint64_t b = (%s) & 0x%" PRIx64 "ull; ", b.s, mask(size));
        if (m == M(SHLD))
            emit("const uint64_t v = ((a << n) | (n == %d ? b : b >> (%d - n))) & 0x%" PRIx64 "ull; const int cf = (a >> (%d - n)) & 1; ",
                 bits, bits, mask(size), bits);
        else
            emit("const uint64_t v = ((a >> n) | (b << (%d - n))) & 0x%" PRIx64 "ull; const int cf = (a >> (n - 1)) & 1; ", bits,
                 mask(size));
        if (flags_dead) emit("(void)cf; ");
        else emit("uint64_t fl = (rc_flags(fk, fa, fb, fr, fz, fcf, ef) & ~(uint64_t)(F_CF | F_OF)) | (cf ? F_CF : 0); "
                  "if ((v ^ a) & 0x%" PRIx64 "ull) fl |= F_OF; ef = rc_szp(fl, v, %d); fk = FK_CPU; ",
                  (uint64_t)1 << (bits - 1), size);
        if (d->type == OP_MEM) emit("%s(ea, %d, v);", stack ? "rc_sts" : "rc_st", size);
        else {
            if (!write_op(off, in, d, "v", &w)) return 0;
            emit("%s", w.s);
        }
        emit(" } }\n");
        return 1;
    }
    case M(SHL): case M(SHR): case M(SAR): {
        if (in->count < 2 || src->type != OP_REG || src->reg != RCX || src->size != 1) return 0;
        const int size = d->size, bits = size * 8;
        if (!sext[size]) return 0;
        emit("{ const unsigned n = (unsigned)r[RCX] & %d; if (n) { ", size == 8 ? 63 : 31);
        if (d->type == OP_MEM) {
            if (!address(off, in, d, &ad, &stack)) return 0;
            emit("const uint64_t ea = %s; const uint64_t a = %s(ea, %d); ", ad.s, stack ? "rc_lds" : "rc_ld", size);
        } else if (gpr(d)) {
            if (!read_op(off, in, d, size, &a)) return 0;
            emit("const uint64_t a = %s; ", a.s);
        } else {
            return 0;
        }
        if (m == M(SHL)) emit("const uint64_t v = n >= %d ? 0 : (a << n) & 0x%" PRIx64 "ull; ", bits, mask(size));
        else if (m == M(SHR)) emit("const uint64_t v = n >= %d ? 0 : a >> n; ", bits);
        else emit("const uint64_t v = (uint64_t)((int64_t)(%s)a >> (n >= %d ? %d : n)) & 0x%" PRIx64 "ull; ", sext[size], bits,
                  bits - 1, mask(size));
        if (!flags_dead)
            emit("ef = rc_shift_flags(rc_flags(fk, fa, fb, fr, fz, fcf, ef), %d, %s, n, v, %d); fk = FK_CPU; ",
                 m == M(SHL) ? 0 : m == M(SHR) ? 1 : 2, m == M(SAR) ? str("(uint64_t)(int64_t)(%s)a", sext[size]).s : "a", size);
        if (d->type == OP_MEM) emit("%s(ea, %d, v);", stack ? "rc_sts" : "rc_st", size);
        else {
            if (!write_op(off, in, d, "v", &w)) return 0;
            emit("%s", w.s);
        }
        emit(" } }\n");
        return 1;
    }
    default:
        return 0;
    }
}

/* shl/shr/sar by a constant; imul with two or three operands. */
static int translate_shift_mul(uint64_t off, const GuestInsn *in) {
    const int m = in->mnemonic;
    const GuestOp *d = &in->op[0];
    const int size = d->size;
    if (size != 4 && size != 8 && size != 2 && size != 1) return 0;
    static const char *const sext[9] = {0, "int8_t", "int16_t", 0, "int32_t", 0, 0, 0, "int64_t"};
    Str a, w;
    if (m == M(SHL) || m == M(SHR) || m == M(SAR)) {
        unsigned count = 1;
        if (in->count > 1) {
            if (in->op[1].type != OP_IMM) return 0; /* by cl */
            count = (unsigned)in->op[1].disp;
        }
        count &= size == 8 ? 63 : 31;
        if (!count) { emit("/* shift by 0 */\n"); return 1; }
        const int bits = size * 8;
        Str da;
        int stack = 0;
        emit("{ ");
        if (d->type == OP_MEM) {
            if (!address(off, in, d, &da, &stack)) return 0;
            emit("const uint64_t ea = %s; const uint64_t a = %s(ea, %d); ", da.s, stack ? "rc_lds" : "rc_ld", size);
        } else if (gpr(d)) {
            if (!read_op(off, in, d, size, &a)) return 0;
            emit("const uint64_t a = %s; ", a.s);
        } else {
            return 0;
        }
        if (m == M(SHL)) emit("const uint64_t v = %s; ", count >= (unsigned)bits ? "0" : str("(a << %u) & 0x%" PRIx64 "ull", count, mask(size)).s);
        else if (m == M(SHR)) emit("const uint64_t v = %s; ", count >= (unsigned)bits ? "0" : str("a >> %u", count).s);
        else emit("const uint64_t v = (uint64_t)((int64_t)(%s)a >> %u) & 0x%" PRIx64 "ull; ", sext[size],
                  count >= (unsigned)bits ? (unsigned)bits - 1 : count, mask(size));
        if (!flags_dead)
            emit("ef = rc_shift_flags(rc_flags(fk, fa, fb, fr, fz, fcf, ef), %d, %s, %u, v, %d); fk = FK_CPU; ",
                 m == M(SHL) ? 0 : m == M(SHR) ? 1 : 2, m == M(SAR) ? str("(uint64_t)(int64_t)(%s)a", sext[size]).s : "a", count,
                 size);
        if (d->type == OP_MEM) emit("%s(ea, %d, v);", stack ? "rc_sts" : "rc_st", size);
        else {
            if (!write_op(off, in, d, "v", &w)) return 0;
            emit("%s", w.s);
        }
        emit(" }\n");
        return 1;
    }
    if (m == M(IMUL) && in->count >= 2 && gpr(d) && size >= 2) {
        Str x, y;
        if (!read_op(off, in, in->count == 3 ? &in->op[1] : d, size, &x)) return 0;
        if (in->count == 3) {
            if (in->op[2].type != OP_IMM) return 0;
            const LiveField *l = live_at(off, 0);
            if (l) y = str("(int64_t)(%s)rc_code(B + 0x%" PRIx64 "ull, %d)", sint[l->size], l->field, l->size);
            else y = str("(int64_t)%" PRId64 "ll", in->op[2].disp);
        } else {
            Str t;
            if (!read_op(off, in, &in->op[1], size, &t)) return 0;
            y = str("(int64_t)(%s)(%s)", sext[size], t.s);
        }
        if (!write_op(off, in, d, "v", &w)) return 0;
        if (flags_dead)
            emit("{ const uint64_t v = (uint64_t)((__int128)(int64_t)(%s)(%s) * %s) & 0x%" PRIx64 "ull; %s }\n", sext[size], x.s,
                 y.s, mask(size), w.s);
        else
            emit("{ const __int128 full = (__int128)(int64_t)(%s)(%s) * %s; const uint64_t v = (uint64_t)full & 0x%" PRIx64 "ull; "
                 "const int ov = (__int128)(int64_t)(%s)v != full; "
                 "ef = rc_szp((rc_flags(fk, fa, fb, fr, fz, fcf, ef) & ~(uint64_t)(F_CF | F_OF)) | (ov ? F_CF | F_OF : 0), v, %d); "
                 "fk = FK_CPU; %s }\n", sext[size], x.s, y.s, mask(size), sext[size], size, w.s);
        return 1;
    }
    return 0;
}

/* A read-modify-write destination: `a` its value (zero-extended), `put` the statement storing v. */
static int rmw_dest(uint64_t off, const GuestInsn *in, const GuestOp *d, Str *load, Str *put) {
    Str ad, w;
    int stack = 0;
    if (d->type == OP_MEM) {
        if (!address(off, in, d, &ad, &stack)) return 0;
        *load = str("const uint64_t ea = %s; const uint64_t a = %s(ea, %d);", ad.s, stack ? "rc_lds" : "rc_ld", d->size);
        *put = str("%s(ea, %d, v);", stack ? "rc_sts" : "rc_st", d->size);
        return 1;
    }
    if (!gpr(d) || !read_op(off, in, d, d->size, &ad) || !write_op(off, in, d, "v", &w)) return 0;
    *load = str("const uint64_t a = %s;", ad.s);
    *put = w;
    return 1;
}

/* Bit scans and counts, BMI1, mul/imul/div/idiv of rdx:rax, rotates, bswap/movbe, xchg, rdtsc (as
 * src/cpu/interp.c does them: flags it leaves alone stay as they were). */
static int translate_bits(uint64_t off, const GuestInsn *in) {
    const int m = in->mnemonic;
    const GuestOp *d = &in->op[0], *src = &in->op[1];
    const int size = d->size, bits = size * 8;
    static const char *const sext[9] = {0, "int8_t", "int16_t", 0, "int32_t", 0, 0, 0, "int64_t"};
    Str a, b, w, load, put;
    switch (m) {
    case M(BSF): case M(BSR): case M(TZCNT): case M(LZCNT): case M(POPCNT): {
        if (!gpr(d) || (size != 2 && size != 4 && size != 8) || !read_op(off, in, src, size, &a) || !write_op(off, in, d, "v", &w))
            return 0;
        /* flags_dead: the value alone */
        emit("{ const uint64_t a = %s; %s", a.s, flags_dead ? "" : "const uint64_t f = " FLAGS_NOW "; ");
        if (m == M(BSF) || m == M(BSR))
            emit(flags_dead ? "if (a) { const uint64_t v = %s; %s } "
                            : "if (!a) ef = f | F_ZF; else { const uint64_t v = %s; ef = f & ~(uint64_t)F_ZF; %s } ",
                 m == M(BSF) ? "(uint64_t)__builtin_ctzll(a)" : "(uint64_t)(63 - __builtin_clzll(a))", w.s);
        else if (m == M(POPCNT))
            emit("const uint64_t v = (uint64_t)__builtin_popcountll(a); %s%s ",
                 flags_dead ? "" : "ef = (f & ~(uint64_t)F_ARITH) | (a ? 0 : F_ZF); ", w.s);
        else
            emit("const uint64_t v = !a ? %d : %s; %s%s ", bits,
                 m == M(TZCNT) ? "(uint64_t)__builtin_ctzll(a)" : str("(uint64_t)(__builtin_clzll(a) - %d)", 64 - bits).s,
                 flags_dead ? "" : "ef = (f & ~(uint64_t)(F_CF | F_ZF)) | (!a ? F_CF : 0) | (!v ? F_ZF : 0); ", w.s);
        emit(flags_dead ? "}\n" : "fk = FK_CPU; }\n");
        return 1;
    }
    case M(ANDN): case M(BEXTR): case M(BLSR): case M(BLSI): case M(BLSMSK): {
        if (!gpr(d) || (size != 4 && size != 8) || !read_op(off, in, src, size, &a) || !write_op(off, in, d, "v", &w)) return 0;
        if ((m == M(ANDN) || m == M(BEXTR)) && (in->count < 3 || !read_op(off, in, &in->op[2], size, &b))) return 0;
        const uint64_t mk = mask(size);
        /* flags_dead: the value alone */
        emit("{ %sconst uint64_t s = %s; ", flags_dead ? "" : "uint64_t f = " FLAGS_NOW " & ~(uint64_t)F_ARITH; ", a.s);
        if (m == M(ANDN)) emit("const uint64_t v = ~s & (%s) & 0x%" PRIx64 "ull; ", b.s, mk);
        else if (m == M(BEXTR))
            emit("const uint64_t ctl = %s; const unsigned start = ctl & 0xff, len = (ctl >> 8) & 0xff; uint64_t v = start >= %d ? 0 : s >> start; "
                 "if (len < 64) v &= (1ull << len) - 1; v &= 0x%" PRIx64 "ull; ", b.s, bits, mk);
        else if (m == M(BLSR)) emit("const uint64_t v = s & (s - 1) & 0x%" PRIx64 "ull; %s", mk, flags_dead ? "" : "if (!s) f |= F_CF; ");
        else if (m == M(BLSI)) emit("const uint64_t v = s & (0 - s) & 0x%" PRIx64 "ull; %s", mk, flags_dead ? "" : "if (s) f |= F_CF; ");
        else emit("const uint64_t v = (s ^ (s - 1)) & 0x%" PRIx64 "ull; %s", mk, flags_dead ? "" : "if (!s) f |= F_CF; ");
        if (flags_dead) emit("%s }\n", w.s);
        else emit("if (!v) f |= F_ZF; if (v & 0x%" PRIx64 "ull) f |= F_SF; ef = f; fk = FK_CPU; %s }\n", mk ^ (mk >> 1), w.s);
        return 1;
    }
    case M(MUL): case M(IMUL): { /* one operand: rdx:rax = rax * it (two and three: translate_shift_mul) */
        if (in->count != 1 || !sext[size] || !read_op(off, in, d, size, &a)) return 0;
        const uint64_t mk = mask(size);
        emit("{ const uint64_t s = %s; const uint64_t a = r[RAX] & 0x%" PRIx64 "ull; uint64_t lo, hi; int ov; ", a.s, mk);
        if (m == M(MUL) && size == 8)
            emit("const unsigned __int128 full = (unsigned __int128)a * s; lo = (uint64_t)full; hi = (uint64_t)(full >> 64); ov = hi != 0; ");
        else if (m == M(MUL))
            emit("const uint64_t full = a * s; lo = full & 0x%" PRIx64 "ull; hi = (full >> %d) & 0x%" PRIx64 "ull; ov = hi != 0; ", mk, bits, mk);
        else if (size == 8)
            emit("const __int128 full = (__int128)(int64_t)a * (int64_t)s; lo = (uint64_t)full; hi = (uint64_t)((unsigned __int128)full >> 64); "
                 "ov = (__int128)(int64_t)lo != full; ");
        else
            emit("const int64_t full = (int64_t)(%s)a * (int64_t)(%s)s; lo = (uint64_t)full & 0x%" PRIx64 "ull; "
                 "hi = ((uint64_t)full >> %d) & 0x%" PRIx64 "ull; ov = (int64_t)(%s)lo != full; ", sext[size], sext[size], mk, bits, mk, sext[size]);
        if (size == 1) emit("r[RAX] = (r[RAX] & ~0xffffull) | lo | (hi << 8); ");
        else if (size == 2) emit("r[RAX] = (r[RAX] & ~0xffffull) | lo; r[RDX] = (r[RDX] & ~0xffffull) | hi; ");
        else emit("r[RAX] = lo; r[RDX] = hi; ");
        emit("ef = (" FLAGS_NOW " & ~(uint64_t)(F_CF | F_OF)) | (ov ? F_CF | F_OF : 0); fk = FK_CPU; }\n");
        return 1;
    }
    case M(DIV): case M(IDIV): { /* 32 and 64 bits; a zero divisor or a quotient too wide: the interpreter */
        if ((size != 4 && size != 8) || !read_op(off, in, d, size, &a)) return 0;
        if (m == M(DIV) && size == 4)
            emit("{ const uint64_t s = %s; const uint64_t n = ((r[RDX] & 0xffffffffull) << 32) | (r[RAX] & 0xffffffffull); "
                 "const int ok = s && (n >> 32) < s; uint64_t q = 0, rem = 0; if (ok) { q = n / s; rem = n %% s; } ", a.s);
        else if (m == M(DIV))
            emit("{ const uint64_t s = %s, hi = r[RDX], lo = r[RAX]; const int ok = s && hi < s; uint64_t q = 0, rem = 0; "
                 "if (ok && !hi) { q = lo / s; rem = lo %% s; } else if (ok) { const unsigned __int128 n = ((unsigned __int128)hi << 64) | lo; "
                 "q = (uint64_t)(n / s); rem = (uint64_t)(n %% s); } ", a.s);
        else if (size == 4)
            emit("{ const int64_t s = (int32_t)(%s); const int64_t n = (int64_t)(((r[RDX] & 0xffffffffull) << 32) | (r[RAX] & 0xffffffffull)); "
                 "int ok = s && !(s == -1 && n == INT64_MIN); int64_t q = 0, rem = 0; if (ok) { q = n / s; rem = n %% s; ok = q == (int32_t)q; } ", a.s);
        else
            emit("{ const int64_t s = (int64_t)(%s); const uint64_t hi = r[RDX], lo = r[RAX]; int ok = s != 0; int64_t q = 0, rem = 0; "
                 "if (ok && hi == (uint64_t)((int64_t)lo >> 63)) { if (s == -1 && lo == 0x8000000000000000ull) ok = 0; "
                 "else { q = (int64_t)lo / s; rem = (int64_t)lo %% s; } } else if (ok && s != -1) { "
                 "const __int128 n = (__int128)(((unsigned __int128)hi << 64) | lo), qq = n / s; ok = qq == (int64_t)qq; "
                 "q = (int64_t)qq; rem = (int64_t)(n %% s); } else ok = 0; ", a.s);
        emit("if (ok) { r[RAX] = %s; r[RDX] = %s; } else ", size == 4 ? "(uint32_t)q" : "(uint64_t)q", size == 4 ? "(uint32_t)rem" : "(uint64_t)rem");
        if (snippet_mode) emit_interp(off, in, 1);
        else emit_fault(off, in, "RC_FAULT_DIVIDE");
        emit(" }\n");
        return 1;
    }
    case M(ROL): case M(ROR): {
        if (!sext[size] || !rmw_dest(off, in, d, &load, &put)) return 0;
        const unsigned cmask = size == 8 ? 63 : 31;
        Str n;
        if (in->count < 2) n = str("1u");
        else if (src->type == OP_IMM) {
            if (!((unsigned)src->disp & cmask)) { emit("/* rotate by 0 */\n"); return 1; }
            n = str("%uu", (unsigned)src->disp & cmask);
        } else if (gpr(src) && src->reg == RCX && src->size == 1) n = str("((unsigned)r[RCX] & %u)", cmask);
        else return 0;
        const uint64_t mk = mask(size), sign = mk ^ (mk >> 1);
        emit("{ const unsigned n = %s; if (n) { %s const unsigned k = n %% %d; ", n.s, load.s, bits);
        if (m == M(ROL))
            emit("const uint64_t v = k ? ((a << k) | (a >> (%d - k))) & 0x%" PRIx64 "ull : a; const int cf = (int)(v & 1); "
                 "ef = (" FLAGS_NOW " & ~(uint64_t)(F_CF | F_OF)) | (cf ? F_CF : 0) | ((((v & 0x%" PRIx64 "ull) != 0) != cf) ? F_OF : 0); ",
                 bits, mk, sign);
        else
            emit("const uint64_t v = k ? ((a >> k) | (a << (%d - k))) & 0x%" PRIx64 "ull : a; const int msb = (v & 0x%" PRIx64 "ull) != 0, "
                 "msb2 = (v & 0x%" PRIx64 "ull) != 0; ef = (" FLAGS_NOW " & ~(uint64_t)(F_CF | F_OF)) | (msb ? F_CF : 0) | (msb != msb2 ? F_OF : 0); ",
                 bits, mk, sign, sign >> 1);
        emit("fk = FK_CPU; %s } }\n", put.s);
        return 1;
    }
    case M(BSWAP):
        if (!gpr(d) || (size != 4 && size != 8)) return 0;
        emit("r[%d] = %s;\n", d->reg, size == 8 ? str("__builtin_bswap64(r[%d])", d->reg).s : str("__builtin_bswap32((uint32_t)r[%d])", d->reg).s);
        return 1;
    case M(MOVBE): {
        if (size != 2 && size != 4 && size != 8) return 0;
        static const char *const swap[9] = {0, 0, "__builtin_bswap16((uint16_t)", 0, "__builtin_bswap32((uint32_t)", 0, 0, 0, "__builtin_bswap64((uint64_t)"};
        if (!read_op(off, in, src, size, &a) || !write_op(off, in, d, str("%s(%s))", swap[size], a.s).s, &w)) return 0;
        emit("%s\n", w.s);
        return 1;
    }
    case M(XCHG): {
        if (in->count != 2) return 0;
        if (gpr(d) && gpr(src)) {
            Str wa, wb;
            if (!read_op(off, in, d, size, &a) || !read_op(off, in, src, size, &b) || !write_op(off, in, d, "vb", &wa) ||
                !write_op(off, in, src, "va", &wb)) return 0;
            emit("{ const uint64_t va = %s, vb = %s; %s %s }\n", a.s, b.s, wa.s, wb.s);
            return 1;
        }
        const GuestOp *mem = d->type == OP_MEM ? d : src, *reg = d->type == OP_MEM ? src : d;
        Str ad;
        int stack;
        static const char *const type[9] = {0, "uint8_t", "uint16_t", 0, "uint32_t", 0, 0, 0, "uint64_t"};
        if (mem->type != OP_MEM || !gpr(reg) || !type[mem->size] || !address(off, in, mem, &ad, &stack) ||
            !read_op(off, in, reg, reg->size, &b) || !write_op(off, in, reg, "old", &w)) return 0;
        emit("{ const uint64_t old = __atomic_exchange_n((%s *)(%s), (%s)(%s), __ATOMIC_SEQ_CST); %s }\n", type[mem->size], ad.s,
             type[mem->size], b.s, w.s);
        return 1;
    }
    case M(RDTSC):
        emit("{ const uint64_t t = rc_tsc(); r[RAX] = (uint32_t)t; r[RDX] = t >> 32; }\n");
        return 1;
    default:
        return 0;
    }
}

/* The instructions of [start, start + size) the code reaches from the entry: fall-through, direct
 * jumps and branches inside the function (data between them, such as jump tables, is never decoded;
 * a place only an indirect jump reaches goes back to the translator). Sorted by offset. */
typedef struct { uint64_t off; GuestInsn in; } Insn;
static int insn_order(const void *a, const void *b) {
    const uint64_t x = ((const Insn *)a)->off, y = ((const Insn *)b)->off;
    return x < y ? -1 : x > y;
}
/* Targets of the jump tables decode found (labels too), for the function being generated. */
static uint64_t *table_targets;
/* The function has a jmp through a register that is not a jump table decode recognized (a
 * computed jump, such as into an unrolled loop): every decoded instruction gets a label, so its
 * targets inside the function dispatch there instead of going back to the translator. */
static int computed_jump;
/* The first such jmp, for the report. */
static uint64_t computed_jump_at;
static size_t table_target_count, table_target_capacity;
/* Over the run: jump tables decoded, and indirect jumps left to the translator (the stats line). */
static uint64_t tables_decoded, jumps_left;

/* The decoded instruction before `off` on a path to it: the one ending there that falls through,
 * else the single decoded branch that jumps there (`taken`: by its jump). NULL: none or several. */
static const Insn *path_before(const Insn *list, size_t n, uint64_t off, int *taken) {
    const Insn *found = NULL;
    for (size_t i = 0; i < n; ++i)
        if (list[i].off + list[i].in.length == off && falls_through(list[i].off, &list[i].in) &&
            list[i].in.mnemonic != M(JMP)) {
            *taken = 0;
            return &list[i];
        }
    for (size_t i = 0; i < n; ++i) {
        const GuestInsn *x = &list[i].in;
        const int cc = cc_of(x->mnemonic);
        if (((cc >= 0 && cc < 16) || x->mnemonic == M(JMP)) && x->op[0].type == OP_IMM &&
            list[i].off + (uint64_t)x->op[0].disp == off) {
            if (found) return NULL;
            found = &list[i];
        }
    }
    *taken = 1;
    return found;
}

/* Whether `x` leaves the flags alone: moves, lea, and the vector instructions other than the
 * compares and tests that set them. */
static int keeps_flags(const GuestInsn *x) {
    const int m = x->mnemonic;
    if (m == M(MOV) || m == M(MOVZX) || m == M(MOVSX) || m == M(MOVSXD) || m == M(LEA)) return 1;
    if (m == M(COMISS) || m == M(VCOMISS) || m == M(COMISD) || m == M(VCOMISD) || m == M(UCOMISS) ||
        m == M(VUCOMISS) || m == M(UCOMISD) || m == M(VUCOMISD) || m == M(PTEST) || m == M(VPTEST) ||
        m == M(VTESTPS) || m == M(VTESTPD))
        return 0;
    for (int k = 0; k < x->count; ++k)
        if (x->op[k].type == OP_REG && x->op[k].kind == RK_VEC) return 1;
    return 0;
}

/* Whether general registers x and y hold the same value at `off`: back along the path from it
 * through register moves only (up to 8), the last writes of the two are the same move of one
 * register, which nothing between them writes (movzx ecx, sil; movzx eax, sil; cmp eax, 8 at
 * 0x7d6e7a: the bound on eax, the table's index in rcx). */
static int copies_of_one(const Insn *list, size_t n, uint64_t off, int x, int y) {
    const GuestInsn *first = NULL; /* the write of x or y nearer `off` */
    int taken = 0;
    for (int steps = 0; steps < 8; ++steps) {
        const Insn *p = path_before(list, n, off, &taken);
        if (!p || taken) return 0;
        const GuestInsn *i = &p->in;
        if ((i->mnemonic != M(MOV) && i->mnemonic != M(MOVZX)) || !gpr(&i->op[0]) || !gpr(&i->op[1]) ||
            i->op[0].size < 4)
            return 0;
        const int d = i->op[0].reg;
        if (!first) {
            if (d == x || d == y) {
                if (i->op[1].reg == d) return 0;
                first = i;
            }
        } else {
            const int other = first->op[0].reg == x ? y : x;
            if (d == other)
                return i->mnemonic == first->mnemonic && i->op[0].size == first->op[0].size &&
                       i->op[1].reg == first->op[1].reg && i->op[1].size == first->op[1].size &&
                       i->op[1].high8 == first->op[1].high8;
            if (d == first->op[1].reg) return 0; /* the source written between the two */
        }
        off = p->off;
    }
    return 0;
}

/* A jump table's size from the code before its load when the bound check is not next to it: back
 * along the path (through the branch into the table's block), the index followed as `reg & mask`
 * through copies and masks, to `cmp X, N` with ja falling through (or jbe taken) where X is the
 * index or `mov X, reg; and X, mask` of it. 0: not found. */
static uint64_t table_bound(const Insn *list, size_t n, int index, uint64_t load_at) {
    int reg = index, taken = 0;
    uint64_t mask = ~UINT64_C(0);
    uint64_t at = load_at;
    for (int steps = 0; steps < 24; ++steps) {
        const Insn *p = path_before(list, n, at, &taken);
        if (!p) return 0;
        const GuestInsn *x = &p->in;
        const int m = x->mnemonic;
        if (m == M(JNBE) || m == M(JBE)) {
            if ((m == M(JNBE)) == taken) return 0; /* the path where the index is above the bound */
            const Insn *c = path_before(list, n, p->off, &taken);
            /* Between the cmp and the branch: what keeps the flags (clang's vector stores and moves,
             * a conditional branch the path falls through: cmp eax, 4; jg; mov eax, eax; ja at
             * 0x775dcf), the index copied there from another register (mov I, X) followed back to X. */
            for (int skip = 0; c && !taken && c->in.mnemonic != M(CMP) && skip < 8 &&
                               (keeps_flags(&c->in) || (cc_of(c->in.mnemonic) >= 0 && cc_of(c->in.mnemonic) < 16));
                 ++skip) {
                if (c->in.count && gpr(&c->in.op[0]) && c->in.op[0].reg == reg) {
                    if (c->in.mnemonic != M(MOV) || !gpr(&c->in.op[1]) || c->in.op[0].size < 4 || mask != ~UINT64_C(0)) return 0;
                    reg = c->in.op[1].reg;
                }
                c = path_before(list, n, c->off, &taken);
            }
            if (!c || taken || c->in.mnemonic != M(CMP) || c->in.op[0].type != OP_REG || c->in.op[1].type != OP_IMM)
                return 0;
            const uint64_t entries = (uint64_t)c->in.op[1].disp + 1;
            const int r = c->in.op[0].reg;
            if (r == reg && mask == ~UINT64_C(0)) return entries;
            if (mask == ~UINT64_C(0) && copies_of_one(list, n, c->off, r, reg)) return entries;
            /* mov I, X; cmp X, N: the index a copy of X */
            const Insn *a = path_before(list, n, c->off, &taken);
            if (a && !taken && mask == ~UINT64_C(0) && a->in.mnemonic == M(MOV) && gpr(&a->in.op[0]) &&
                a->in.op[0].reg == reg && a->in.op[0].size >= 4 && gpr(&a->in.op[1]) && a->in.op[1].reg == r)
                return entries;
            /* mov X, reg; and X, mask; cmp X, N: X is the index too */
            const Insn *v = a && !taken ? path_before(list, n, a->off, &taken) : NULL;
            if (!v || taken || a->in.mnemonic != M(AND) || !gpr(&a->in.op[0]) || a->in.op[0].reg != r ||
                a->in.op[1].type != OP_IMM || (uint64_t)a->in.op[1].disp != mask || v->in.mnemonic != M(MOV) ||
                !gpr(&v->in.op[0]) || v->in.op[0].reg != r || !gpr(&v->in.op[1]) || v->in.op[1].reg != reg)
                return 0;
            return entries <= mask + 1 ? entries : mask + 1;
        }
        if (taken || m == M(CALL) || m == M(RET)) return 0;
        /* What the step back passes: writes of the index's register are copies or masks. */
        if (x->count && gpr(&x->op[0]) && x->op[0].reg == reg) {
            if (m == M(AND) && x->op[1].type == OP_IMM && x->op[1].disp > 0 && x->op[1].disp < 4096 &&
                !((x->op[1].disp + 1) & x->op[1].disp))
                mask &= (uint64_t)x->op[1].disp;
            else if (m == M(MOV) && gpr(&x->op[1]) && x->op[0].size >= 4) reg = x->op[1].reg;
            else return 0;
        }
        at = p->off;
    }
    return 0;
}

/* Whether `x`, placed between a jump table's add and its jmp, leaves general register `c` alone:
 * the instructions clang moves there (the case's common code: moves, lea, xor, bextr, bswap,
 * vector instructions) and nothing with an implicit register write (push, pop, mul, div, call,
 * string instructions). */
static int leaves_alone(const GuestInsn *x, int c) {
    const int m = x->mnemonic;
    if (x->count && x->op[0].type == OP_REG && x->op[0].kind == RK_VEC) return 1;
    if (x->count == 2 && x->op[0].type == OP_MEM && x->op[1].type == OP_REG && x->op[1].kind == RK_VEC) return 1;
    if (!(m == M(MOV) || m == M(LEA) || m == M(XOR) || m == M(AND) || m == M(OR) || m == M(SUB) || m == M(ADD) ||
          m == M(MOVZX) || m == M(MOVSX) || m == M(MOVSXD) || m == M(SHL) || m == M(SHR) || m == M(SAR) ||
          m == M(NEG) || m == M(NOT) || m == M(INC) || m == M(DEC) || m == M(TEST) || m == M(CMP) || m == M(BEXTR) ||
          m == M(BSWAP)))
        return 0;
    return !(x->count && gpr(&x->op[0]) && x->op[0].reg == c);
}

/* Whether a table of `entries` (1 to 4096) relative offsets at `table` has its targets in `fn`. */
static int table_in(Function fn, uint64_t table, uint64_t entries) {
    if (!entries || entries > 4096) return 0;
    for (uint64_t e = 0; e < entries; ++e) {
        int32_t rel;
        memcpy(&rel, (const void *)(uintptr_t)(base + table + 4 * e), 4);
        const uint64_t target = table + (uint64_t)(int64_t)rel;
        if (target < fn.offset || target >= fn.offset + fn.size) return 0;
    }
    return 1;
}

/* A decode of `fn` from its first byte to its last, one instruction after the other: where its
 * instructions start (sweep_starts, a byte per byte of the function), and the addresses they take
 * relative to rip (sweep_refs: its tables and constants). */
static uint64_t sweep_of = ~UINT64_C(0);
static uint8_t *sweep_starts;
static uint64_t *sweep_refs;
static size_t sweep_ref_count;
static void sweep(Function fn) {
    if (sweep_of == fn.offset) return;
    sweep_of = fn.offset;
    sweep_starts = realloc(sweep_starts, fn.size ? fn.size : 1);
    memset(sweep_starts, 0, fn.size);
    sweep_ref_count = 0;
    size_t capacity = 0;
    for (uint64_t off = fn.offset; off < fn.offset + fn.size;) {
        GuestBlock *block = guest_cpu_block_uncached(base + off);
        if (!block || !block->count) { /* not an instruction: the decode goes on a byte further */
            free(block);
            ++off;
            continue;
        }
        for (uint32_t i = 0; i < block->count && off < fn.offset + fn.size; ++i) {
            const GuestInsn *in = &block->insn[i];
            sweep_starts[off - fn.offset] = 1;
            for (int o = 0; o < in->count; ++o) {
                if (in->op[o].type != OP_MEM && in->op[o].type != OP_AGEN) continue;
                if (in->op[o].base != 0xfe) continue;
                if (sweep_ref_count == capacity) {
                    capacity = capacity ? capacity * 2 : 256;
                    sweep_refs = realloc(sweep_refs, capacity * sizeof(*sweep_refs));
                }
                sweep_refs[sweep_ref_count++] = off + in->length + (uint64_t)in->op[o].disp;
            }
            off += in->length;
        }
        free(block);
    }
}

/* The length of the table of relative offsets at `table` when the code gives no bound of the
 * index: its entries from the first one (up to 4096) that lead to the start of an instruction of
 * `fn`, up to the next address the function's code refers to (its next table, whose entries are
 * relative to that table and lead elsewhere from this one). */
static uint64_t table_length(Function fn, uint64_t table) {
    sweep(fn);
    uint64_t end = image_size;
    for (size_t i = 0; i < sweep_ref_count; ++i)
        if (sweep_refs[i] > table && sweep_refs[i] < end) end = sweep_refs[i];
    uint64_t entries = 0;
    while (entries < 4096 && table + 4 * entries + 4 <= end) {
        int32_t rel;
        memcpy(&rel, (const void *)(uintptr_t)(base + table + 4 * entries), 4);
        const uint64_t target = table + (uint64_t)(int64_t)rel;
        if (target < fn.offset || target >= fn.offset + fn.size || !sweep_starts[target - fn.offset]) break;
        ++entries;
    }
    return entries;
}

/* A compiler's jump table ending at list[n - 1] (jmp reg): lea T, [rip + table]; movsxd C,
 * dword [T + 4 * I]; add C, T; jmp C, its size from cmp I, N; ja (N + 1 entries; or jbe to the
 * table, the default case falling through). Between the add and the jmp, instructions that leave C
 * alone; T may be a copy (mov T, X) of the register the lea wrote. Its targets
 * are pushed to `work` and kept in table_targets; 0 if it is not one. */
static int jump_table(Function fn, const Insn *list, size_t n, uint64_t *work, size_t *pending, const uint8_t *seen) {
    if (n < 4) return 0;
    const GuestInsn *jmp = &list[n - 1].in;
    if (jmp->op[0].type != OP_REG || jmp->op[0].kind != RK_GPR || jmp->op[0].size != 8) return 0;
    const int c = jmp->op[0].reg;
    size_t a = n - 1;
    while (a-- > 1 && a + 96 > n) { /* the case's common code between them: 70 instructions at 0x12c14ed */
        const GuestInsn *x = &list[a].in;
        if (x->mnemonic == M(ADD) && gpr(&x->op[0]) && x->op[0].reg == c) break;
        if (!leaves_alone(x, c)) return 0;
    }
    const GuestInsn *add = &list[a].in;
    if (add->mnemonic != M(ADD) || add->op[0].type != OP_REG || add->op[0].reg != c || add->op[1].type != OP_REG ||
        add->op[0].size != 8) return 0;
    const int t = add->op[1].reg;
    /* The load: right before the add, or with instructions between them that leave C and T alone
     * (luaV_execute: shl and lea of the case's common code). */
    size_t l = a - 1;
    while (!(list[l].in.mnemonic == M(MOVSXD) && list[l].in.op[0].type == OP_REG && list[l].in.op[0].reg == c)) {
        if (!l || l + 8 <= a || !leaves_alone(&list[l].in, c) || !leaves_alone(&list[l].in, t)) return 0;
        --l;
    }
    const GuestInsn *load = &list[l].in;
    if (load->op[1].type != OP_MEM || load->op[1].base != t || load->op[1].scale != 4 || load->op[1].index == 0xff ||
        load->op[1].disp != 0) return 0;
    const int index = load->op[1].index;
    uint64_t table = 0, entries = 0;
    int tr = t; /* the register the table's address was in: T, or what T was copied from */
    for (size_t k = l; k-- > 0 && k + 32 > l;) { /* the nearest; the default case can sit between */
        const Insn *x = &list[k];
        if (!table && x->in.mnemonic == M(LEA) && x->in.op[0].type == OP_REG && x->in.op[0].reg == tr &&
            x->in.op[1].base == 0xfe && x->in.op[1].index == 0xff)
            table = x->off + x->in.length + (uint64_t)x->in.op[1].disp;
        else if (!table && x->in.mnemonic == M(MOV) && gpr(&x->in.op[0]) && x->in.op[0].reg == tr &&
                 x->in.op[0].size == 8 && gpr(&x->in.op[1]) && x->in.op[1].size == 8)
            tr = x->in.op[1].reg;
        if (!entries && x->in.mnemonic == M(CMP) && x->in.op[0].type == OP_REG && x->in.op[0].reg == index &&
            x->in.op[1].type == OP_IMM && k + 1 < n &&
            (list[k + 1].in.mnemonic == M(JNBE) || list[k + 1].in.mnemonic == M(JBE)))
            entries = (uint64_t)x->in.op[1].disp + 1;
    }
    /* Also: cmp R, N, then flag-free moves (the index copied from R: mov I, R), then ja
     * (clang interleaves the stores of the case's common code). */
    for (size_t k = l; !entries && k-- > 0 && k + 32 > l;) {
        if (list[k].in.mnemonic != M(JNBE) && list[k].in.mnemonic != M(JBE)) continue;
        int from = -1; /* the register the bound applies to */
        for (size_t j = k; j-- > 0 && j + 8 > k;) {
            const GuestInsn *x = &list[j].in;
            if (x->mnemonic == M(CMP)) {
                if (x->op[0].type == OP_REG && x->op[1].type == OP_IMM) from = x->op[0].reg, entries = (uint64_t)x->op[1].disp + 1;
                break;
            }
            if (x->mnemonic != M(MOV) && x->mnemonic != M(LEA) && x->mnemonic != M(MOVZX) && x->mnemonic != M(MOVSXD)) break;
        }
        if (!entries) break;
        /* The index is R, or a copy of R made after the cmp (before the table load). */
        int ok = from == index;
        for (size_t j = k + 1; !ok && j < l; ++j) {
            const GuestInsn *x = &list[j].in;
            if (x->mnemonic == M(MOV) && x->op[0].type == OP_REG && x->op[0].reg == index && x->op[1].type == OP_REG &&
                x->op[1].reg == from) ok = 1;
        }
        for (size_t j = k; !ok && j-- > 0 && j + 8 > k;) { /* or just before it: mov I, R; cmp R, N */
            const GuestInsn *x = &list[j].in;
            if (x->mnemonic == M(CMP)) continue;
            if (x->mnemonic == M(MOV) && x->op[0].type == OP_REG && x->op[0].reg == index && x->op[1].type == OP_REG &&
                x->op[1].reg == from) ok = 1;
        }
        if (!ok) entries = 0;
        break;
    }
    if (!table) { /* the lea hoisted out of a loop: the nearest one before the load, in address order */
        uint64_t best_off = 0;
        for (size_t k = 0; k + 1 < a; ++k) {
            const Insn *x = &list[k];
            if (x->in.mnemonic == M(LEA) && x->in.op[0].type == OP_REG && x->in.op[0].reg == tr &&
                x->in.op[1].base == 0xfe && x->in.op[1].index == 0xff && x->off < list[l].off && x->off >= best_off) {
                best_off = x->off;
                table = x->off + x->in.length + (uint64_t)x->in.op[1].disp;
            }
        }
    }
    /* The bound on the path into the table's block when there is one: one found nearby can be
     * another table's or branch's (what decode found just before the block need not be on its path,
     * as in mov I, R; cmp R, N; jbe block). */
    if (table) {
        const uint64_t path = table_bound(list, n, index, list[l].off);
        if (path) entries = path;
        /* No bound found (the compare far before its branch, the index kept on the stack between
         * them, sub in place of cmp): the table's own length. */
        if (!entries) entries = table_length(fn, table);
    }
    if (!table || !table_in(fn, table, entries)) return 0; /* not what it looked like */
    jump_tables = realloc(jump_tables, (jump_table_count + 1) * sizeof(*jump_tables));
    JumpTable *jt = &jump_tables[jump_table_count++];
    *jt = (JumpTable){list[n - 1].off, list[l].off, entries, malloc((entries ? entries : 1) * sizeof(uint64_t)), index};
    for (uint64_t e = 0; e < entries; ++e) {
        int32_t rel;
        memcpy(&rel, (const void *)(uintptr_t)(base + table + 4 * e), 4);
        const uint64_t target = table + (uint64_t)(int64_t)rel;
        jt->targets[e] = target;
        if (table_target_count == table_target_capacity) {
            table_target_capacity = table_target_capacity ? table_target_capacity * 2 : 256;
            table_targets = realloc(table_targets, table_target_capacity * sizeof(*table_targets));
        }
        table_targets[table_target_count++] = target;
        if (!seen[target - fn.offset]) work[(*pending)++] = target;
    }
    return 1;
}

/* Whether the jmp through a register at list[n - 1] may go inside the function: the register is the
 * sum an add of a register made (a jump table jump_table did not read). One loaded as it is (a
 * function pointer, a vtable entry, the function a thunk goes on to) is a tail call. */
static int table_like(const Insn *list, size_t n) {
    const int c = list[n - 1].in.op[0].reg;
    for (size_t a = n - 1; a-- > 0 && a + 96 > n;) {
        const GuestInsn *x = &list[a].in;
        if (x->mnemonic == M(ADD) && gpr(&x->op[0]) && x->op[0].reg == c) return x->op[1].type == OP_REG;
        if (!leaves_alone(x, c)) return 0;
    }
    return 0;
}

/* The cave at `t`, the entry of a function the patches wrote code over, when that code jumps back
 * into `fn`: a patch's detour out of `fn` and back, placed in a function the game leaves unused
 * (the frame rate patches' at 0x10c700, from 0x7db931), code of `fn` as any other cave. NULL
 * otherwise: a jump to a function's entry is a tail call. */
static const Range *detour_cave_at(Function fn, uint64_t t) {
    const long f = cave_count ? function_at(t) : -1;
    if (f < 0 || functions[f].offset != t) return NULL;
    const Range *cave = NULL;
    for (size_t lo = 0, hi = cave_count; lo < hi && !cave;) {
        const size_t mid = (lo + hi) / 2;
        if (t < caves[mid].start) hi = mid;
        else if (t >= caves[mid].end) lo = mid + 1;
        else cave = &caves[mid];
    }
    for (uint64_t off = t; cave && off >= cave->start && off < cave->end;) {
        GuestBlock *block = guest_cpu_block_uncached(base + off);
        int back = block && block->count ? -1 : 0; /* 1: a jump into fn; 0: the code ends another way */
        for (uint32_t i = 0; back < 0 && i < block->count; ++i) {
            const GuestInsn *in = &block->insn[i];
            const int m = in->mnemonic, cc = cc_of(m);
            if ((m == M(JMP) || (cc >= 0 && cc < 16)) && in->op[0].type == OP_IMM &&
                off + (uint64_t)in->op[0].disp >= fn.offset && off + (uint64_t)in->op[0].disp < fn.offset + fn.size)
                back = 1;
            else if (!falls_through(off, in)) back = 0;
            off += in->length;
        }
        free(block);
        if (back >= 0) return back ? cave : NULL;
    }
    return NULL;
}

/* The decoded mark of `off` in the function being decoded (its own bytes, `seen`) or in a cave of
 * the patches being applied, which a jump (`enter`) adds to the function; NULL: neither. */
static uint8_t *entered_seen[MAX_ENTERED];
static uint8_t *seen_at(Function fn, uint8_t *seen, uint64_t off, int enter) {
    if (off >= fn.offset && off < fn.offset + fn.size) return &seen[off - fn.offset];
    for (size_t i = 0; i < entered_count; ++i)
        if (off >= entered[i].start && off < entered[i].end) return &entered_seen[i][off - entered[i].start];
    const Range *cave = enter ? cave_at(off) : NULL;
    if (!cave && enter) cave = detour_cave_at(fn, off);
    if (!cave) return NULL;
    if (entered_count == MAX_ENTERED) {
        fprintf(stderr, "recompile: %#" PRIx64 " jumps into more than %d caves\n", fn.offset, MAX_ENTERED);
        exit(1);
    }
    entered[entered_count] = *cave;
    entered_seen[entered_count] = calloc(cave->end - cave->start, 1);
    return &entered_seen[entered_count++][off - cave->start];
}

static Insn *decode(Function fn, size_t *count) {
    Insn *list = NULL;
    size_t n = 0, capacity = 0;
    uint8_t *seen = calloc(fn.size + 16, 1);
    size_t work_capacity = fn.size + 16 + 4096;
    uint64_t *work = malloc(work_capacity * sizeof(*work));
    size_t pending = 0;
    table_target_count = 0;
    for (size_t i = 0; i < jump_table_count; ++i) free(jump_tables[i].targets);
    jump_table_count = 0;
    computed_jump = 0;
    for (size_t i = 0; i < entered_count; ++i) free(entered_seen[i]);
    entered_count = 0;
    cave_call_count = 0;
    span_count = 0;
    work[pending++] = fn.offset;
    while (pending) {
        uint64_t off = work[--pending];
        uint8_t *mark;
        while ((mark = seen_at(fn, seen, off, 0)) && !*mark) {
            GuestBlock *own = patches_on ? guest_cpu_block_uncached(base + off) : NULL;
            const GuestBlock *block = own ? own : guest_cpu_block(base + off);
            if (!block->count) {
                free(own);
                break;
            }
            int ended = 0;
            for (uint32_t i = 0; i < block->count && !ended; ++i) {
                mark = seen_at(fn, seen, off, 0);
                if (!mark || *mark) { ended = 1; break; }
                const GuestInsn *in = &block->insn[i];
                *mark = 1;
                if (n == capacity) {
                    capacity = capacity ? capacity * 2 : 256;
                    list = realloc(list, capacity * sizeof(*list));
                }
                list[n++] = (Insn){off, *in};
                const int m = in->mnemonic, cc = cc_of(m);
                if (cave_count && m == M(CALL) && in->op[0].type == OP_IMM && cave_at(off + (uint64_t)in->op[0].disp)) {
                    cave_calls = realloc(cave_calls, (cave_call_count + 1) * sizeof(*cave_calls));
                    cave_calls[cave_call_count++] = off + (uint64_t)in->op[0].disp;
                }
                /* A cave's address taken (a callback the patches pass on): its code is a function
                 * of the patches too. */
                if (cave_count && m == M(LEA) && in->op[1].base == 0xfe && in->op[1].index == 0xff &&
                    cave_at(off + in->length + (uint64_t)in->op[1].disp)) {
                    cave_calls = realloc(cave_calls, (cave_call_count + 1) * sizeof(*cave_calls));
                    cave_calls[cave_call_count++] = off + in->length + (uint64_t)in->op[1].disp;
                }
                if (((cc >= 0 && cc < 16) || m == M(JMP) || m == M(JRCXZ) || m == M(JECXZ) || m == M(LOOP) ||
                     m == M(LOOPE) || m == M(LOOPNE)) && in->op[0].type == OP_IMM) {
                    const uint64_t t = off + (uint64_t)in->op[0].disp;
                    const uint8_t *target = seen_at(fn, seen, t, 1);
                    if (target && !*target) {
                        if (pending + 4096 >= work_capacity) work = realloc(work, (work_capacity *= 2) * sizeof(*work));
                        work[pending++] = t;
                    }
                }
                if (m == M(JMP) && in->op[0].type == OP_REG) {
                    if (pending + 4096 < work_capacity && jump_table(fn, list, n, work, &pending, seen)) ++tables_decoded;
                    else if (++jumps_left, table_like(list, n)) {
                        if (!computed_jump) computed_jump_at = off;
                        computed_jump = 1;
                    }
                }
                /* No fall-through after these. */
                if (!falls_through(off, in)) ended = 2;
                off += in->length;
            }
            free(own);
            if (ended == 2) break;
        }
    }
    free(work);
    free(seen);
    qsort(list, n, sizeof(*list), insn_order);
    for (size_t i = 0; i < n && entered_count; ++i) {
        if (list[i].off >= fn.offset && list[i].off < fn.offset + fn.size) continue;
        const uint64_t end = list[i].off + list[i].in.length;
        if (span_count && spans[span_count - 1].end >= list[i].off) {
            if (end > spans[span_count - 1].end) spans[span_count - 1].end = end;
            continue;
        }
        spans = realloc(spans, (span_count + 1) * sizeof(*spans));
        spans[span_count++] = (Range){list[i].off, end};
    }
    if (getenv("GUEST_RECOMP_OVERLAPS"))
        for (size_t i = 0; i + 1 < n; ++i)
            if (list[i].off + list[i].in.length > list[i + 1].off) {
                fprintf(stderr, "overlap %#llx at %#llx\n", (unsigned long long)fn.offset, (unsigned long long)list[i + 1].off);
                break;
            }
    *count = n;
    return list;
}

/* GUEST_RECOMP_TRACE=1: a trace point before each instruction (replay --lockstep). */
static int trace_points;
static uint64_t snippet_line;

static int by_offset(const void *a, const void *b) {
    const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

/* --abi: the functions other code jumps to, rather than calls, with a frame of its own still on
 * the stack. One may be a block a compiler moved out of the function that jumps to it, which runs
 * with that function's registers: its returns store all the callee-saved registers they pop. A
 * jump with the stack as at the jumping function's entry (stack_depths) is a tail call, and its
 * target a function as any other. */
static uint64_t *jumped_to;
static size_t jumped_to_count;
static int is_jumped_to(uint64_t t) {
    return jumped_to_count && bsearch(&t, jumped_to, jumped_to_count, sizeof(*jumped_to), by_offset) != NULL;
}

/* The bytes of stack below the entry's RSP at each instruction's start, or DEPTH_UNKNOWN (also
 * where the walk does not come): what pushes, pops and constants added to RSP make it, and the
 * frame pointer (mov rbp, rsp; leave; lea rsp, [rbp + n]), along the direct jumps and from a
 * jump table's jmp to its targets. */
#define DEPTH_UNKNOWN INT64_MIN
#define DEPTH_UNSET (INT64_MIN + 1)
static void depth_merge(int64_t *depth, int64_t *frame, size_t t, int64_t d, int64_t fp, int *changed) {
    if (depth[t] == DEPTH_UNSET) {
        depth[t] = d;
        frame[t] = fp;
        *changed = 1;
        return;
    }
    if (depth[t] != d && depth[t] != DEPTH_UNKNOWN) { depth[t] = DEPTH_UNKNOWN; *changed = 1; }
    if (frame[t] != fp && frame[t] != DEPTH_UNKNOWN) { frame[t] = DEPTH_UNKNOWN; *changed = 1; }
}
static int64_t *stack_depths(const Insn *list, size_t count, Range f) {
    int64_t *depth = malloc((count + 1) * sizeof(*depth)), *frame = malloc((count + 1) * sizeof(*frame));
    for (size_t k = 0; k < count; ++k) depth[k] = frame[k] = DEPTH_UNSET;
    const Insn *entry = bsearch(&f.start, list, count, sizeof(*list), by_offset);
    if (entry) {
        depth[entry - list] = 0;
        frame[entry - list] = DEPTH_UNKNOWN; /* rbp: the caller's */
    }
    for (int changed = 1; changed;) {
        changed = 0;
        for (size_t k = 0; k < count; ++k) {
            if (depth[k] == DEPTH_UNSET) continue;
            const GuestInsn *in = &list[k].in;
            const GuestOp *a = &in->op[0], *b = &in->op[1];
            const int m = in->mnemonic, cc = cc_of(m);
            const int to_rsp = in->count && gpr(a) && a->reg == RSP, to_rbp = in->count && gpr(a) && a->reg == RBP;
            int64_t d = depth[k], fp = frame[k]; /* fp: the depth rbp points at */
            if (m == M(PUSH) || m == M(PUSHFQ)) {
                if (d != DEPTH_UNKNOWN) d += in->opsize == 2 ? 2 : 8;
            } else if (m == M(POP) || m == M(POPFQ)) {
                if (d != DEPTH_UNKNOWN) d -= in->opsize == 2 ? 2 : 8;
                if (m == M(POP) && to_rsp) d = DEPTH_UNKNOWN;
                if (m == M(POP) && to_rbp) fp = DEPTH_UNKNOWN;
            } else if ((m == M(SUB) || m == M(ADD)) && to_rsp && a->size == 8 && b->type == OP_IMM) {
                if (d != DEPTH_UNKNOWN) d += m == M(SUB) ? b->disp : -b->disp;
            } else if (m == M(LEA) && to_rsp && a->size == 8 && b->index == 0xff && b->base == RSP) {
                if (d != DEPTH_UNKNOWN) d -= b->disp;
            } else if (m == M(LEA) && to_rsp && a->size == 8 && b->index == 0xff && b->base == RBP) {
                d = fp == DEPTH_UNKNOWN ? DEPTH_UNKNOWN : fp - b->disp;
            } else if (m == M(MOV) && to_rbp && a->size == 8 && gpr(b) && b->reg == RSP) {
                fp = d;
            } else if (m == M(MOV) && to_rsp && a->size == 8 && gpr(b) && b->reg == RBP) {
                d = fp;
            } else if (m == M(LEAVE)) {
                d = fp == DEPTH_UNKNOWN ? DEPTH_UNKNOWN : fp - 8;
                fp = DEPTH_UNKNOWN;
            } else if (m == M(ENTER)) {
                d = fp = DEPTH_UNKNOWN;
            } else if (m != M(CMP) && m != M(TEST)) { /* any other write of them (and rsp, -16) */
                const int swaps = (m == M(XCHG) || m == M(XADD)) && gpr(b);
                if (to_rsp || (swaps && b->reg == RSP)) d = DEPTH_UNKNOWN;
                if (to_rbp || (swaps && b->reg == RBP)) fp = DEPTH_UNKNOWN;
            }
            if (falls_through(list[k].off, in)) {
                const uint64_t next = list[k].off + in->length;
                const Insn *t = bsearch(&next, list, count, sizeof(*list), by_offset);
                if (t) depth_merge(depth, frame, (size_t)(t - list), d, fp, &changed);
            }
            if (((cc >= 0 && cc < 16) || m == M(JMP) || m == M(JRCXZ) || m == M(JECXZ) || m == M(LOOP) || m == M(LOOPE) ||
                 m == M(LOOPNE)) && a->type == OP_IMM) {
                const uint64_t to = list[k].off + (uint64_t)a->disp;
                const Insn *t = inside(f, to) ? bsearch(&to, list, count, sizeof(*list), by_offset) : NULL;
                if (t) depth_merge(depth, frame, (size_t)(t - list), d, fp, &changed);
            } else if (m == M(JMP) && jump_table_at(list[k].off)) { /* to its jump table's targets */
                const JumpTable *jt = jump_table_at(list[k].off);
                for (uint64_t j = 0; j < jt->entries; ++j) {
                    const Insn *t = bsearch(&jt->targets[j], list, count, sizeof(*list), by_offset);
                    if (t) depth_merge(depth, frame, (size_t)(t - list), d, fp, &changed);
                }
            }
        }
    }
    for (size_t k = 0; k < count; ++k)
        if (depth[k] == DEPTH_UNSET) depth[k] = DEPTH_UNKNOWN;
    free(frame);
    return depth;
}

static void find_jumped_to(void) {
    size_t tail_calls = 0;
    for (size_t i = 0; i < function_count; ++i) {
        size_t count;
        Insn *list = decode(functions[i], &count);
        const Range f = {functions[i].offset, functions[i].offset + functions[i].size};
        int64_t *depth = stack_depths(list, count, f);
        for (size_t k = 0; k < count; ++k) {
            const GuestInsn *in = &list[k].in;
            const int m = in->mnemonic, cc = cc_of(m);
            if (!((cc >= 0 && cc < 16) || m == M(JMP) || m == M(JRCXZ) || m == M(JECXZ) || m == M(LOOP) ||
                  m == M(LOOPE) || m == M(LOOPNE)) || in->op[0].type != OP_IMM)
                continue;
            const uint64_t t = list[k].off + (uint64_t)in->op[0].disp;
            if (inside(f, t) || !is_recompiled(t)) continue;
            if (!depth[k]) { /* a tail call */
                ++tail_calls;
                continue;
            }
            jumped_to = realloc(jumped_to, (jumped_to_count + 1) * sizeof(*jumped_to));
            jumped_to[jumped_to_count++] = t;
        }
        free(depth);
        free(list);
    }
    qsort(jumped_to, jumped_to_count, sizeof(*jumped_to), by_offset);
    fprintf(stderr, "jumps to other functions: %zu tail calls, %zu with a frame on the stack\n", tail_calls, jumped_to_count);
}

/* ---- register sync ---- */

/* generate writes a function's instructions with the SPILL/RELOAD markers, then fills them in from
 * the text, with two analyses over the gotos between the instructions (registers as masks: r[n]
 * bits 0-15, xmm[n] bits 16-31):
 * - dirty (forward): the registers whose local may differ from the GuestCpu's, written since the
 *   last RELOAD. A SPILL stores those.
 * - must (forward): the dirty registers written on every path there. A SPILL stores a vector
 *   register dirty on some paths only where the function's mask dv has it (rc_vset marks each
 *   write; a RELOAD clears dv), so that such a register is not read where nothing wrote it.
 * - live (backward): the registers read before they are written again; a SPILL reads its dirty
 *   ones (those it stores always). A RELOAD loads those, and the entry; the others keep their
 *   value in the GuestCpu only, until an instruction overwrites them whole (mov, movzx, movsx,
 *   lea, pop into a 32- or 64-bit register, its only mention).
 * The flags go with the markers too: a RELOAD loads ef, a SPILL stores it if the function writes
 * it other than by FLAGS_OUT (ef_stored). */
enum { EV_USE, EV_WRITE, EV_KILL, EV_SPILL, EV_RELOAD, EV_RELOAD_IF, EV_GOTO, EV_DISPATCH };
typedef struct { int kind; uint32_t mask; uint64_t target; } SyncEvent;
static SyncEvent *events;
static size_t event_count, event_capacity;
static const uint32_t *marker_masks, *marker_conds; /* for the markers in order, while writing the text */
static const uint32_t *marker_kills; /* the vector registers each RELOAD sets to a constant */
static size_t marker_next;
static int ef_stored;

static int ident_char(char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_'; }

static void event(int kind, uint32_t mask, uint64_t target) {
    if (event_count == event_capacity) {
        event_capacity = event_capacity ? 2 * event_capacity : 4096;
        events = realloc(events, event_capacity * sizeof(*events));
    }
    events[event_count++] = (SyncEvent){kind, mask, target};
}

/* Whether text writes ef other than by FLAGS_OUT (which stores c->flags itself). */
static int writes_ef(const char *text, size_t n) {
    static const char flags_out[] = FLAGS_OUT;
    for (size_t i = 0; i + 2 < n; ++i) {
        if (text[i] != 'e' || text[i + 1] != 'f' || (i && ident_char(text[i - 1])) || ident_char(text[i + 2])) continue;
        size_t p = i + 2;
        while (p < n && text[p] == ' ') ++p;
        if (p + 1 >= n) continue;
        if (text[p] == '=' && text[p + 1] != '=') {
            if (n - i >= sizeof(flags_out) - 1 && !memcmp(text + i, flags_out, sizeof(flags_out) - 1)) continue;
            return 1;
        }
        if (text[p + 1] == '=' && strchr("|&^+-", text[p])) return 1;
    }
    return 0;
}

/* The copies of the registers in `mask` (loads, or stores), and the stores of the vector registers
 * in `cond` where dv has them. The ABI's markers (abi) leave the flags: no call or return of code a
 * compiler wrote keeps them. */
static void sync_copy(FILE *o, uint32_t mask, uint32_t cond, int load, int abi) {
    for (int k = 0; k < 16; ++k)
        if (mask >> k & 1) fprintf(o, load ? " r[%d] = c->r[%d];" : " c->r[%d] = r[%d];", k, k);
    for (int k = 0; k < 16; ++k)
        if (mask >> (16 + k) & 1)
            fprintf(o, load ? " xmm[%d] = rc_cv_get(&c->v[%d]);" : " rc_cv_put(&c->v[%d], xmm[%d]);", k, k);
    for (int k = 0; k < 16 && !load; ++k)
        if (cond >> (16 + k) & 1) fprintf(o, " if (dv >> %d & 1) rc_cv_put(&c->v[%d], xmm[%d]);", k, k, k);
    if (load) fprintf(o, abi ? " dv = 0;" : " ef = c->flags; dv = 0;");
    else if (ef_stored && !abi) fprintf(o, " c->flags = ef;");
}

/* A register index as generated code writes it (r[3], r[RSP]) at s, or -1. */
static int sync_index(const char *s, size_t n, size_t *end) {
    static const char *const names[16] = {"RAX", "RCX", "RDX", "RBX", "RSP", "RBP", "RSI", "RDI",
                                          "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15"};
    size_t j = 0;
    while (j < n && ident_char(s[j])) ++j;
    if (!j || j >= n || s[j] != ']') return -1;
    *end = j + 1;
    if (s[0] >= '0' && s[0] <= '9') {
        const long v = strtol(s, NULL, 10);
        return v >= 0 && v < 16 ? (int)v : -1;
    }
    for (int k = 0; k < 16; ++k)
        if (strlen(names[k]) == j && !memcmp(s, names[k], j)) return k;
    return -1;
}

/* The vector register an instruction's text overwrites whole, or -1: its only mention is
 * rc_vset(&xmm[k], ..., bytes, vex) with vex set or all 32 bytes, in straight code. */
static int sync_vector_kill(const char *s, size_t n) {
    if (memmem(s, n, "if", 2) || memchr(s, '?', n) || memchr(s, '@', n) || memmem(s, n, "goto", 4)) return -1;
    const char *p = memmem(s, n, "rc_vset(&xmm[", 13);
    if (!p) return -1;
    char *end;
    const long k = strtol(p + 13, &end, 10);
    if (end == p + 13 || end >= s + n || *end != ']' || k < 0 || k > 15) return -1;
    const char *q = p + 7; /* the call's parentheses */
    for (int depth = 0; q < s + n; ++q) {
        if (*q == '(') ++depth;
        else if (*q == ')' && --depth == 0) break;
    }
    if (q >= s + n) return -1;
    const char *vex = q;
    while (vex > p && *vex != ',') --vex;
    const char *bytes = vex - 1;
    while (bytes > p && *bytes != ',') --bytes;
    if (bytes <= p || !(strtol(vex + 1, NULL, 10) == 1 || strtol(bytes + 1, NULL, 10) == 32)) return -1;
    for (const char *x = s; (x = memmem(x, (size_t)(s + n - x), "xmm", 3)); x += 3) {
        if (x > s && ident_char(x[-1])) continue;
        if (x + 3 >= s + n || x[3] != '[') return -1; /* all of them */
        if (x != p + 9 && strtol(x + 4, NULL, 10) == k && x[4] >= '0' && x[4] <= '9') return -1;
    }
    return (int)k;
}

/* Goes through the generated text of one instruction. Without `o`, notes its events in order: a
 * register's mention is a use, and a write too where an assignment follows it (after any .member
 * or [index]), its address is taken or a member array of it is (xmm[n].b, as memcpy's
 * destination); a bare r or xmm is all of them. `kill` (or -1): the register the instruction
 * overwrites whole, if this is its only mention. With `o`, writes the text there with the
 * markers filled in from marker_masks. */
static void sync_scan(const char *s, size_t n, int kill, FILE *o) {
    size_t i = 0, run = 0;
    const size_t first = event_count;
    int kill_mentions = 0;
    const int vector_kill = o ? -1 : sync_vector_kill(s, n);
    while (i < n) {
        if (s[i] == '/' && i + 1 < n && s[i + 1] == '*') { /* comments name instructions and imports */
            i += 2;
            while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) ++i;
            i += 2;
            continue;
        }
        if (s[i] == '@' && i + 3 < n && (s[i + 1] == 'Y' || s[i + 1] == 'Z')) { /* PUSH_SLOT, POP_SLOT */
            const int k = (s[i + 2] - '0') * 10 + (s[i + 3] - '0'), elided = elide_mask >> k & 1;
            if (o) {
                fwrite(s + run, 1, i - run, o);
                if (s[i + 1] == 'Y') {
                    if (!elided) fprintf(o, " rc_sts(r[RSP], 8, r[%d]);", k);
                } else if (elided) fprintf(o, " r[%d] = c->r[%d];", k, k);
                else fprintf(o, " r[%d] = rc_lds(r[RSP], 8);", k);
            } else if (s[i + 1] == 'Y') event(EV_USE, 1u << k, 'Y'); /* none where elided (generate) */
            else {
                event(EV_KILL, 1u << k, 'Z');
                event(EV_WRITE, 1u << k, 0);
            }
            i += 4;
            run = i;
            continue;
        }
        if (s[i] == '@' && i + 1 < n && s[i + 1] == 'X') { /* CALLEE_SAVED_RESTORE: the kept values */
            if (o) {
                fwrite(s + run, 1, i - run, o);
                for (int k = 0; k < 16; ++k)
                    if (restore_mask >> k & 1) fprintf(o, " c->r[%d] = sv%d;", k, k);
                restore_mask = 0;
            }
            i += 2;
            run = i;
            continue;
        }
        if (s[i] == '@' && i + 1 < n) {
            const char m = s[i + 1];
            const int kind = strchr("SEFCTJKPQUVW", m) ? EV_SPILL
                             : m == 'R' || m == 'A' || m == 'B' ? EV_RELOAD
                             : m == 'r'                         ? EV_RELOAD_IF
                                                                : -1;
            if (kind < 0 || !m) {
                fprintf(stderr, "recompile: unknown sync marker @%c\n", s[i + 1]);
                exit(1);
            }
            if (o && m == 'U') { /* the stores, then RAX and RDX: the locals where they are dirty */
                const uint32_t k = marker_masks[marker_next];
                fwrite(s + run, 1, i - run, o);
                sync_copy(o, k & ~ABI_REGISTER_RETURNS, marker_conds[marker_next], 0, 1);
                fprintf(o, " return (RcRet){%s, %s};", k & 1 ? "r[RAX]" : "c->r[RAX]", k & 4 ? "r[RDX]" : "c->r[RDX]");
                ++marker_next;
            } else if (o) {
                fwrite(s + run, 1, i - run, o);
                if (m == 'W' || m == 'K' || m == 'Q') { /* the GuestCpu's values, for CALLEE_SAVED_RESTORE */
                    restore_mask = marker_masks[marker_next] & ABI_CALLEE_SAVED;
                    for (int k = 0; k < 16; ++k)
                        if (restore_mask >> k & 1) fprintf(o, " const uint64_t sv%d = c->r[%d];", k, k);
                } else if (m == 'S' || m == 'E') { /* to code that may pop the elided slots: the entry's values */
                    for (int k = 0; k < 16; ++k)
                        if (elide_mask >> k & 1) fprintf(o, " rc_sts(entry_rsp - 0x%xull, 8, c->r[%d]);", elide_depth[k], k);
                }
                sync_copy(o, marker_masks[marker_next], marker_conds[marker_next], kind != EV_SPILL,
                          m != 'S' && m != 'E' && m != 'F' && m != 'R' && m != 'r');
                if (kind == EV_RELOAD)
                    for (int k = 0; k < 16; ++k)
                        if (marker_kills[marker_next] >> (16 + k) & 1)
                            fprintf(o, " xmm[%d].lo = xmm[%d].hi = (RcU4){0};", k, k);
                ++marker_next;
            } else {
                /* The registers a marker may copy (all but the ABI's). */
                const uint32_t filter = m == 'C'               ? ABI_ARGUMENTS
                                        : m == 'J' || m == 'K' ? ABI_ARGUMENTS | ABI_CALLEE_SAVED
                                        : m == 'T' ? ABI_RETURNS | ABI_CALLEE_SAVED | 1u << RSP
                                        : m == 'A' ? ABI_RETURNS
                                        : m == 'P' ? ABI_ARGUMENTS & ~ABI_REGISTER_ARGUMENTS
                                        : m == 'Q' || m == 'V' ? (ABI_ARGUMENTS | ABI_CALLEE_SAVED) & ~ABI_REGISTER_ARGUMENTS
                                        : m == 'U' ? ABI_RETURNS | ABI_CALLEE_SAVED
                                        : m == 'B' ? ABI_RETURNS & ~ABI_REGISTER_RETURNS
                                        : m == 'W' ? ABI_CALLEE_SAVED
                                                   : ~0u;
                event(kind, filter, (uint64_t)(unsigned char)m); /* the marker, for the returns' */
            }
            i += 2;
            run = i;
            continue;
        }
        if (!ident_char(s[i]) || (i && ident_char(s[i - 1]))) { ++i; continue; }
        size_t j = i;
        while (j < n && ident_char(s[j])) ++j;
        const size_t len = j - i;
        const int vec = len == 3 && !memcmp(s + i, "xmm", 3);
        if (o) {
        } else if (len == 4 && !memcmp(s + i, "goto", 4)) {
            size_t k = j;
            while (k < n && s[k] == ' ') ++k;
            if (k + 10 <= n && !memcmp(s + k, "L_dispatch", 10)) event(EV_DISPATCH, 0, 0);
            else if (k + 2 < n && !memcmp(s + k, "L_", 2)) event(EV_GOTO, 0, strtoull(s + k + 2, NULL, 16));
            else {
                fprintf(stderr, "recompile: a goto the register sync does not know: %.40s\n", s + i);
                exit(1);
            }
        } else if (len == 7 && !memcmp(s + i, "RC_VOUT", 7)) { /* trace points */
            event(EV_USE, 0xffff0000u, 0);
        } else if ((vec || (len == 1 && s[i] == 'r')) && !(i && (s[i - 1] == '.' || s[i - 1] == '>'))) {
            if (j < n && s[j] == '[') {
                size_t end;
                const int k = sync_index(s + j + 1, n - j - 1, &end);
                if (k < 0) {
                    fprintf(stderr, "recompile: a register the register sync does not know: %.40s\n", s + i);
                    exit(1);
                }
                const uint32_t bit = 1u << (k + (vec ? 16 : 0));
                int write = i && s[i - 1] == '&', assign = 0;
                if (i >= 2 && ((s[i - 1] == '+' && s[i - 2] == '+') || (s[i - 1] == '-' && s[i - 2] == '-'))) write = 1;
                size_t p = j + 1 + end;
                int array = 0;
                for (;;) { /* .member, [index] */
                    if (p < n && s[p] == '.') {
                        ++p;
                        while (p < n && ident_char(s[p])) ++p;
                        array = 1;
                    } else if (p < n && s[p] == '[') {
                        int depth = 0;
                        do depth += s[p] == '[' ? 1 : s[p] == ']' ? -1 : 0;
                        while (++p < n && depth);
                        array = 0;
                    } else {
                        break;
                    }
                }
                if (array) write = 1; /* xmm[n].b as a pointer: memcpy's destination */
                while (p < n && s[p] == ' ') ++p;
                if (p + 1 < n) {
                    const char a = s[p], b = s[p + 1];
                    if (a == '=' && b != '=') { write = 1; assign = p == j + 1 + end + 1; }
                    if (b == '=' && strchr("+-*/%&|^", a)) write = 1;
                    if ((a == '+' && b == '+') || (a == '-' && b == '-')) write = 1;
                    if (p + 2 < n && ((a == '<' && b == '<') || (a == '>' && b == '>')) && s[p + 2] == '=') write = 1;
                }
                if (!vec && k == kill) {
                    ++kill_mentions;
                    if (!assign || !write) kill = -1;
                }
                if (vec && write && !(i >= 9 && !memcmp(s + i - 9, "rc_vset(&", 9))) { /* dv: see sync_copy */
                    fprintf(stderr, "recompile: a vector register written other than by rc_vset: %.60s\n", s + i);
                    exit(1);
                }
                event((!vec && k == kill) || (vec && k == vector_kill) ? EV_KILL : EV_USE, bit, 0);
                if (write) event(EV_WRITE, bit, 0);
            } else {
                const uint32_t all = vec ? 0xffff0000u : 0xffffu;
                event(EV_USE, all, 0);
                event(EV_WRITE, all, 0);
            }
        }
        i = j;
    }
    if (o) {
        fwrite(s + run, 1, n - run, o);
        return;
    }
    /* A kill only as the instruction's one mention of it, in straight code (sync_vector_kill
     * checked a vector register's). */
    for (size_t e = first; e < event_count; ++e)
        if (events[e].kind == EV_KILL && events[e].mask < 0x10000u && events[e].target != 'Z' &&
            (kill_mentions != 1 || kill < 0 || memmem(s, n, "if", 2) || memchr(s, '?', n) || memchr(s, '@', n) ||
             memmem(s, n, "goto", 4)))
            events[e].kind = EV_USE;
}

/* The register an instruction overwrites whole (its 32 or 64 bits), or -1. */
static int sync_kill(const GuestInsn *in) {
    const int m = in->mnemonic;
    const GuestOp *d = &in->op[0];
    if (!in->count || d->type != OP_REG || d->kind != RK_GPR || d->reg > 15 || (d->size != 4 && d->size != 8)) return -1;
    if (m == M(MOV) || m == M(MOVZX) || m == M(MOVSX) || m == M(MOVSXD) || m == M(LEA)) return d->reg;
    return m == M(POP) && d->size == 8 ? d->reg : -1;
}

/* Where instruction i goes on to: 0 the next one in address order (or past the function's end), 1
 * another decoded instruction (a label), 2 an address not decoded (the dispatcher), -1 nowhere. */
static int falls_to(const Insn *list, size_t count, size_t i, Range f) {
    if (!falls_through(list[i].off, &list[i].in)) return -1;
    const uint64_t next = list[i].off + list[i].in.length;
    if (i + 1 < count ? list[i + 1].off == next : next == f.end) return 0;
    return bsearch(&next, list, count, sizeof(*list), by_offset) ? 1 : 2;
}

/* The name of the version being written after f_<offset> ("" for the unpatched one). */
static char version_suffix[24];

/* --hooks FILE: code that the program running the game rewrites (GR2port's hooks: a jump to its
 * stub, a return), one range a line, "ADDRESS SIZE" (GoldHEN addresses, the image at 0x400000).
 * A function holding one gets a hook point at its start: where the bytes there are not the ones
 * compiled, RcApi.hook runs them and the code they lead to. The range counts as live (hashed as
 * zeros, in RcFunction.live), so that the program knows the function reads it as it runs. */
static Range *hook_sites; /* image offsets, by start */
static size_t hook_site_count;
static uint64_t hook_points;
static int range_order(const void *a, const void *b) {
    const Range *x = a, *y = b;
    return x->start < y->start ? -1 : x->start > y->start;
}
static void load_hooks(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); exit(1); }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        const char *p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == '\n' || !*p) continue;
        char *end;
        const uint64_t address = strtoull(p, &end, 0), size = strtoull(end, NULL, 0);
        if (address < 0x400000 || !size || size > 64) { fprintf(stderr, "%s: not ADDRESS SIZE: %s", path, line); exit(1); }
        hook_sites = realloc(hook_sites, (hook_site_count + 1) * sizeof(*hook_sites));
        hook_sites[hook_site_count++] = (Range){address - 0x400000, address - 0x400000 + size};
    }
    fclose(f);
    qsort(hook_sites, hook_site_count, sizeof(*hook_sites), range_order);
    size_t unique = 0; /* one range a start: the longest */
    for (size_t i = 0; i < hook_site_count; ++i) {
        if (unique && hook_sites[unique - 1].start == hook_sites[i].start) {
            if (hook_sites[i].end > hook_sites[unique - 1].end) hook_sites[unique - 1].end = hook_sites[i].end;
        } else {
            hook_sites[unique++] = hook_sites[i];
        }
    }
    hook_site_count = unique;
}
/* The first hook site starting at or after `off`. */
static size_t hook_site_from(uint64_t off) {
    size_t lo = 0, hi = hook_site_count;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (hook_sites[mid].start < off) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}
/* Whether the function at `t` holds a hook site (HOOKED_CALL_SPILL). */
static int holds_hook_site(uint64_t t) {
    const Function *fn = bsearch(&t, functions, function_count, sizeof(*functions), by_offset);
    if (!fn) return 0;
    const size_t k = hook_site_from(t);
    return k < hook_site_count && hook_sites[k].start < t + fn->size;
}
/* A hook point of the function being written: its site [at, next) (`label`: next is one), and the
 * targets in the function of the branches at the site, where the hook's code may go on too (the
 * frame windows' AttachCount at 0xbde981 goes on at its jg's target). */
typedef struct { uint64_t at, next; int label; uint64_t exits[4]; int exit_count; } HookPoint;

/* The hook point's code, before the instruction at its site. At the function's entry (`entry`), a
 * ret written over its first byte (GR2port's returning functions) returns as the function's own
 * ret does, the stack as at the entry. */
static void emit_hook_point(const HookPoint *h, int entry) {
    const uint64_t length = h->next - h->at, k = length < 8 ? length : 8;
    uint64_t expected = 0;
    memcpy(&expected, (const void *)(uintptr_t)(base + h->at), k);
    const uint64_t m = k == 8 ? ~UINT64_C(0) : (UINT64_C(1) << (8 * k)) - 1;
    emit("/* hook point */ { uint64_t hw; memcpy(&hw, (const void *)(uintptr_t)(B + 0x%" PRIx64 "ull), 8); if ((hw & 0x%" PRIx64
         "ull) != 0x%" PRIx64 "ull) {", h->at, m, expected);
    if (entry && register_function) emit(" if ((hw & 0xff) == 0xc3 && r[RSP] == entry_rsp) { " REGISTER_RET " }");
    else if (entry)
        emit(" if ((hw & 0xff) == 0xc3 && r[RSP] == entry_rsp) { const uint64_t t = rc_lds(r[RSP], 8); r[RSP] += 8; %s c->rip = t; "
             "return; }",
             abi_function ? RET_SPILL : SPILL);
    emit(" " SPILL " " FLAGS_OUT " c->rip = B + 0x%" PRIx64 "ull; rc->hook(c, B + 0x%" PRIx64 "ull); " RELOAD_IF " fk = FK_CPU;", h->at,
         h->next);
    if (h->label) emit(" if (c->rip == B + 0x%" PRIx64 "ull) goto L_%" PRIx64 ";", h->next, h->next);
    for (int e = 0; e < h->exit_count; ++e)
        emit(" if (c->rip == B + 0x%" PRIx64 "ull) goto L_%" PRIx64 ";", h->exits[e], h->exits[e]);
    emit(" if (c->r[RSP] == entry_rsp + 8 && c->rip == rc_lds(entry_rsp, 8)) %s dn = c->rip; %s } }\n", exit_return,
         dispatch());
    ++hook_points;
}

/* Whether the status flags that list[i] writes are read by nothing: on the way on from it (up to 64
 * instructions, through direct jumps inside the function), all of CF PF ZF SF OF are written again
 * before one of them is read, or a call or a return comes first (their translations pass no flags
 * on). A conditional branch, a hook point or an instruction not listed ends the way: read. */
static int flags_dead_after(const Insn *list, size_t count, size_t i, Range f) {
    static const unsigned cc_reads[16] = {F_OF, F_OF, F_CF, F_CF, F_ZF, F_ZF, F_CF | F_ZF, F_CF | F_ZF,
                                          F_SF, F_SF, F_PF, F_PF, F_SF | F_OF, F_SF | F_OF,
                                          F_ZF | F_SF | F_OF, F_ZF | F_SF | F_OF};
    const unsigned all = F_CF | F_PF | F_ZF | F_SF | F_OF;
    unsigned w = all; /* the ones not written again yet */
    uint64_t at = list[i].off + list[i].in.length;
    for (int steps = 0; steps < 64; ++steps) {
        const Insn *x = bsearch(&at, list, count, sizeof(*list), by_offset);
        if (!x) return 0;
        const size_t k = hook_site_from(x->off);
        if (k < hook_site_count && hook_sites[k].start == x->off) return 0;
        const GuestInsn *in = &x->in;
        const int m = in->mnemonic, cc = cc_of(m);
        at = x->off + in->length;
        unsigned reads = 0, writes = 0;
        int vector = 0;
        for (int o = 0; o < in->count; ++o) vector |= in->op[o].type == OP_REG && in->op[o].kind == RK_VEC;
        if (cc >= 0 && cc < 16) return 0; /* a branch: both ways */
        if (cc >= 16) {
            reads = cc_reads[cc & 15]; /* setcc, cmovcc */
        } else {
            switch (m) {
            case M(ADD): case M(SUB): case M(CMP): case M(AND): case M(OR): case M(XOR): case M(TEST): case M(NEG):
            case M(COMISS): case M(COMISD): case M(UCOMISS): case M(UCOMISD): case M(VCOMISS): case M(VCOMISD):
            case M(VUCOMISS): case M(VUCOMISD):
                writes = all;
                break;
            case M(ADC): case M(SBB):
                reads = F_CF, writes = all;
                break;
            case M(INC): case M(DEC):
                writes = all & ~F_CF;
                break;
            case M(SHL): case M(SHR): case M(SAR):
                if (in->count < 2 || in->op[1].type == OP_IMM) { /* by cl: maybe none */
                    const unsigned n = in->count < 2 ? 1 : (unsigned)in->op[1].disp;
                    writes = n & (in->op[0].size == 8 ? 63 : 31) ? all : 0;
                }
                break;
            case M(IMUL):
                if (in->count < 2) return 0;
                writes = all;
                break;
            case M(ANDN): case M(BEXTR): case M(BLSR): case M(BLSI): case M(BLSMSK): case M(POPCNT): case M(TZCNT):
            case M(LZCNT): case M(BSF): case M(BSR): /* the ones they set, the rest undefined after them */
                writes = all;
                break;
            case M(MOV): case M(MOVZX): case M(MOVSX): case M(MOVSXD): case M(LEA): case M(PUSH): case M(POP):
            case M(NOP): case M(XCHG): case M(BSWAP): case M(NOT): case M(CDQE): case M(CDQ): case M(CQO):
            case M(CWDE): case M(CBW): case M(CWD):
                break;
            case M(CALL): case M(RET):
                return 1;
            case M(JMP):
                if (in->op[0].type != OP_IMM || !inside(f, at + (uint64_t)in->op[0].disp)) return 0;
                at += (uint64_t)in->op[0].disp;
                break;
            case M(PTEST): case M(VPTEST): case M(VTESTPS): case M(VTESTPD): case M(PCMPESTRI): case M(PCMPESTRM):
            case M(PCMPISTRI): case M(PCMPISTRM): case M(VPCMPESTRI): case M(VPCMPESTRM): case M(VPCMPISTRI):
            case M(VPCMPISTRM):
                return 0;
            default:
                if (!vector) return 0; /* SSE and AVX write no flags but the ones above */
                break;
            }
        }
        if (reads & w) return 0;
        w &= ~writes;
        if (!w) return 1;
    }
    return 0;
}

/* Whether the pop (or leave) at list[i] is on the way to a return or a jump: the instructions after
 * it in address order pop, move RSP (add, lea, mov from RBP, leave) and then return or jump. */
static int returns_after(const Insn *list, size_t count, size_t i) {
    for (size_t j = i + 1; j < count && list[j].off == list[j - 1].off + list[j - 1].in.length; ++j) {
        const GuestInsn *in = &list[j].in;
        const int m = in->mnemonic;
        if (m == M(RET) || m == M(JMP)) return 1;
        const int to_rsp = in->count && in->op[0].type == OP_REG && in->op[0].kind == RK_GPR && in->op[0].reg == RSP;
        if (!(m == M(POP) && gpr(&in->op[0])) && m != M(LEAVE) && !((m == M(ADD) || m == M(LEA) || m == M(MOV)) && to_rsp))
            return 0;
    }
    return 0;
}

static void generate(Function fn, uint64_t *stats) {
    abi_function = abi_mode && !writing_patch_code && !trace_points && !snippet_mode;
    size_t count;
    Insn *list = decode(fn, &count);
    /* A function that loads RSP from memory (longjmp, setcontext) returns on another frame's stack:
     * it keeps SPILL and RELOAD, and its return leaves every register in the GuestCpu, where the
     * runtime finds the call of setjmp a longjmp goes back to. */
    for (size_t i = 0; i < count && abi_function; ++i)
        if (list[i].in.mnemonic == M(MOV) && list[i].in.op[0].type == OP_REG && list[i].in.op[0].kind == RK_GPR &&
            list[i].in.op[0].reg == RSP && list[i].in.op[1].type == OP_MEM)
            abi_function = 0;
    const Range f = {fn.offset, fn.offset + fn.size};
    /* Its hook points: the hook sites inside it (one past its end is not its). */
    HookPoint *points = NULL;
    size_t point_count = 0;
    for (size_t k = hook_site_from(f.start); k < hook_site_count && hook_sites[k].start < f.end; ++k) {
        const Range s = hook_sites[k];
        if (s.end > f.end) {
            if (!writing_version)
                fprintf(stderr, "recompile: the hook site %#" PRIx64 "-%#" PRIx64 " crosses the end of the function at %#" PRIx64
                        ": not a hook point\n", s.start + 0x400000, s.end + 0x400000, f.start);
            continue;
        }
        const int at = bsearch(&s.start, list, count, sizeof(*list), by_offset) != NULL,
                  label = bsearch(&s.end, list, count, sizeof(*list), by_offset) != NULL;
        if (!at || (!label && s.end != f.end)) {
            if (writing_version) { /* the patches moved its instructions */
                version_broken = 1;
                continue;
            }
            fprintf(stderr, "recompile: the hook site %#" PRIx64 "-%#" PRIx64 " of the function at %#" PRIx64
                    " is not whole instructions\n", s.start + 0x400000, s.end + 0x400000, f.start);
            exit(1);
        }
        HookPoint point = {s.start, s.end, label, {0}, 0};
        for (size_t i = 0; i < count && point.exit_count < 4; ++i) {
            const GuestInsn *in = &list[i].in;
            const int cc = cc_of(in->mnemonic);
            if (list[i].off < s.start || list[i].off >= s.end || in->op[0].type != OP_IMM ||
                !((cc >= 0 && cc < 16) || in->mnemonic == M(JMP)))
                continue;
            const uint64_t t = list[i].off + (uint64_t)in->op[0].disp;
            if (inside(f, t) && t != s.end) point.exits[point.exit_count++] = t;
        }
        points = realloc(points, (point_count + 1) * sizeof(*points));
        points[point_count++] = point;
    }
    /* Labels only where a direct jump goes (and the entry), or everywhere in a function with a
     * computed jump: every other address an indirect jump or an interpreted branch reaches goes
     * back to the translator (L_dispatch). */
    uint64_t *targets = malloc((2 * count + 1 + table_target_count + 2 * point_count) * sizeof(*targets));
    size_t target_count = 0;
    targets[target_count++] = f.start;
    for (size_t k = 0; k < point_count; ++k) {
        targets[target_count++] = points[k].at;
        if (points[k].label) targets[target_count++] = points[k].next;
    }
    /* Up to 1000 instructions: a label everywhere in a larger function made clang's -O2 run for
     * over ten minutes on one file. */
    if (computed_jump && count <= 1000) {
        for (size_t i = 0; i < count; ++i) targets[target_count++] = list[i].off;
    }
    /* A target of the computed jump without a label goes back to the translator: listed. */
    if (computed_jump && count > 1000 && not_compiled_report)
        fprintf(not_compiled_report, "%#" PRIx64 "	computed jmp	function %#" PRIx64 ", %zu instructions, labels at jump targets only\n",
                computed_jump_at, fn.offset, count);
    for (size_t i = 0; i < table_target_count; ++i) targets[target_count++] = table_targets[i]; /* jump tables */
    /* Instructions decoded from different starting points can overlap (data decoded as code after
     * a jump table, a branch into the middle of an instruction): the instruction after one in
     * address order is then not where it continues, so it gets an explicit jump (falls_to). */
    for (size_t i = 0; i < count; ++i)
        if (falls_to(list, count, i, f) == 1) targets[target_count++] = list[i].off + list[i].in.length;
    for (size_t i = 0; i < count; ++i) {
        const GuestInsn *in = &list[i].in;
        const int m = in->mnemonic, cc = cc_of(m);
        if (((cc >= 0 && cc < 16) || m == M(JMP) || m == M(JRCXZ) || m == M(JECXZ) || m == M(LOOP) || m == M(LOOPE) ||
             m == M(LOOPNE)) && in->op[0].type == OP_IMM) {
            const uint64_t t = list[i].off + (uint64_t)in->op[0].disp;
            if (inside(f, t)) targets[target_count++] = t;
        }
    }
    qsort(targets, target_count, sizeof(*targets), by_offset);
    emit("\n/* 0x%" PRIx64 ", %" PRIu64 " bytes, %zu instructions%s%s */\n", fn.offset, fn.size, count,
         annotation(fn.offset) ? ": " : "", annotation(fn.offset) ? annotation(fn.offset) : "");
    /* Vector registers in locals: the ones the instructions name (non-VEX blendv reads xmm0). */
    vused = 0;
    for (size_t i = 0; i < count; ++i) {
        const GuestInsn *in = &list[i].in;
        for (int k = 0; k < 5; ++k)
            if (in->op[k].type == OP_REG && in->op[k].kind == RK_VEC && in->op[k].reg < 16) vused |= 1u << in->op[k].reg;
        const int m = in->mnemonic;
        if (!in->vex && (m == M(BLENDVPS) || m == M(BLENDVPD) || m == M(PBLENDVB))) vused |= 1;
    }
    if (trace_points) { /* the trace reads the locals: all of them are kept */
        emit("#define RC_VOUT");
        for (int k = 0; k < 16; ++k) if (vused >> k & 1) emit(" rc_cv_put(&c->v[%d], xmm[%d]);", k, k);
        emit("\n");
    }
    /* With calls of setjmp: the function's code in f_<offset>_in, its RcResume in the frame of
     * f_<offset>, which tells the runtime when it returns. */
    int setjmps = 0;
    for (size_t i = 0; i < count && !snippet_mode; ++i)
        setjmps += list[i].in.mnemonic == M(CALL) && list[i].in.op[0].type == OP_IMM &&
                   is_setjmp(list[i].off + (uint64_t)list[i].in.op[0].disp);
    setjmp_calls = 0;
    /* An ABI function is a_<offset> (the register convention), f_<offset> its entry with the
     * registers in the GuestCpu; not with setjmp's RcResume or a return that pops more than the
     * return address. */
    int pops = 0;
    for (size_t i = 0; i < count; ++i)
        pops |= list[i].in.mnemonic == M(RET) && list[i].in.count && list[i].in.op[0].type == OP_IMM && list[i].in.op[0].disp;
    register_function = abi_function && !setjmps && !pops;
    exit_return = register_function ? "return rc_back(c);" : "return;";
    /* The callee-saved registers whose prologue pushes and pops may be elided: pushed from the
     * entry on (pushes and mov rbp, rsp, no label) while they hold the entry's values, nowhere
     * else, and popped only on the way to a return or a jump; not in a function with hook points,
     * whose hook code reads the frame. */
    elide_candidates = 0;
    if (register_function && !point_count && count && list[0].off == f.start) {
        uint32_t depth = 0, written = 0, pushed = 0;
        for (size_t i = 0; i < count; ++i) {
            const GuestInsn *in = &list[i].in;
            if (i && (list[i].off != list[i - 1].off + list[i - 1].in.length ||
                      bsearch(&list[i].off, targets, target_count, sizeof(*targets), by_offset)))
                break;
            if (in->mnemonic == M(PUSH) && gpr(&in->op[0]) && in->op[0].size == 8) {
                const int k = in->op[0].reg;
                depth += 8;
                if ((ABI_CALLEE_SAVED >> k & 1) && !(written >> k & 1) && !(pushed >> k & 1)) {
                    elide_candidates |= 1u << k;
                    elide_push[k] = list[i].off;
                    elide_depth[k] = depth;
                }
                pushed |= 1u << k;
            } else if (in->mnemonic == M(MOV) && gpr(&in->op[0]) && in->op[0].reg == RBP && in->op[0].size == 8 &&
                       gpr(&in->op[1]) && in->op[1].reg == RSP) {
                written |= 1u << RBP;
            } else {
                break;
            }
        }
        for (size_t i = 0; i < count && elide_candidates; ++i) {
            const GuestInsn *in = &list[i].in;
            const int m = in->mnemonic;
            const int k = (m == M(PUSH) || m == M(POP)) && gpr(&in->op[0]) ? in->op[0].reg : m == M(LEAVE) ? RBP : -1;
            if (k < 0 || !(elide_candidates >> k & 1)) continue;
            if (m == M(PUSH) ? list[i].off != elide_push[k] : !returns_after(list, count, i))
                elide_candidates &= ~(1u << k);
        }
    }
    if (snippet_mode) emit("static void s_%" PRIx64 "(GuestCpu *c) {\n", snippet_line);
    else if (setjmps) emit("static void f_%" PRIx64 "%s_in(GuestCpu *c, RcResume *rs) {\n", fn.offset, version_suffix);
    else if (register_function)
        emit("__attribute__((visibility(\"hidden\"))) RcRet a_%" PRIx64 "%s(GuestCpu *c, " REGISTER_PARAMETERS ") {\n",
             fn.offset, version_suffix);
    else emit("__attribute__((visibility(\"hidden\"))) void f_%" PRIx64 "%s(GuestCpu *c) {\n", fn.offset, version_suffix);
    /* An address only known as the code runs goes out of the function where it comes up, unless a
     * computed jump can go to any instruction (all of them labels, L_dispatch). A hook point's
     * code going on anywhere but past it or back to the caller leaves to the translator. */
    dispatch_cases = (computed_jump && count <= 1000) || trace_points || snippet_mode;
    if (!dispatch_cases && hook_code) {
        snprintf(dispatch_exit, sizeof(dispatch_exit), "{ " EXIT_SPILL " " FLAGS_OUT " c->rip = dn; return; }");
    } else if (!dispatch_cases) {
        int n = snprintf(dispatch_exit, sizeof(dispatch_exit),
                         "{ " EXIT_SPILL " " FLAGS_OUT " c->rip = dn; if ((dn - B >= 0x%" PRIx64 "ull && dn - B < 0x%" PRIx64 "ull)",
                         f.start, f.end);
        for (size_t k = 0; k < entered_count; ++k)
            n += snprintf(dispatch_exit + n, sizeof(dispatch_exit) - (size_t)n,
                          " || (dn - B >= 0x%" PRIx64 "ull && dn - B < 0x%" PRIx64 "ull)", entered[k].start, entered[k].end);
        snprintf(dispatch_exit + n, sizeof(dispatch_exit) - (size_t)n,
                 " || r[RSP] != entry_rsp) rc->bail(c, entry_rsp); else rc->tail(c); %s }", exit_return);
    }
    /* The instructions, each from starts[i]; then the copies filled in (register sync). flow[i]:
     * the code goes on into the next instruction's (not after a goto or a return). */
    const long body = ftell(out);
    long *starts = malloc((count + 1) * sizeof(*starts));
    unsigned char *flow = malloc(count + 1);
    size_t next_point = 0;
    for (size_t i = 0; i < count; ++i) {
        const Insn *x = &list[i];
        starts[i] = ftell(out) - body;
        /* A cave below the function: its code comes first in address order. */
        if (!i && x->off != f.start) emit("goto L_%" PRIx64 "; ", f.start);
        if (bsearch(&x->off, targets, target_count, sizeof(*targets), by_offset)) emit("L_%" PRIx64 ": ", x->off);
        if (trace_points) emit("RC_VOUT rc->trace(0x%" PRIx64 ", r, c); ", x->off);
        while (next_point < point_count && points[next_point].at < x->off) ++next_point;
        if (next_point < point_count && points[next_point].at == x->off) emit_hook_point(&points[next_point], x->off == fn.offset);
        for (size_t k = 0; k < jump_table_count; ++k) /* the index of a jump table's load, for its jmp */
            if (jump_tables[k].load == x->off) emit("jt_%" PRIx64 " = r[%d]; ", x->off, jump_tables[k].index);
        ++stats[2];
        const long mark = ftell(out);
        const int fm = x->in.mnemonic;
        flags_dead = !snippet_mode && !trace_points &&
                     (fm == M(SHL) || fm == M(SHR) || fm == M(SAR) || fm == M(SHLD) || fm == M(SHRD) || fm == M(IMUL) ||
                      fm == M(ANDN) || fm == M(BEXTR) || fm == M(BLSR) || fm == M(BLSI) || fm == M(BLSMSK) ||
                      fm == M(POPCNT) || fm == M(TZCNT) || fm == M(LZCNT) || fm == M(BSF) || fm == M(BSR)) &&
                     flags_dead_after(list, count, i, f);
        hook_read = 0;
        const int translated = translate(f, x->off, &x->in, stats);
        flags_dead = 0;
        if (translated && hook_code) { /* each field that is not in the hash is one the code reads as it runs */
            LiveField imm, disp, rel;
            hook_fields(x->off, &imm, &disp, &rel);
            if ((imm.size && !(hook_read & 1)) || (disp.size && !(hook_read & 2)) || (rel.size && !(hook_read & 4))) {
                fprintf(stderr, "recompile: hook code %#" PRIx64 ": a field of the instruction is not read as the code runs\n",
                        x->off);
                exit(1);
            }
        }
        if (translated) {
            ++stats[0];
        } else {
            fseek(out, mark, SEEK_SET); /* what a translation wrote before it gave up */
            if (snippet_mode) emit_interp(x->off, &x->in, 0);
            else emit_untranslated(x->off, &x->in);
            emit("\n");
        }
        const uint64_t next = x->off + x->in.length;
        const int to = falls_to(list, count, i, f);
        if (to == 1) emit("goto L_%" PRIx64 "; /* not the next one in address order */\n", next);
        else if (to == 2) emit("{ dn = B + 0x%" PRIx64 "ull; %s }\n", next, dispatch());
        flow[i] = to == 0 || (to < 0 && !(translated && (x->in.mnemonic == M(JMP) || x->in.mnemonic == M(RET))));
    }
    starts[count] = ftell(out) - body;
    char *text = malloc((size_t)starts[count] + 1);
    fseek(out, body, SEEK_SET);
    if (fread(text, 1, (size_t)starts[count], out) != (size_t)starts[count]) { perror("recompile: reading back"); exit(1); }
    fseek(out, body, SEEK_SET);
    text[starts[count]] = 0;
    ef_stored = writes_ef(text, (size_t)starts[count]);
    /* The events of each instruction, from evs[i]. */
    size_t *evs = malloc((count + 1) * sizeof(*evs));
    event_count = 0;
    for (size_t i = 0; i < count; ++i) {
        evs[i] = event_count;
        sync_scan(text + starts[i], (size_t)(starts[i + 1] - starts[i]), sync_kill(&list[i].in), NULL);
    }
    evs[count] = event_count;
    if (trace_points) event(EV_USE, 0xffffu, 0); /* the entry: the trace reads all of r */
    for (size_t e = 0; e < evs[count]; ++e) {
        if (events[e].kind != EV_GOTO) continue;
        const Insn *t = bsearch(&events[e].target, list, count, sizeof(*list), by_offset);
        if (!t) { fprintf(stderr, "recompile: goto L_%" PRIx64 " without its instruction\n", events[e].target); exit(1); }
        events[e].target = (uint64_t)(t - list); /* the instruction it goes to */
    }
    /* The elided pushes and pops: none where a RELOAD of every register (after a hook point, setjmp,
     * the interpreter) may leave other values of the callee-saved registers in the GuestCpu than the
     * entry's, or where other code jumps in; their pushes read nothing. */
    elide_mask = is_jumped_to(fn.offset) ? 0 : elide_candidates;
    for (size_t e = 0; e < evs[count] && elide_mask; ++e)
        if ((events[e].kind == EV_RELOAD && events[e].target == 'R') || events[e].kind == EV_RELOAD_IF) elide_mask = 0;
    for (size_t e = 0; e < evs[count]; ++e)
        if (events[e].kind == EV_USE && events[e].target == 'Y' && (events[e].mask & elide_mask)) events[e].mask = 0;
    unsigned char *is_case = malloc(count); /* L_dispatch's cases */
    for (size_t i = 0; i < count; ++i) is_case[i] = bsearch(&list[i].off, targets, target_count, sizeof(*targets), by_offset) != NULL;
    /* Dirty sets at each instruction's start (the entry clean but a_<offset>'s arguments), to a
     * fixed point; L_dispatch takes its set to every case and to the exit. The set at each SPILL
     * goes in mask[]. After an ABI call all are clean but the callee-saved ones (a call does not
     * store them, or CALLEE_SAVED_RESTORE puts the entry's values back), and RSP after a call of
     * a_<offset>: the callee gives back RSP and the callee-saved registers as the call left them,
     * and left any value in the caller-saved ones. */
    uint32_t *mask = calloc(evs[count] + 1, sizeof(*mask));
    uint32_t *dirty_in = calloc(count + 1, sizeof(*dirty_in)), dispatch = 0;
    dirty_in[0] = register_function ? ABI_REGISTER_ARGUMENTS : 0;
    for (int changed = 1; changed;) {
        changed = 0;
        for (size_t i = 0; i < count; ++i) {
            uint32_t dirty = dirty_in[i];
            for (size_t e = evs[i]; e < evs[i + 1]; ++e) {
                const SyncEvent *v = &events[e];
                if (v->kind == EV_WRITE) dirty |= v->mask;
                else if (v->kind == EV_SPILL) mask[e] = dirty & v->mask;
                else if (v->kind == EV_RELOAD) /* after a_<offset>: RSP only in the local */
                    dirty = v->mask == ~0u ? 0 : (dirty & ABI_CALLEE_SAVED) | (v->target == 'B' ? 1u << RSP : 0);
                else if (v->kind == EV_DISPATCH) dispatch |= dirty;
                else if (v->kind == EV_GOTO && (dirty_in[v->target] | dirty) != dirty_in[v->target]) {
                    dirty_in[v->target] |= dirty;
                    changed = 1;
                }
            }
            if (flow[i] && (dirty_in[i + 1] | dirty) != dirty_in[i + 1]) { dirty_in[i + 1] |= dirty; changed = 1; }
        }
        for (size_t i = 0; i < count; ++i)
            if (is_case[i] && (dirty_in[i] | dispatch) != dirty_in[i]) { dirty_in[i] |= dispatch; changed = 1; }
    }
    /* Must sets: the dirty registers written on every path to each instruction's start (the entry
     * none), to a fixed point down from all; L_dispatch's is what every jump to it has. A vector
     * register dirty at a SPILL but not on every path goes from mask[] (stored always) to cond[]
     * (stored where dv has it). */
    const uint32_t vec_bits = 0xffff0000u;
    uint32_t *cond = calloc(evs[count] + 1, sizeof(*cond));
    uint32_t *must_in = malloc((count + 1) * sizeof(*must_in)), must_dispatch = ~0u;
    for (size_t i = 0; i <= count; ++i) must_in[i] = i ? ~0u : 0;
    for (int changed = 1; changed;) {
        changed = 0;
        for (size_t i = 0; i < count; ++i) {
            uint32_t must = must_in[i];
            for (size_t e = evs[i]; e < evs[i + 1]; ++e) {
                const SyncEvent *v = &events[e];
                if (v->kind == EV_WRITE) must |= v->mask;
                else if (v->kind == EV_RELOAD) must = v->mask == ~0u ? 0 : must & ~(ABI_CALLER_SAVED | 1u << RSP);
                else if (v->kind == EV_DISPATCH && (must_dispatch & must) != must_dispatch) { must_dispatch &= must; changed = 1; }
                else if (v->kind == EV_GOTO && (must_in[v->target] & must) != must_in[v->target]) {
                    must_in[v->target] &= must;
                    changed = 1;
                }
            }
            if (flow[i] && (must_in[i + 1] & must) != must_in[i + 1]) { must_in[i + 1] &= must; changed = 1; }
        }
        for (size_t i = 0; i < count; ++i)
            if (is_case[i] && (must_in[i] & must_dispatch) != must_in[i]) { must_in[i] &= must_dispatch; changed = 1; }
    }
    for (size_t i = 0; i < count; ++i) {
        uint32_t must = must_in[i];
        for (size_t e = evs[i]; e < evs[i + 1]; ++e) {
            const SyncEvent *v = &events[e];
            if (v->kind == EV_WRITE) must |= v->mask;
            else if (v->kind == EV_RELOAD) must = v->mask == ~0u ? 0 : must & ~(ABI_CALLER_SAVED | 1u << RSP);
            else if (v->kind == EV_SPILL) { cond[e] = mask[e] & ~must & vec_bits; mask[e] &= ~cond[e]; }
        }
    }
    /* A return or tail call leaves a callee-saved register the GuestCpu still has at the entry's
     * value: code a compiler wrote pops back what its entry had. Spilled sets (forward, the entry
     * none): the callee-saved registers a marker stored on some path from the entry, all of them
     * past a SPILL (its hook or patch code may change them); not the ones a call stored that
     * CALLEE_SAVED_RESTORE puts back, nor an EXIT_SPILL's. Not where other code jumps to. */
    if (abi_function && !is_jumped_to(fn.offset)) {
        uint32_t *spilled_in = calloc(count + 1, sizeof(*spilled_in)), spilled_dispatch = 0;
        for (int pass = 0; pass < 2; ++pass)
            for (int changed = 1; changed;) {
                changed = 0;
                for (size_t i = 0; i < count; ++i) {
                    uint32_t spilled = spilled_in[i];
                    for (size_t e = evs[i]; e < evs[i + 1]; ++e) {
                        const SyncEvent *v = &events[e];
                        if (v->kind == EV_SPILL) {
                            if (pass && strchr("TJUV", (int)v->target)) mask[e] &= ~(ABI_CALLEE_SAVED & ~spilled);
                            spilled |= v->target == 'S'                   ? ABI_CALLEE_SAVED
                                       : strchr("EFKQW", (int)v->target) ? 0
                                                                        : mask[e] & ABI_CALLEE_SAVED;
                        } else if (v->kind == EV_RELOAD_IF) spilled |= ABI_CALLEE_SAVED;
                        else if (v->kind == EV_DISPATCH) spilled_dispatch |= spilled;
                        else if (v->kind == EV_GOTO && (spilled_in[v->target] | spilled) != spilled_in[v->target]) {
                            spilled_in[v->target] |= spilled;
                            changed = 1;
                        }
                    }
                    if (flow[i] && (spilled_in[i + 1] | spilled) != spilled_in[i + 1]) {
                        spilled_in[i + 1] |= spilled;
                        changed = 1;
                    }
                }
                for (size_t i = 0; i < count; ++i)
                    if (is_case[i] && (spilled_in[i] | spilled_dispatch) != spilled_in[i]) {
                        spilled_in[i] |= spilled_dispatch;
                        changed = 1;
                    }
                if (pass) break; /* the fixed point's sets, once */
            }
        free(spilled_in);
    }
    const uint32_t dirty_out = dispatch | dirty_in[count]; /* L_dispatch's default, past the end */
    const uint32_t out_cond = dirty_out & ~((snippet_mode ? ~0u : must_in[count]) & must_dispatch) & vec_bits;
    const uint32_t end_out = snippet_mode ? dirty_in[count] : dirty_out;
    const uint32_t end_cond = snippet_mode ? end_out & ~must_in[count] & vec_bits : out_cond;
    /* Live sets at each instruction's start, to a fixed point; the exits read the dirty registers
     * they store always, and RSP. The set after each RELOAD goes in mask[]. */
    uint32_t *live_in = calloc(count + 1, sizeof(*live_in)), live_dispatch = 0;
    live_in[count] = (end_out & ~end_cond) | 1u << RSP;
    for (int changed = 1; changed;) {
        changed = 0;
        uint32_t cases = (dirty_out & ~out_cond) | 1u << RSP;
        for (size_t i = 0; i < count; ++i) cases |= is_case[i] ? live_in[i] : 0;
        if (cases != live_dispatch) { live_dispatch = cases; changed = 1; }
        for (size_t i = count; i-- > 0;) {
            uint32_t live = flow[i] ? live_in[i + 1] : 0;
            for (size_t e = evs[i + 1]; e-- > evs[i];) {
                const SyncEvent *v = &events[e];
                if (v->kind == EV_USE) live |= v->mask;
                else if (v->kind == EV_KILL) live &= ~v->mask;
                else if (v->kind == EV_SPILL) live |= mask[e];
                else if (v->kind == EV_RELOAD) { mask[e] = live & v->mask; live &= ~v->mask; }
                else if (v->kind == EV_RELOAD_IF) mask[e] = live;
                else if (v->kind == EV_GOTO) live |= live_in[v->target];
                else if (v->kind == EV_DISPATCH) live |= live_dispatch;
            }
            if ((live_in[i] | live) != live_in[i]) { live_in[i] |= live; changed = 1; }
        }
    }
    /* Vector registers live after a RELOAD only for the stores where dv has them (cond[]): the
     * RELOAD clears dv, so on its path those stores do not run before the register is written
     * again, and its value goes nowhere. They get a constant there (kill[]), which the compiler
     * keeps in no register across a call. Sets live with those stores (cl_in), to a fixed point. */
    uint32_t *kill = calloc(evs[count] + 1, sizeof(*kill));
    uint32_t *cl_in = calloc(count + 1, sizeof(*cl_in)), cl_dispatch = 0;
    cl_in[count] = end_out | 1u << RSP;
    for (int changed = 1; changed;) {
        changed = 0;
        uint32_t cases = dirty_out | 1u << RSP;
        for (size_t i = 0; i < count; ++i) cases |= is_case[i] ? cl_in[i] : 0;
        if (cases != cl_dispatch) { cl_dispatch = cases; changed = 1; }
        for (size_t i = count; i-- > 0;) {
            uint32_t live = flow[i] ? live_in[i + 1] : 0, cl = flow[i] ? cl_in[i + 1] : 0;
            for (size_t e = evs[i + 1]; e-- > evs[i];) {
                const SyncEvent *v = &events[e];
                if (v->kind == EV_USE) { live |= v->mask; cl |= v->mask; }
                else if (v->kind == EV_KILL) { live &= ~v->mask; cl &= ~v->mask; }
                else if (v->kind == EV_SPILL) { live |= mask[e]; cl |= mask[e] | cond[e]; }
                else if (v->kind == EV_RELOAD) {
                    kill[e] = cl & ~live & vec_bits;
                    live &= ~v->mask;
                    cl &= ~(v->mask | kill[e]);
                } else if (v->kind == EV_GOTO) { live |= live_in[v->target]; cl |= cl_in[v->target]; }
                else if (v->kind == EV_DISPATCH) { live |= live_dispatch; cl |= cl_dispatch; }
            }
            if ((cl_in[i] | cl) != cl_in[i]) { cl_in[i] |= cl; changed = 1; }
        }
    }
    free(cl_in);
    uint32_t entry = live_in[0] | 1u << RSP; /* entry_rsp */
    for (size_t e = evs[count]; e < event_count; ++e) entry |= events[e].mask;
    if (fixed_base) emit("    const uint64_t B = 0x%" PRIx64 "ull;\n", fixed_base);
    else if (hook_code) emit("    const uint64_t B = c->rip - 0x%" PRIx64 "ull;\n", fn.offset);
    else emit("    const uint64_t B = rc->image_base;\n");
    emit("    uint64_t r[16];\n    RcV xmm[16];\n    uint32_t dv = 0;\n    uint64_t ef;\n   ");
    if (register_function) emit(" r[RSP] = a4; r[RDI] = a7; r[RSI] = a6; r[RDX] = a2; r[RCX] = a1;");
    sync_copy(out, register_function ? entry & ~ABI_REGISTER_ARGUMENTS : entry, 0, 1, 0);
    emit("\n    int fk = FK_CPU, fz = 8, fcf = 0;\n    uint64_t fa = 0, fb = 0, fr = 0, dn = 0;\n");
    for (size_t k = 0; k < jump_table_count; ++k) emit("    uint64_t jt_%" PRIx64 " = 0;\n", jump_tables[k].load);
    emit("    const uint64_t entry_rsp = r[RSP];\n");
    emit("    (void)fa; (void)fb; (void)fr; (void)fz; (void)fcf; (void)entry_rsp; (void)dn; (void)dv; (void)ef;\n");
    /* The text with the copies: the masks of the markers in order. */
    size_t markers = 0;
    for (size_t e = 0; e < evs[count]; ++e)
        if (events[e].kind == EV_SPILL || events[e].kind == EV_RELOAD || events[e].kind == EV_RELOAD_IF) {
            cond[markers] = cond[e];
            kill[markers] = kill[e];
            mask[markers++] = mask[e];
        }
    marker_masks = mask;
    marker_conds = cond;
    marker_kills = kill;
    marker_next = 0;
    for (size_t i = 0; i < count; ++i)
        sync_scan(text + starts[i], (size_t)(starts[i + 1] - starts[i]), -1, out);
    if (snippet_mode) {
        emit("   ");
        sync_copy(out, end_out & ~end_cond, end_cond, 0, 0);
        emit(" " FLAGS_OUT " c->rip = B + 0x%" PRIx64 "ull; return;\n", f.end);
    }
    emit("    dn = B + 0x%" PRIx64 "ull; /* past the end */\n", f.end);
    if (dispatch_cases) {
        emit("L_dispatch:\n    switch (dn - B) {\n");
        for (size_t i = 0; i < target_count; ++i)
            if (!i || targets[i] != targets[i - 1]) emit("    case 0x%" PRIx64 ": goto L_%" PRIx64 ";\n", targets[i], targets[i]);
        emit("    default: break;\n    }\n");
    }
    emit("   ");
    for (int k = 0; k < 16; ++k) /* as at a SPILL: the elided slots for the translator */
        if (elide_mask >> k & 1) emit(" rc_sts(entry_rsp - 0x%xull, 8, c->r[%d]);", elide_depth[k], k);
    sync_copy(out, dirty_out & ~out_cond, out_cond, 0, 0);
    emit(" " FLAGS_OUT "\n    c->rip = dn;\n");
    if (!hook_code) { /* hook code: what runs the function goes on from there */
        emit("    if ((dn - B >= 0x%" PRIx64 "ull && dn - B < 0x%" PRIx64 "ull)", f.start, f.end);
        for (size_t i = 0; i < entered_count; ++i)
            emit(" || (dn - B >= 0x%" PRIx64 "ull && dn - B < 0x%" PRIx64 "ull)", entered[i].start, entered[i].end);
        emit(" || r[RSP] != entry_rsp) rc->bail(c, entry_rsp); /* the translator */\n");
        emit("    else rc->tail(c); /* out of the function, the stack as at its entry: a tail call */\n");
    }
    emit(register_function ? "    return rc_back(c);\n}\n" : "}\n");
    if (setjmps) {
        if (setjmp_calls != setjmps) {
            fprintf(stderr, "recompile: %d calls of setjmp at %#" PRIx64 ", %d written\n", setjmps, fn.offset, setjmp_calls);
            exit(1);
        }
        emit("__attribute__((visibility(\"hidden\"))) void f_%" PRIx64 "%s(GuestCpu *c) {\n    RcResume rs[%d];\n"
             "    for (int k = 0; k < %d; ++k) rs[k].at = ~0u;\n    f_%" PRIx64 "%s_in(c, rs);\n    rc->leave(rs, %d);\n}\n",
             fn.offset, version_suffix, setjmps, setjmps, fn.offset, version_suffix, setjmps);
        setjmp_calls_total += (size_t)setjmps;
    }
    if (register_function) /* the slot's and the program's entry: the registers from the GuestCpu and back */
        emit("__attribute__((visibility(\"hidden\"))) void f_%" PRIx64 "%s(GuestCpu *c) {\n"
             "    const uint64_t sp = c->r[RSP];\n"
             "    const RcRet q = a_%" PRIx64 "%s(c, sp, c->r[RDI], c->r[RSI], c->r[RDX], c->r[RCX]);\n"
             "    c->r[RAX] = q.rax;\n    c->r[RDX] = q.rdx;\n    c->r[RSP] = sp + 8;\n    c->rip = rc_lds(sp, 8);\n}\n",
             fn.offset, version_suffix, fn.offset, version_suffix);
    else if (abi_mode && !snippet_mode && !version_suffix[0]) /* a_<offset> for the register convention's calls */
        emit("__attribute__((visibility(\"hidden\"))) RcRet a_%" PRIx64 "(GuestCpu *c, " REGISTER_PARAMETERS ") {\n"
             "    c->r[RSP] = a4;\n    c->r[RDI] = a7;\n    c->r[RSI] = a6;\n    c->r[RDX] = a2;\n    c->r[RCX] = a1;\n"
             "    f_%" PRIx64 "(c);\n    return rc_back(c);\n}\n",
             fn.offset, fn.offset);
    generated_register = register_function;
    register_function = 0;
    elide_candidates = elide_mask = 0;
    exit_return = "return;";
    if (trace_points) emit("#undef RC_VOUT\n");
    free(cond);
    free(kill);
    free(must_in);
    free(live_in);
    free(dirty_in);
    free(mask);
    free(is_case);
    free(flow);
    free(evs);
    free(text);
    free(starts);
    free(targets);
    free(points);
    free(list);
}

/* ---- the game's patches (--patches XML) ---- */

/* GoldHEN's patch format, as GR2port's memory patcher writes it (src/common/memory_patcher.cpp):
 * each <Metadata Name=... AppElf=...> holds a <PatchList> of <Line Type= Address= Value=>, written
 * in the file's order at Address - 0x400000. Each function a patch changes gets versions in the
 * library (plan_patches), so that whichever patches the player turns on run as C. */
typedef struct { uint64_t off; uint32_t size; uint8_t *bytes; } PatchLine;
/* `outside`: the runs of its bytes, caves code may jump into (plan_patches). */
typedef struct { char *name; PatchLine *lines; size_t count; Range *outside; size_t outside_count; } Patch;
static Patch *patches;
static size_t patch_count;

/* An attribute's value with XML's entities replaced, or NULL. */
static char *xml_attribute(const char *tag, const char *end, const char *name) {
    char key[64];
    const int key_size = snprintf(key, sizeof(key), " %s=\"", name);
    const char *at = tag;
    while (at && at < end && strncmp(at, key, (size_t)key_size)) at = strchr(at + 1, ' ');
    if (!at || at >= end) return NULL;
    at += key_size;
    const char *stop = strchr(at, '"');
    if (!stop || stop > end) return NULL;
    char *value = malloc((size_t)(stop - at) + 1);
    size_t n = 0;
    static const struct { const char *entity; char c; } entities[] = {
        {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
    while (at < stop) {
        int done = 0;
        for (size_t e = 0; e < sizeof(entities) / sizeof(entities[0]) && *at == '&'; ++e) {
            const size_t length = strlen(entities[e].entity);
            if (!strncmp(at, entities[e].entity, length)) { value[n++] = entities[e].c; at += length; done = 1; break; }
        }
        if (!done && at[0] == '&' && at[1] == '#') { /* a character reference, written as UTF-8 */
            char *after;
            const unsigned long code = at[2] == 'x' ? strtoul(at + 3, &after, 16) : strtoul(at + 2, &after, 10);
            if (*after == ';' && code < 0x110000) {
                if (code < 0x80) value[n++] = (char)code;
                else if (code < 0x800) { value[n++] = (char)(0xc0 | code >> 6); value[n++] = (char)(0x80 | (code & 0x3f)); }
                else if (code < 0x10000) { value[n++] = (char)(0xe0 | code >> 12); value[n++] = (char)(0x80 | (code >> 6 & 0x3f)); value[n++] = (char)(0x80 | (code & 0x3f)); }
                else { value[n++] = (char)(0xf0 | code >> 18); value[n++] = (char)(0x80 | (code >> 12 & 0x3f)); value[n++] = (char)(0x80 | (code >> 6 & 0x3f)); value[n++] = (char)(0x80 | (code & 0x3f)); }
                at = after + 1;
                done = 1;
            }
        }
        if (!done) value[n++] = *at++;
    }
    value[n] = 0;
    return value;
}

/* A line's bytes, as the memory patcher converts its value (convertValueToHex); 0: a type it
 * writes some other way. */
static uint32_t line_bytes(const char *type, const char *value, uint8_t **bytes) {
    const int hex = (value[0] == '0' && (value[1] == 'x' || value[1] == 'X'));
    *bytes = malloc(strlen(value) + 9);
    uint32_t n = 0;
    if (!strcmp(type, "bytes")) {
        for (size_t i = 0; value[i]; i += value[i + 1] ? 2 : 1) {
            const char pair[3] = {value[i], value[i + 1], 0};
            (*bytes)[n++] = (uint8_t)strtol(pair, NULL, 16);
        }
    } else if (!strcmp(type, "byte") || !strcmp(type, "bytes16") || !strcmp(type, "bytes32") || !strcmp(type, "bytes64")) {
        const uint64_t v = strtoull(value, NULL, hex ? 16 : 10);
        const uint32_t size = type[4] == 0 ? 1 : type[5] == '1' ? 2 : type[5] == '3' ? 4 : 8;
        for (n = 0; n < size; ++n) (*bytes)[n] = (uint8_t)(v >> (8 * n)); /* little-endian */
    } else if (!strcmp(type, "float32")) {
        const float f = strtof(value, NULL);
        memcpy(*bytes, &f, 4);
        n = 4;
    } else if (!strcmp(type, "float64")) {
        const double d = strtod(value, NULL);
        memcpy(*bytes, &d, 8);
        n = 8;
    } else if (!strcmp(type, "utf8")) {
        n = (uint32_t)strlen(value) + 1;
        memcpy(*bytes, value, n);
    }
    return n;
}

static void load_patches(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = malloc((size_t)size + 1);
    if (fread(text, 1, (size_t)size, f) != (size_t)size) { perror(path); exit(1); }
    text[size] = 0;
    fclose(f);
    size_t lines = 0;
    for (const char *m = strstr(text, "<Metadata"); m; m = strstr(m + 1, "<Metadata")) {
        const char *tag_end = strchr(m, '>');
        if (!tag_end) break;
        const char *next = strstr(m + 1, "<Metadata"), *list_end = strstr(tag_end, "</PatchList>");
        if (!list_end || (next && list_end > next)) continue;
        char *elf = xml_attribute(m, tag_end, "AppElf");
        const int eboot = elf && !strcmp(elf, "eboot.bin");
        free(elf);
        if (!eboot) continue;
        Patch p = {xml_attribute(m, tag_end, "Name"), NULL, 0, NULL, 0};
        if (!p.name) p.name = strdup("?");
        for (const char *l = strstr(tag_end, "<Line"); l && l < list_end; l = strstr(l + 1, "<Line")) {
            const char *line_end = strchr(l, '>');
            char *type = xml_attribute(l, line_end, "Type"), *at = xml_attribute(l, line_end, "Address"),
                 *value = xml_attribute(l, line_end, "Value");
            if (!type || !at || !value) {
                fprintf(stderr, "%s: a line of \"%s\" without Type, Address or Value\n", path, p.name);
                exit(1);
            }
            PatchLine line = {strtoull(at, NULL, 16) - 0x400000, 0, NULL};
            line.size = line_bytes(type, value, &line.bytes);
            if (!line.size) {
                fprintf(stderr, "%s: \"%s\": lines of type %s are not supported\n", path, p.name, type);
                exit(1);
            }
            if (line.off + line.size > image_size) {
                fprintf(stderr, "%s: \"%s\": %s is outside the image, left out\n", path, p.name, at);
            } else {
                p.lines = realloc(p.lines, (p.count + 1) * sizeof(*p.lines));
                p.lines[p.count++] = line;
                ++lines;
            }
            free(type);
            free(at);
            free(value);
        }
        patches = realloc(patches, (patch_count + 1) * sizeof(*patches));
        patches[patch_count++] = p;
    }
    free(text);
    printf("%zu patches of the eboot, %zu lines (%s)\n", patch_count, lines, path);
}

/* The function of `functions` holding image offset `off`, or -1. */
static long function_at(uint64_t off) {
    size_t lo = 0, hi = function_count;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (functions[mid].offset <= off) lo = mid + 1; else hi = mid;
    }
    return lo && off < functions[lo - 1].offset + functions[lo - 1].size ? (long)lo - 1 : -1;
}

/* Patches put on the image, and taken off again: their bytes, and their runs as the caves code
 * may jump into. */
typedef struct { uint64_t off; uint32_t size; uint8_t *saved; } Undo;
static Undo *undo;
static size_t undo_count;
static int by_start(const void *a, const void *b) {
    const Range *x = a, *y = b;
    return x->start < y->start ? -1 : x->start > y->start;
}
static void apply_patches(const uint32_t *chosen, size_t n) {
    cave_count = 0;
    patches_on = 1;
    for (size_t c = 0; c < n; ++c) {
        const Patch *p = &patches[chosen[c]];
        for (size_t i = 0; i < p->count; ++i) {
            const PatchLine *l = &p->lines[i];
            undo = realloc(undo, (undo_count + 1) * sizeof(*undo));
            undo[undo_count] = (Undo){l->off, l->size, malloc(l->size)};
            memcpy(undo[undo_count++].saved, (const void *)(uintptr_t)(base + l->off), l->size);
            memcpy((void *)(uintptr_t)(base + l->off), l->bytes, l->size);
        }
        caves = realloc(caves, (cave_count + p->outside_count + 1) * sizeof(*caves));
        memcpy(caves + cave_count, p->outside, p->outside_count * sizeof(*caves));
        cave_count += p->outside_count;
    }
    qsort(caves, cave_count, sizeof(*caves), by_start);
    size_t merged = 0;
    for (size_t i = 0; i < cave_count; ++i) {
        if (merged && caves[i].start <= caves[merged - 1].end) {
            if (caves[i].end > caves[merged - 1].end) caves[merged - 1].end = caves[i].end;
        } else {
            caves[merged++] = caves[i];
        }
    }
    cave_count = merged;
}
static void remove_patches(void) {
    while (undo_count) {
        const Undo *u = &undo[--undo_count];
        memcpy((void *)(uintptr_t)(base + u->off), u->saved, u->size);
        free(u->saved);
    }
    cave_count = 0;
    patches_on = 0;
}
static int in_patched_bytes(uint64_t off, uint64_t size) {
    for (size_t i = 0; i < undo_count; ++i)
        if (undo[i].off < off + size && off < undo[i].off + undo[i].size) return 1;
    return 0;
}

/* Whether the C of the instruction at `off` reading `field` from the image is the same whatever
 * value the field holds: it is translated twice, with the value changed in between. */
static int live_works(Range f, uint64_t off, const GuestInsn *in, const LiveField *field) {
    GuestInsn other = *in;
    int changed = 0;
    for (int k = 0; k < 5; ++k) {
        GuestOp *op = &other.op[k];
        if ((field->kind == 0 && op->type == OP_IMM) || (field->kind == 1 && (op->type == OP_MEM || op->type == OP_AGEN))) {
            op->disp ^= 0x5b;
            changed = 1;
        }
    }
    if (!changed) return 0;
    FILE *const saved_out = out;
    const LiveField *const saved_live = live;
    const size_t saved_count = live_count;
    unsigned char *const saved_read = live_read;
    const uint32_t saved_vused = vused;
    live = field;
    live_count = 1;
    live_read = NULL;
    vused = 0xffff;
    char *text[2] = {NULL, NULL};
    size_t size[2] = {0, 0};
    int ok = 1;
    uint64_t stats[3] = {0, 0, 0};
    for (int pass = 0; pass < 2; ++pass) {
        out = open_memstream(&text[pass], &size[pass]);
        ok &= translate(f, off, pass ? &other : in, stats);
        fclose(out);
    }
    out = saved_out;
    live = saved_live;
    live_count = saved_count;
    live_read = saved_read;
    vused = saved_vused;
    ok = ok && size[0] == size[1] && !memcmp(text[0], text[1], size[0]);
    free(text[0]);
    free(text[1]);
    return ok;
}

/* For each function the patches write: the operand fields they set, read from the image as the
 * code runs (LiveField); and the groups of patches whose other changes there overlap, a version
 * for each choice of at most one of each group. */
enum { MAX_VERSIONS = 64 };
typedef struct {
    size_t fn;                 /* in functions */
    LiveField *live;
    size_t live_count;
    uint32_t *options;         /* patches, by group: one for each different change */
    size_t *group_start;       /* group g's options: options[group_start[g]] .. options[group_start[g + 1]] */
    size_t group_count;
} Plan;
static Plan *plans;
static size_t plan_count;
static const Plan *plan_of(size_t fn) {
    for (size_t lo = 0, hi = plan_count; lo < hi;) {
        const size_t mid = (lo + hi) / 2;
        if (plans[mid].fn == fn) return &plans[mid];
        if (plans[mid].fn < fn) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

typedef struct { uint64_t off; uint32_t patch; uint8_t value; } Write;
static int write_order(const void *a, const void *b) {
    const Write *x = a, *y = b;
    if (x->off != y->off) return x->off < y->off ? -1 : 1;
    return x->patch < y->patch ? -1 : x->patch > y->patch;
}
static uint32_t find_root(uint32_t *parent, uint32_t i) {
    while (parent[i] != i) i = parent[i] = parent[parent[i]];
    return i;
}
static uint64_t version_hash(uint64_t off, uint64_t size, const Plan *plan);

/* A key of the code that the function decoded last reaches as functions of patches: the bytes of
 * the caves its calls and taken addresses (cave_calls) go to, and of the caves that code reaches in
 * turn. Each frame rate's patch gives the function at 0x11d4350 the same code, which passes a
 * cave's address on, and writes its own code in that cave: without the key those patches count
 * once, and only one patch's cave gets C. */
static uint64_t called_caves_key(void) {
    uint64_t key = 0xcbf29ce484222325ull, *work = malloc((cave_call_count + 1) * sizeof(*work)), *done = NULL;
    size_t pending = cave_call_count, done_count = 0;
    memcpy(work, cave_calls, cave_call_count * sizeof(*work));
    while (pending) {
        const uint64_t t = work[--pending];
        const Range *cave = cave_at(t);
        int seen = !cave;
        for (size_t k = 0; k < done_count && !seen; ++k) seen = done[k] == t;
        if (seen) continue;
        done = realloc(done, (done_count + 1) * sizeof(*done));
        done[done_count++] = t;
        const uint64_t end = cave->end;
        for (uint64_t a = t; a < end; ++a) key = (key ^ ((const uint8_t *)(uintptr_t)base)[a]) * 0x100000001b3ull;
        size_t count;
        free(decode((Function){t, end - t}, &count));
        work = realloc(work, (pending + cave_call_count + 1) * sizeof(*work));
        memcpy(work + pending, cave_calls, cave_call_count * sizeof(*work));
        pending += cave_call_count;
    }
    free(work);
    free(done);
    return key;
}

static void plan_patches(void) {
    for (uint32_t p = 0; p < patch_count; ++p) /* the runs of each patch's bytes */
        for (size_t i = 0; i < patches[p].count; ++i) {
            const PatchLine *l = &patches[p].lines[i];
            patches[p].outside = realloc(patches[p].outside, (patches[p].outside_count + 1) * sizeof(Range));
            patches[p].outside[patches[p].outside_count++] = (Range){l->off, l->off + l->size};
        }
    /* The bytes the patches change in functions. */
    Write *writes = NULL;
    size_t write_count = 0;
    for (uint32_t p = 0; p < patch_count; ++p)
        for (size_t i = 0; i < patches[p].count; ++i) {
            const PatchLine *l = &patches[p].lines[i];
            for (uint32_t k = 0; k < l->size; ++k) {
                const uint64_t off = l->off + k;
                if (function_at(off) < 0 || ((const uint8_t *)(uintptr_t)base)[off] == l->bytes[k]) continue;
                writes = realloc(writes, (write_count + 1) * sizeof(*writes));
                writes[write_count++] = (Write){off, p, l->bytes[k]};
            }
        }
    qsort(writes, write_count, sizeof(*writes), write_order);
    ZydisDecoder zd;
    ZydisDecoderInit(&zd, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    uint32_t *parent = malloc(patch_count * sizeof(*parent));
    unsigned char *code_change = calloc(patch_count, 1);
    uint64_t *code_hash = malloc(patch_count * sizeof(*code_hash));
    size_t live_total = 0, live_functions = 0, demoted = 0, versions_planned = 0, capped = 0;
    for (size_t w = 0; w < write_count;) {
        const long fn = function_at(writes[w].off);
        size_t end = w;
        while (end < write_count && function_at(writes[end].off) == fn) ++end;
        const Function F = functions[fn];
        const Range f = {F.offset, F.offset + F.size};
        size_t count;
        Insn *list = decode(F, &count);
        Plan plan = {(size_t)fn, NULL, 0, NULL, NULL, 0};
        for (uint32_t p = 0; p < patch_count; ++p) parent[p] = p, code_change[p] = 0;
        /* Each written instruction: its fields, or a change of its code. Bytes in no decoded
         * instruction are code too (a jump of a patch may reach them). */
        uint64_t last_code = UINT64_MAX; /* the instruction (or loose byte) of the last code change */
        uint32_t last_patch = 0;
        for (size_t i = w; i < end;) {
            size_t lo = 0, hi = count;
            while (lo < hi) { const size_t mid = (lo + hi) / 2; if (list[mid].off <= writes[i].off) lo = mid + 1; else hi = mid; }
            const Insn *x = lo && writes[i].off < list[lo - 1].off + list[lo - 1].in.length ? &list[lo - 1] : NULL;
            size_t j = i + 1;
            const uint64_t unit = x ? x->off : writes[i].off;
            while (j < end && (x ? writes[j].off < x->off + x->in.length : writes[j].off == writes[i].off)) ++j;
            /* writes[i, j): this instruction's. The field every one of them lies in, if any. */
            int kind = -1;
            uint64_t field = 0;
            int field_size = 0;
            ZydisDecodedInstruction zi;
            if (x && ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(&zd, NULL, (const void *)(uintptr_t)(base + x->off), 15, &zi))) {
                for (int k = 0; k < 2 && kind < 0; ++k) {
                    const uint64_t at = k ? x->off + zi.raw.disp.offset : x->off + zi.raw.imm[0].offset;
                    const int size = (k ? zi.raw.disp.size : zi.raw.imm[0].size) / 8;
                    if (!size || (!k && (zi.raw.imm[0].is_relative || zi.raw.imm[1].size))) continue;
                    int all = 1;
                    for (size_t q = i; q < j && all; ++q) all = writes[q].off >= at && writes[q].off < at + (uint64_t)size;
                    if (all && (size == 1 || size == 2 || size == 4 || size == 8)) kind = k, field = at, field_size = size;
                }
            }
            if (kind >= 0) {
                const LiveField candidate = {x->off, field, field_size, kind};
                if (live_works(f, x->off, &x->in, &candidate)) {
                    plan.live = realloc(plan.live, (plan.live_count + 1) * sizeof(*plan.live));
                    plan.live[plan.live_count++] = candidate;
                } else {
                    kind = -1;
                    ++demoted;
                }
            }
            if (kind < 0) { /* a change of the code: these patches' groups join */
                for (size_t q = i; q < j; ++q) {
                    code_change[writes[q].patch] = 1;
                    if (unit == last_code) parent[find_root(parent, writes[q].patch)] = find_root(parent, last_patch);
                    last_code = unit;
                    last_patch = writes[q].patch;
                }
            }
            i = j;
        }
        free(list);
        /* The groups, each with its different changes: patches that give the function the same
         * code (but for its live fields, with the same caves, and the same code at the caves it
         * calls) count once. */
        for (uint32_t p = 0; p < patch_count; ++p) {
            if (!code_change[p]) continue;
            apply_patches(&p, 1);
            free(decode(F, &count));
            code_hash[p] = version_hash(F.offset, F.size, &plan);
            code_hash[p] = (code_hash[p] ^ called_caves_key()) * 0x100000001b3ull;
            remove_patches();
        }
        size_t product = 1;
        plan.group_start = malloc(sizeof(*plan.group_start));
        plan.group_start[0] = 0;
        for (uint32_t r = 0; r < patch_count; ++r) {
            if (!code_change[r] || find_root(parent, r) != r) continue;
            const size_t start = plan.group_start[plan.group_count];
            size_t options = 0;
            for (uint32_t p = 0; p < patch_count; ++p) {
                if (!code_change[p] || find_root(parent, p) != r) continue;
                int same = 0;
                for (size_t o = start; o < start + options && !same; ++o)
                    same = code_hash[plan.options[o]] == code_hash[p];
                if (same) continue;
                plan.options = realloc(plan.options, (start + options + 1) * sizeof(*plan.options));
                plan.options[start + options++] = p;
            }
            plan.group_start = realloc(plan.group_start, (plan.group_count + 2) * sizeof(*plan.group_start));
            plan.group_start[++plan.group_count] = start + options;
            product *= options + 1;
            if (product > 1u << 20) product = 1u << 20;
        }
        if (plan.live_count) { ++live_functions; live_total += plan.live_count; }
        if (plan.group_count) {
            if (product - 1 > MAX_VERSIONS) {
                ++capped;
                versions_planned += plan.group_start[plan.group_count];
            } else {
                versions_planned += product - 1;
            }
        }
        if (plan.live_count || plan.group_count) {
            plans = realloc(plans, (plan_count + 1) * sizeof(*plans));
            plans[plan_count++] = plan;
        }
        w = end;
    }
    free(parent);
    free(code_change);
    free(code_hash);
    free(writes);
    printf("patches: %zu functions written; %zu operand fields read as the code runs, in %zu functions (%zu fields "
           "that are code); up to %zu versions; %zu functions with more than %d combinations: their patches alone\n",
           plan_count, live_total, live_functions, demoted, versions_planned, capped, MAX_VERSIONS);
}

/* The versions written (of planned functions, and the functions of patches they call): for the
 * library's table. */
typedef struct {
    uint64_t off, size, hash;
    char suffix[24];
    uint32_t flags;
    Range *spans;
    size_t span_count;
    const Plan *plan; /* the function's plan (its live fields); NULL for a function of patches */
} Version;
static Version *versions;
static size_t version_count, versions_broken;

/* --hook-code: the live pairs of the function `fn` of hook code, by offset: its instructions'
 * fields (hook_fields) and the bytes between its instructions, which are data or code the
 * function does not run. Returns how many; *out is malloc'd. */
static size_t hook_pairs(Function fn, uint64_t **out) {
    size_t count;
    Insn *list = decode(fn, &count);
    uint64_t *pairs = malloc((4 * count + 1) * 2 * sizeof(*pairs));
    size_t n = 0;
    uint64_t at = fn.offset; /* the end of the instructions so far */
    for (size_t i = 0; i < count; ++i) {
        const uint64_t off = list[i].off;
        if (off > at) {
            pairs[2 * n] = at;
            pairs[2 * n++ + 1] = off - at;
        }
        LiveField field[3];
        hook_fields(off, &field[1], &field[0], &field[2]); /* as the instruction holds them: disp, then imm */
        for (int k = 0; k < 3; ++k)
            if (field[k].size && field[k].field >= at) {
                pairs[2 * n] = field[k].field;
                pairs[2 * n++ + 1] = (uint64_t)field[k].size;
            }
        if (off + list[i].in.length > at) at = off + list[i].in.length;
    }
    if (at < fn.offset + fn.size) {
        pairs[2 * n] = at;
        pairs[2 * n++ + 1] = fn.offset + fn.size - at;
    }
    free(list);
    *out = pairs;
    return n;
}

/* --hook-code: whether the bytes at `off` read as code: instructions up to a ret or a jmp. */
static int hook_code_at(uint64_t off) {
    for (int steps = 0; steps < 64 && off < image_size;) {
        GuestBlock *block = guest_cpu_block_uncached(base + off);
        if (!block || !block->count) {
            free(block);
            return 0;
        }
        for (uint32_t i = 0; i < block->count; ++i, ++steps) {
            const int m = block->insn[i].mnemonic;
            if (m == M(INVALID) || m == M(INT3)) { free(block); return 0; }
            if (m == M(RET) || m == M(JMP)) { free(block); return 1; }
            off += block->insn[i].length;
        }
        free(block);
    }
    return 0;
}

/* --hook-code: the functions, from the places in the hook code that the hook sites jump to (the
 * dump's list, after hook_base): each is the code from its start to the last instruction it
 * reaches before the next one's start. Their starts are also what that code names in the hook
 * code: where a direct jump or call leaves its function for, what a jmp or call through a pointer
 * of the hook code finds there, a 64-bit immediate (a host function's call of a stub) and the
 * address a lea takes (a continuation) when code is there. */
static void hook_functions(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); exit(1); }
    uint64_t *starts = NULL;
    size_t start_count = 0, capacity = 0;
    char line[64];
    if (!fgets(line, sizeof(line), f)) { fprintf(stderr, "%s: no address of the hook code\n", path); exit(1); }
    hook_base = strtoull(line, NULL, 0);
#define ADD_START(value) do { \
        const uint64_t start_ = (value); \
        int known_ = start_ >= image_size; \
        for (size_t k_ = 0; k_ < start_count && !known_; ++k_) known_ = starts[k_] == start_; \
        if (!known_) { \
            if (start_count == capacity) starts = realloc(starts, (capacity = capacity ? capacity * 2 : 256) * sizeof(*starts)); \
            starts[start_count++] = start_; \
            changed = 1; \
        } \
    } while (0)
    int changed = 0;
    while (fgets(line, sizeof(line), f)) ADD_START(strtoull(line, NULL, 0));
    fclose(f);
    while (changed) {
        changed = 0;
        qsort(starts, start_count, sizeof(*starts), by_offset);
        const size_t known = start_count; /* ADD_START appends: the functions of this pass are the first */
        functions = realloc(functions, (known ? known : 1) * sizeof(*functions));
        function_count = known;
        for (size_t i = 0; i < known; ++i)
            functions[i] = (Function){starts[i], (i + 1 < known ? starts[i + 1] : image_size) - starts[i]};
        for (size_t i = 0; i < known; ++i) {
            const Function fn = functions[i];
            size_t count;
            Insn *list = decode(fn, &count);
            uint64_t end = fn.offset;
            for (size_t k = 0; k < count; ++k) {
                const uint64_t off = list[k].off;
                const GuestInsn *in = &list[k].in;
                const int m = in->mnemonic, cc = cc_of(m);
                if (off + in->length > end) end = off + in->length;
                if (((cc >= 0 && cc < 16) || m == M(JMP) || m == M(CALL)) && in->op[0].type == OP_IMM) {
                    const uint64_t t = off + (uint64_t)in->op[0].disp;
                    if (t < fn.offset || t >= fn.offset + fn.size) ADD_START(t);
                }
                if ((m == M(JMP) || m == M(CALL)) && in->op[0].type == OP_MEM && in->op[0].base == 0xfe &&
                    in->op[0].index == 0xff && off + in->length + (uint64_t)in->op[0].disp + 8 <= image_size) {
                    uint64_t pointer;
                    memcpy(&pointer, (const void *)(uintptr_t)(base + off + in->length + (uint64_t)in->op[0].disp), 8);
                    ADD_START(pointer - hook_base);
                }
                if (m == M(MOV) && in->op[1].type == OP_IMM && in->op[0].type == OP_REG && in->op[0].size == 8 &&
                    (uint64_t)in->op[1].disp - hook_base < image_size && hook_code_at((uint64_t)in->op[1].disp - hook_base))
                    ADD_START((uint64_t)in->op[1].disp - hook_base);
                if (m == M(LEA) && in->op[1].base == 0xfe && in->op[1].index == 0xff &&
                    off + in->length + (uint64_t)in->op[1].disp < image_size &&
                    hook_code_at(off + in->length + (uint64_t)in->op[1].disp))
                    ADD_START(off + in->length + (uint64_t)in->op[1].disp);
            }
            free(list);
            functions[i].size = end - fn.offset;
        }
    }
#undef ADD_START
    free(starts);
    /* A place without code (a list from other hook code): no function. */
    size_t kept = 0;
    for (size_t i = 0; i < function_count; ++i)
        if (functions[i].size) functions[kept++] = functions[i];
    function_count = kept;
}

/* The live pairs (image offset, size) of the function at [off, off + size), by offset: the fields
 * of its patches (plan) and the hook sites inside it. Returns how many; *out is malloc'd. */
static size_t hash_pairs(uint64_t off, uint64_t size, const Plan *plan, uint64_t **out) {
    if (hook_code) return hook_pairs((Function){off, size}, out);
    const size_t fields = plan ? plan->live_count : 0, first = hook_site_from(off);
    size_t sites = 0;
    for (size_t k = first; k < hook_site_count && hook_sites[k].start < off + size; ++k) sites += hook_sites[k].end <= off + size;
    uint64_t *pairs = malloc((fields + sites + 1) * 2 * sizeof(*pairs));
    size_t n = 0, a = 0;
    for (size_t b = first;;) {
        while (b < hook_site_count && hook_sites[b].start < off + size && hook_sites[b].end > off + size) ++b;
        const int site = b < hook_site_count && hook_sites[b].start < off + size;
        if (!site && a == fields) break;
        if (site && (a == fields || hook_sites[b].start < plan->live[a].field)) {
            pairs[2 * n] = hook_sites[b].start;
            pairs[2 * n + 1] = hook_sites[b].end - hook_sites[b].start;
            ++b;
        } else {
            pairs[2 * n] = plan->live[a].field;
            pairs[2 * n + 1] = (uint64_t)plan->live[a].size;
            ++a;
        }
        ++n;
    }
    *out = pairs;
    return n;
}

/* The hash of a function's code as the program computes it (RcFunction): its bytes and the spans
 * of the caves decode entered last, live fields as zeros. */
static uint64_t version_hash(uint64_t off, uint64_t size, const Plan *plan) {
    uint64_t *pairs, *span_pairs = malloc((span_count ? span_count : 1) * 2 * sizeof(*span_pairs));
    const size_t pair_count = hash_pairs(off, size, plan, &pairs);
    for (size_t i = 0; i < span_count; ++i) {
        span_pairs[2 * i] = spans[i].start;
        span_pairs[2 * i + 1] = spans[i].end - spans[i].start;
    }
    const uint64_t h = guest_cpu_recomp_hash_parts(base, off, size, span_pairs, span_count, pairs, pair_count);
    free(pairs);
    free(span_pairs);
    return h;
}
static int have_version(uint64_t off, uint64_t hash) {
    for (size_t i = 0; i < version_count; ++i)
        if (versions[i].off == off && versions[i].hash == hash) return 1;
    return 0;
}

/* What a file of functions starts with; in ABI mode the miss of an inline cache too (rc_miss): the
 * function at t, the library's own or another library's (RcApi.find), kept in the cache, else
 * rc_none. */
static void emit_file_header(void) {
    emit("#include \"recomp/rc.h\"\n\nextern const RcApi *rc;\n%s", abi_mode ? ENTRY_FIND : "");
    if (abi_mode)
        emit("static __attribute__((noinline, unused)) const RcEntry *rc_miss(const RcEntry **ic, uint64_t t, uint64_t base) {\n"
             "    const RcEntry *e = t - base < 0x%" PRIx64 "ull ? rc_find(t - base) : rc->find(t, base);\n"
             "    if (!e) return &rc_none;\n"
             "    __atomic_store_n(ic, e, __ATOMIC_RELAXED);\n"
             "    return e;\n}\n",
             image_size);
}

/* One file of versions after another, VERSIONS_PER_FILE functions each, compiled in parallel. */
enum { VERSIONS_PER_FILE = 24 };
static int version_files, in_version_file;
static void version_file(const char *stem_path, int stem) {
    if (out && in_version_file < VERSIONS_PER_FILE) return;
    if (out) close_output();
    char path[1100];
    snprintf(path, sizeof(path), "%.*s_v%03d.c", stem, stem_path, version_files++);
    out = fopen(path, "w+");
    if (!out) { perror(path); exit(1); }
    note_output(path, 1);
    emit("/* Written by tools/recomp/recompile.c from the game's eboot.bin and its patches: not to be distributed. */\n");
    emit_file_header();
    in_version_file = 0;
}

/* Writes `fn` as a version (the patches applied) unless one with its code exists: the function's
 * own (plan) or a function of patches (no plan). Returns 1 when it wrote one, -1 when the code is
 * the unpatched function's or an earlier version's (patched bytes in live fields and hook sites
 * only), 0 otherwise. */
static int write_version(Function fn, const Plan *plan, uint64_t unpatched_hash, const char *stem_path, int stem,
                         uint64_t *stats) {
    size_t count;
    free(decode(fn, &count));
    uint64_t size = fn.size;
    if (!plan) { /* a function of patches: the code it decodes, from its entry */
        uint64_t end = fn.offset;
        Insn *list = decode(fn, &count);
        for (size_t i = 0; i < count; ++i)
            if (list[i].off >= fn.offset && list[i].off < fn.offset + fn.size && list[i].off + list[i].in.length > end)
                end = list[i].off + list[i].in.length;
        free(list);
        size = end - fn.offset;
        if (!size) return 0;
        fn.size = size;
        free(decode(fn, &count));
    }
    const uint64_t hash = version_hash(fn.offset, size, plan);
    if ((plan && hash == unpatched_hash) || have_version(fn.offset, hash)) return -1;
    version_file(stem_path, stem);
    Version v = {fn.offset, size, hash, "", plan ? 0u : 1u, NULL, span_count, plan};
    int same_offset = 0;
    for (size_t i = 0; i < version_count; ++i) same_offset += versions[i].off == fn.offset;
    snprintf(v.suffix, sizeof(v.suffix), "_%d", same_offset + 1);
    v.spans = malloc((span_count ? span_count : 1) * sizeof(*v.spans));
    memcpy(v.spans, spans, span_count * sizeof(*spans));
    snprintf(version_suffix, sizeof(version_suffix), "%s", v.suffix);
    live = plan ? plan->live : NULL;
    live_count = plan ? plan->live_count : 0;
    const long start = ftell(out);
    uint64_t own_stats[3] = {0, 0, 0};
    writing_version = 1;
    writing_patch_code = !plan;
    version_broken = 0;
    generate(fn, own_stats);
    writing_version = 0;
    writing_patch_code = 0;
    live = NULL;
    live_count = 0;
    version_suffix[0] = 0;
    if (version_broken) {
        fseek(out, start, SEEK_SET);
        free(v.spans);
        ++versions_broken;
        return 0;
    }
    for (int k = 0; k < 3; ++k) stats[k] += own_stats[k];
    versions = realloc(versions, (version_count + 1) * sizeof(*versions));
    versions[version_count++] = v;
    ++in_version_file;
    return 1;
}

/* The versions of every planned function: each choice of at most one option per group (only the
 * options alone when there are more than MAX_VERSIONS choices), with the functions of patches its
 * calls reach. */
static void write_versions(const char *stem_path, int stem, uint64_t *stats) {
    size_t written = 0, functions_of_patches = 0, alone = 0;
    for (size_t pi = 0; pi < plan_count; ++pi) {
        const Plan *plan = &plans[pi];
        if (!plan->group_count) continue;
        const Function F = functions[plan->fn];
        uint64_t *pairs;
        const size_t pair_count = hash_pairs(F.offset, F.size, plan, &pairs);
        const uint64_t unpatched = guest_cpu_recomp_hash_parts(base, F.offset, F.size, NULL, 0, pairs, pair_count);
        free(pairs);
        size_t product = 1;
        for (size_t g = 0; g < plan->group_count; ++g) {
            product *= plan->group_start[g + 1] - plan->group_start[g] + 1;
            if (product > MAX_VERSIONS + 1) break;
        }
        const int all = product <= MAX_VERSIONS + 1;
        alone += !all;
        size_t *digit = calloc(plan->group_count, sizeof(*digit));
        uint32_t *chosen = malloc(plan->group_count * sizeof(*chosen));
        for (size_t choice = 0;; ++choice) {
            /* The next choice: a mixed-radix count over the groups (0: none of the group), or
             * one option at a time. */
            size_t n = 0;
            if (all) {
                size_t g = 0;
                while (g < plan->group_count && ++digit[g] > plan->group_start[g + 1] - plan->group_start[g]) digit[g++] = 0;
                if (g == plan->group_count) break;
                for (size_t k = 0; k < plan->group_count; ++k)
                    if (digit[k]) chosen[n++] = plan->options[plan->group_start[k] + digit[k] - 1];
            } else {
                if (choice >= plan->group_start[plan->group_count]) break;
                chosen[n++] = plan->options[choice];
            }
            for (size_t a = 1; a < n; ++a) /* in the file's order, as the patcher writes them */
                for (size_t b = a; b > 0 && chosen[b - 1] > chosen[b]; --b) {
                    const uint32_t t = chosen[b];
                    chosen[b] = chosen[b - 1];
                    chosen[b - 1] = t;
                }
            apply_patches(chosen, n);
            const int wrote = write_version(F, plan, unpatched, stem_path, stem, stats);
            if (wrote > 0) ++written;
            if (wrote != 0) {
                /* The functions of patches its calls reach, and theirs: also when the patched code
                 * runs from the unpatched function's hook points, and past one with an earlier
                 * one's code (each frame rate's patch has the same code at 0x1402d9e, which calls
                 * that patch's own at 0x10bd90). */
                uint64_t *work = malloc((cave_call_count + 1) * sizeof(*work)), *done = NULL;
                size_t pending = cave_call_count, done_count = 0;
                memcpy(work, cave_calls, cave_call_count * sizeof(*work));
                while (pending) {
                    const uint64_t t = work[--pending];
                    const Range *cave = cave_at(t);
                    int seen = !cave;
                    for (size_t k = 0; k < done_count && !seen; ++k) seen = done[k] == t;
                    if (seen) continue;
                    done = realloc(done, (done_count + 1) * sizeof(*done));
                    done[done_count++] = t;
                    const int got = write_version((Function){t, cave->end - t}, NULL, 0, stem_path, stem, stats);
                    if (got > 0) ++functions_of_patches;
                    if (got != 0) {
                        work = realloc(work, (pending + cave_call_count + 1) * sizeof(*work));
                        memcpy(work + pending, cave_calls, cave_call_count * sizeof(*work));
                        pending += cave_call_count;
                    }
                }
                free(work);
                free(done);
            }
            remove_patches();
        }
        free(digit);
        free(chosen);
    }
    if (out) close_output();
    out = NULL;
    printf("patches: %zu versions of %zu functions, %zu functions of patches, in %d files; %zu functions with only "
           "their patches alone; %zu versions data, not code (left out)\n", written, plan_count, functions_of_patches,
           version_files, alone, versions_broken);
}

/* --snippets ENCODINGS OUT.c: each instruction (hex bytes per line) at CODE_AT, alone; the ones
 * translated natively get a function s_<line number>, in a table (offset: the line number). */
#define SNIPPET_AT UINT64_C(0x30000000000)
static int snippets(const char *encodings, const char *path) {
    uint8_t *code = mmap((void *)SNIPPET_AT, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (code == MAP_FAILED) { perror("mmap"); return 1; }
    base = SNIPPET_AT;
    guest_cpu_add_guest_code(SNIPPET_AT, 0x4000);
    static GuestCpu warm;
    guest_cpu_run(&warm, 0);
    snippet_mode = 1;
    FILE *in = fopen(encodings, "r");
    out = fopen(path, "w+"); /* generate reads each function back */
    if (!in || !out) { perror(encodings); return 1; }
    emit("/* Written by tools/recomp/recompile.c --snippets (tests/fuzz_recomp.c). */\n");
    emit("#include \"recomp/rc.h\"\n\nstatic const RcApi *rc;\n");
    char line[128];
    uint64_t stats[3] = {0, 0, 0}, number = 0;
    uint64_t *lines = NULL;
    size_t count = 0, capacity = 0;
    while (fgets(line, sizeof(line), in)) {
        ++number;
        uint8_t bytes[16];
        int len = 0;
        for (const char *p = line; p[0] && p[1] && p[0] != '\n' && len < 15; p += 2) {
            unsigned v;
            if (sscanf(p, "%2x", &v) != 1) break;
            bytes[len++] = (uint8_t)v;
        }
        if (!len) continue;
        memset(code, 0x90, 64);
        memcpy(code, bytes, (size_t)len);
        guest_cpu_invalidate(SNIPPET_AT, 64);
        const GuestBlock *block = guest_cpu_block(SNIPPET_AT);
        if (!block->count || block->insn[0].length != len || block->insn[0].branch) continue;
        /* Natively translated only: a fallback is the interpreter itself. */
        const long mark = ftell(out);
        const uint64_t before = stats[0];
        snippet_line = number;
        generate((Function){0, (uint64_t)len}, stats);
        if (stats[0] == before) { fseek(out, mark, SEEK_SET); continue; }
        if (count == capacity) { capacity = capacity ? capacity * 2 : 4096; lines = realloc(lines, capacity * 8); }
        lines[count++] = number;
    }
    emit("\nstatic const RcFunction table[] = {\n");
    for (size_t i = 0; i < count; ++i) emit("    {%" PRIu64 ", 0, 0, s_%" PRIx64 "},\n", lines[i], lines[i]);
    emit("};\n\nconst RcFunction *guest_recomp_init(const RcApi *api, size_t *count) {\n");
    emit("    rc = api;\n    *count = sizeof(table) / sizeof(table[0]);\n    return table;\n}\n");
    close_output();
    printf("%zu instructions translated natively (of %" PRIu64 " lines)\n", count, number);
    return 0;
}

/* ---- copies of short functions ---- */

/* --abi: each file gets a static copy l_<offset> of every function of COPY_MOST instructions or
 * fewer that it calls in another file, and calls the copy, which the compiler can inline as it
 * does a function of the same file. A call keeps the slot's guard (d == f_<offset>), so a hook or
 * a patch's version still runs in the function's place. The copies' own calls of such functions
 * get copies too. */
enum { COPY_MOST = 30 };
#define DEFINITION HIDDEN " RcRet a_"
#define SIGNATURE "(GuestCpu *c, " REGISTER_PARAMETERS ")"
typedef struct { uint64_t offset; char *body; } Copy; /* body: the lines after the definition's */
typedef struct { char *path; int version; uint64_t *defined; size_t defined_count; } Output;
static Output *outputs;
static size_t output_count;
static Copy *copies;
static size_t copy_count;

static void note_output(const char *path, int version) {
    outputs = realloc(outputs, (output_count + 1) * sizeof(*outputs));
    outputs[output_count++] = (Output){strdup(path), version, NULL, 0};
}

static char *read_text(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f || fseek(f, 0, SEEK_END)) { perror(path); exit(1); }
    const long size = ftell(f);
    char *text = malloc((size_t)size + 1);
    rewind(f);
    if (fread(text, 1, (size_t)size, f) != (size_t)size) { perror(path); exit(1); }
    fclose(f);
    text[size] = 0;
    return text;
}

static int by_value(const void *a, const void *b) {
    const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}
static const Copy *copy_of(uint64_t offset) { /* a Copy starts with its offset */
    return copy_count ? bsearch(&offset, copies, copy_count, sizeof(*copies), by_value) : NULL;
}

/* The next call of a function in text from p, "q = a_<offset>(c, " or a tail call's
 * "return a_<offset>(c, ": where its name starts, its offset in *offset; NULL past the last. */
static const char *next_call(const char *text, const char *p, uint64_t *offset) {
    while ((p = strstr(p, "a_"))) {
        const char *name = p;
        p += 2;
        if (!(name - text >= 4 && !memcmp(name - 4, "q = ", 4)) && !(name - text >= 7 && !memcmp(name - 7, "return ", 7)))
            continue;
        char *end;
        *offset = strtoull(p, &end, 16);
        if (end != p && !strncmp(end, "(c, ", 4)) return name;
    }
    return NULL;
}

/* Adds to want the short functions that text calls and o does not define. */
static void want_calls(const Output *o, const char *text, uint64_t **want, size_t *count, size_t *capacity) {
    uint64_t offset;
    for (const char *p = text; (p = next_call(text, p, &offset)); p += 2) {
        if (!copy_of(offset)) continue;
        if (o->defined_count && bsearch(&offset, o->defined, o->defined_count, sizeof(uint64_t), by_value)) continue;
        size_t k = 0;
        while (k < *count && (*want)[k] != offset) ++k;
        if (k < *count) continue;
        if (*count == *capacity) *want = realloc(*want, (*capacity = *capacity ? *capacity * 2 : 64) * sizeof(**want));
        (*want)[(*count)++] = offset;
    }
}

/* text, its calls of the functions in want (sorted) made calls of their copies. */
static void write_calling_copies(FILE *f, const char *text, const uint64_t *want, size_t count) {
    uint64_t offset;
    const char *from = text, *call;
    while ((call = next_call(text, from, &offset))) {
        fwrite(from, 1, (size_t)(call - from), f);
        fputs(bsearch(&offset, want, count, sizeof(*want), by_value) ? "l_" : "a_", f);
        from = call + 2;
    }
    fputs(from, f);
}

static void copy_short_functions(void) {
    /* The functions each file of the image's functions defines, and the short ones' bodies. */
    size_t capacity = 0;
    for (size_t i = 0; i < output_count; ++i) {
        Output *o = &outputs[i];
        if (o->version) continue;
        char *text = read_text(o->path);
        size_t defined_capacity = 0;
        long insns = -1;
        for (char *line = text, *end; (end = strchr(line, '\n'));) {
            uint64_t at;
            long n;
            if (line[0] == '/' && sscanf(line, "/* 0x%" SCNx64 ", %*" SCNu64 " bytes, %ld instructions", &at, &n) == 2)
                insns = n;
            else if (!strncmp(line, DEFINITION, strlen(DEFINITION))) {
                char *after;
                const uint64_t offset = strtoull(line + strlen(DEFINITION), &after, 16);
                char *close = strstr(end, "\n}\n");
                if (!strncmp(after, SIGNATURE " {\n", strlen(SIGNATURE) + 3) && close) {
                    if (o->defined_count == defined_capacity) {
                        defined_capacity = defined_capacity ? defined_capacity * 2 : 256;
                        o->defined = realloc(o->defined, defined_capacity * sizeof(uint64_t));
                    }
                    o->defined[o->defined_count++] = offset;
                    if (insns >= 0 && insns <= COPY_MOST) {
                        if (copy_count == capacity) {
                            capacity = capacity ? capacity * 2 : 4096;
                            copies = realloc(copies, capacity * sizeof(*copies));
                        }
                        copies[copy_count++] = (Copy){offset, strndup(end + 1, (size_t)(close + 3 - (end + 1)))};
                    }
                    insns = -1;
                    line = close + 3;
                    continue;
                }
            }
            line = end + 1;
        }
        if (o->defined_count) qsort(o->defined, o->defined_count, sizeof(uint64_t), by_value);
        free(text);
    }
    if (copy_count) qsort(copies, copy_count, sizeof(*copies), by_value);
    /* Each file's copies, declared after its header and written after its functions. */
    size_t files = 0, written = 0;
    for (size_t i = 0; i < output_count; ++i) {
        const Output *o = &outputs[i];
        char *text = read_text(o->path), *header = strstr(text, ENTRY_FIND);
        uint64_t *want = NULL;
        size_t count = 0, want_capacity = 0;
        want_calls(o, text, &want, &count, &want_capacity);
        for (size_t k = 0; k < count; ++k) want_calls(o, copy_of(want[k])->body, &want, &count, &want_capacity);
        if (count && header) {
            header += strlen(ENTRY_FIND);
            qsort(want, count, sizeof(*want), by_value);
            FILE *f = fopen(o->path, "w");
            if (!f) { perror(o->path); exit(1); }
            fwrite(text, 1, (size_t)(header - text), f);
            for (size_t k = 0; k < count; ++k) fprintf(f, "static RcRet l_%" PRIx64 SIGNATURE ";\n", want[k]);
            write_calling_copies(f, header, want, count);
            for (size_t k = 0; k < count; ++k) {
                fprintf(f, "\n/* a copy of a_%" PRIx64 " */\nstatic RcRet l_%" PRIx64 SIGNATURE " {\n", want[k], want[k]);
                write_calling_copies(f, copy_of(want[k])->body, want, count);
            }
            if (fclose(f)) { perror(o->path); exit(1); }
            ++files;
            written += count;
        }
        free(want);
        free(text);
    }
    printf("copies: %zu of the %zu functions of %d instructions or fewer, in %zu files\n", written, copy_count, COPY_MOST,
           files);
}

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "--snippets")) return snippets(argv[2], argv[3]);
    const char *patch_file = NULL, *hook_file = NULL;
    for (;;) {
        if (argc > 1 && (!strcmp(argv[1], "--abi") || !strcmp(argv[1], "--hook-code"))) {
            if (!strcmp(argv[1], "--abi")) abi_mode = 1;
            else hook_code = 1;
            argv[1] = argv[0];
            ++argv;
            --argc;
            continue;
        }
        if (!(argc > 2 && (!strcmp(argv[1], "--patches") || !strcmp(argv[1], "--hooks") ||
                           !strcmp(argv[1], "--image-base")))) break;
        if (!strcmp(argv[1], "--patches")) patch_file = argv[2];
        else if (!strcmp(argv[1], "--image-base")) fixed_base = strtoull(argv[2], NULL, 0);
        else hook_file = argv[2];
        argv[2] = argv[0];
        argv += 2;
        argc -= 2;
    }
    if (hook_code ? argc != 4 : argc < 5) {
        fprintf(stderr, "usage: %s [--abi] [--image-base ADDRESS] [--patches XML] [--hooks FILE] ELF FUNCTIONS_TSV OUT.c OFFSET...\n"
                        "       %s --hook-code DUMP.bin DUMP.txt OUT.c\n", argv[0], argv[0]);
        return 2;
    }
    if (hook_code) map_dump(argv[1]);
    else map_image(argv[1]);
    if (patch_file) load_patches(patch_file);
    if (hook_file) load_hooks(hook_file);
    static GuestCpu warm;
    guest_cpu_run(&warm, 0); /* the interpreter's tables */
    trace_points = getenv("GUEST_RECOMP_TRACE") && getenv("GUEST_RECOMP_TRACE")[0] == '1';
    if (getenv("GUEST_RECOMP_ANNOTATIONS") && *getenv("GUEST_RECOMP_ANNOTATIONS")) load_annotations(getenv("GUEST_RECOMP_ANNOTATIONS"));
    /* OFFSET... or @LIST (one offset a line). */
    size_t capacity = (size_t)argc;
    functions = calloc(capacity, sizeof(*functions));
    for (int i = 4; i < argc; ++i) {
        FILE *list = argv[i][0] == '@' ? fopen(argv[i] + 1, "r") : NULL;
        if (argv[i][0] == '@' && !list) { perror(argv[i] + 1); return 1; }
        char line[64];
        while (list ? fgets(line, sizeof(line), list) != NULL : (snprintf(line, sizeof(line), "%s", argv[i]), 1)) {
            char *end;
            const uint64_t offset = strtoull(line, &end, 0);
            if (end != line || !list) { /* a module's init function is at 0 */
                const uint64_t size = size_of(argv[2], offset);
                if (!size) { fprintf(stderr, "%#" PRIx64 ": not in %s\n", offset, argv[2]); return 1; }
                if (function_count == capacity) { capacity *= 2; functions = realloc(functions, capacity * sizeof(*functions)); }
                functions[function_count++] = (Function){offset, size};
            }
            if (!list) break;
        }
        if (list) fclose(list);
    }
    if (hook_code) hook_functions(argv[2]);
    /* The import stubs by name, from imports.tsv next to FUNCTIONS_TSV (tools/recomp/imports.py):
     * calls of setjmp's come back from a longjmp too (is_setjmp, RcResume). */
    if (!hook_code) {
        char dir[1024];
        snprintf(dir, sizeof(dir), "%s", argv[2]);
        char *slash = strrchr(dir, '/');
        if (slash) slash[1] = 0; else dir[0] = 0;
        char path[1100], line[512];
        snprintf(path, sizeof(path), "%simports.tsv", dir);
        FILE *f = fopen(path, "r");
        while (f && fgets(line, sizeof(line), f)) {
            char name[256];
            unsigned long long stub;
            if (sscanf(line, "%llx\t%255s", &stub, name) == 2) {
                imports = realloc(imports, (import_count + 1) * sizeof(*imports));
                imports[import_count].stub = stub;
                snprintf(imports[import_count].name, sizeof(imports[import_count].name), "%s", name);
                ++import_count; /* imports.tsv is sorted */
            }
        }
        if (f) fclose(f);
        else fprintf(stderr, "%s: none (calls of setjmp are not known)\n", path);
        /* Import stubs (a jump through a slot the loader fills) are not written: the runtime calls
         * what their slot holds (rc->call). Recompiled, the jump is a tail call through the
         * runtime, which puts its own return address on the stack, where setjmp would keep it. */
        size_t stubs = 0;
        for (size_t i = 0; i < function_count;) {
            if (import_name(functions[i].offset)) { functions[i] = functions[--function_count]; ++stubs; }
            else ++i;
        }
        if (stubs) printf("%zu import stubs: called through their slots\n", stubs);
    }
    /* By offset, once each. */
    qsort(functions, function_count, sizeof(*functions), by_offset); /* offset first in Function */
    size_t unique = 0;
    for (size_t i = 0; i < function_count; ++i)
        if (!unique || functions[i].offset != functions[unique - 1].offset) functions[unique++] = functions[i];
    function_count = unique;
    /* Hook code: none called as C, each leaves to what runs it (the runtime's loop goes on). */
    recompiled = hook_code ? NULL : malloc(function_count * sizeof(*recompiled));
    for (size_t i = 0; i < function_count && recompiled; ++i) recompiled[i] = functions[i].offset;
    if (patch_count) plan_patches();
    if (abi_mode) find_jumped_to();
    tables_decoded = jumps_left = 0; /* the functions' own, below */
    /* A file per WINDOW of the image (OUT_<window>.c, compiled in parallel; the table in OUT.c):
     * adding functions changes only the files of their windows, and recomp.sh compiles only files
     * that changed. */
    enum { WINDOW = 0x20000 };
    char path[1024];
    const size_t stem = strlen(argv[3]) - (strlen(argv[3]) > 2 && !strcmp(argv[3] + strlen(argv[3]) - 2, ".c") ? 2 : 0);
    uint64_t stats[3] = {0, 0, 0}; /* translated, indirect jumps, instructions */
    snprintf(path, sizeof(path), "%.*s.not-compiled.tsv", (int)stem, argv[3]);
    not_compiled_report = fopen(path, "w");
    int files = 0;
    uint64_t window = UINT64_MAX;
    out = NULL;
    for (size_t i = 0; i < function_count; ++i) {
        if (functions[i].offset / WINDOW != window) {
            if (out) close_output();
            window = functions[i].offset / WINDOW;
            snprintf(path, sizeof(path), "%.*s_%05" PRIx64 ".c", (int)stem, argv[3], window);
            out = fopen(path, "w+"); /* generate reads each function back */
            if (!out) { perror(path); return 1; }
            note_output(path, 0);
            ++files;
            emit("/* Written by tools/recomp/recompile.c from the game's eboot.bin: not to be distributed. */\n");
            emit_file_header();
        }
        const Plan *plan = plan_of(i);
        live = plan ? plan->live : NULL;
        live_count = plan ? plan->live_count : 0;
        live_read = live_count ? calloc(live_count, 1) : NULL;
        generate(functions[i], stats);
        if (!register_functions) register_functions = calloc(function_count, 1);
        register_functions[i] = (unsigned char)generated_register;
        for (size_t k = 0; k < live_count; ++k)
            if (!live_read[k]) {
                fprintf(stderr, "recompile: %#" PRIx64 ": the field at %#" PRIx64 " that patches set is not read\n",
                        functions[i].offset, live[k].field);
                return 1;
            }
        free(live_read);
        live_read = NULL;
        live = NULL;
        live_count = 0;
    }
    if (out) close_output();
    out = NULL;
    const uint64_t base_not_compiled = not_compiled, base_tables = tables_decoded, base_jumps = jumps_left;
    uint64_t version_stats[3] = {0, 0, 0};
    if (plan_count) write_versions(argv[3], (int)stem, version_stats);
    out = fopen(argv[3], "w");
    if (!out) { perror(argv[3]); return 1; }
    emit("/* Written by tools/recomp/recompile.c from the game's eboot.bin: not to be distributed. */\n");
    emit("#include \"recomp/rc.h\"\n\n__attribute__((visibility(\"hidden\"))) const RcApi *rc;\n");
    for (size_t i = 0; i < function_count; ++i)
        emit("void f_%" PRIx64 "(GuestCpu *c);\n__attribute__((visibility(\"hidden\"))) RcFn d_%" PRIx64 ";\n",
             functions[i].offset, functions[i].offset);
    if (abi_mode) { /* the functions for indirect calls: rc_find, open addressing at most half full */
        for (size_t i = 0; i < function_count; ++i)
            emit("RcRet a_%" PRIx64 "(GuestCpu *c, " REGISTER_PARAMETERS ") " HIDDEN ";\n", functions[i].offset);
        /* rc_held: a slot never NULL, for the checks of the functions that hold a hook site and of
         * rc_none, which the indirect calls start from (at no offset, nothing to call) */
        emit("static void rc_held_fn(GuestCpu *c) {\n    (void)c;\n    __builtin_trap();\n}\n"
             "static RcFn rc_held = rc_held_fn;\n"
             "__attribute__((visibility(\"hidden\"))) const RcEntry rc_none = {~0ull, &rc_held, &rc_held, NULL};\n");
        emit("static const RcEntry entries[] = {\n");
        for (size_t i = 0; i < function_count; ++i) {
            const uint64_t o = functions[i].offset;
            if (holds_hook_site(o)) emit("    {0x%" PRIx64 ", &d_%" PRIx64 ", &rc_held, a_%" PRIx64 "},\n", o, o, o);
            else emit("    {0x%" PRIx64 ", &d_%" PRIx64 ", &d_%" PRIx64 ", a_%" PRIx64 "},\n", o, o, o, o);
        }
        emit("};\n");
        int bits = 10;
        while ((UINT64_C(1) << bits) < 2 * (uint64_t)function_count) ++bits;
        const uint64_t slots = UINT64_C(1) << bits;
        uint32_t *at = calloc(slots, sizeof(*at)); /* each slot: its entry + 1, 0 for none */
        for (size_t i = 0; i < function_count; ++i) {
            uint64_t k = (functions[i].offset * 0x9e3779b97f4a7c15ull) >> (64 - bits);
            while (at[k]) k = (k + 1) & (slots - 1);
            at[k] = (uint32_t)i + 1;
        }
        emit("static const struct { uint32_t offset, entry; } entry_at[%" PRIu64 "] = {", slots);
        for (uint64_t k = 0; k < slots; ++k)
            emit("%s{0x%" PRIx64 ", %u},", k % 16 ? "" : "\n", at[k] ? functions[at[k] - 1].offset : 0, at[k]);
        emit("\n};\n__attribute__((visibility(\"hidden\"))) const RcEntry *rc_find(uint64_t offset) {\n"
             "    for (uint64_t k = (offset * 0x9e3779b97f4a7c15ull) >> %d;; k = (k + 1) & 0x%" PRIx64 "ull) {\n"
             "        if (!entry_at[k].entry) return NULL;\n"
             "        if (entry_at[k].offset == offset) return &entries[entry_at[k].entry - 1];\n    }\n}\n",
             64 - bits, slots - 1);
        free(at);
        /* What other libraries call (RcApi.find, guest_recomp_find): the functions whose a_<offset> is
         * the register convention's own. A wrapper's f_<offset> may return elsewhere (a longjmp), which
         * only the runtime's call goes on from. */
        const size_t bytes = (function_count + 7) / 8;
        emit("static const uint8_t registered[%zu] = {", bytes ? bytes : 1);
        for (size_t k = 0; k < bytes; ++k) {
            unsigned v = 0;
            for (size_t j = 0; j < 8 && 8 * k + j < function_count; ++j)
                v |= (unsigned)(register_functions && register_functions[8 * k + j]) << j;
            emit("%s0x%02x,", k % 16 ? "" : "\n", v);
        }
        emit("\n};\n__attribute__((visibility(\"default\"))) const RcEntry *guest_recomp_find(uint64_t offset) {\n"
             "    const RcEntry *e = rc_find(offset);\n"
             "    const size_t k = e ? (size_t)(e - entries) : 0;\n"
             "    return e && registered[k >> 3] >> (k & 7) & 1 ? e : NULL;\n}\n");
    }
    /* Versions: their functions and the caves they run (sp_); the live pairs (hash_pairs) of each
     * function (lv_), and of each function of patches with any (lv_ with its suffix). */
    for (size_t v = 0; v < version_count; ++v) {
        const Version *x = &versions[v];
        emit("void f_%" PRIx64 "%s(GuestCpu *c);\n", x->off, x->suffix);
        if (!x->plan) {
            uint64_t *pairs;
            const size_t pair_count = hash_pairs(x->off, x->size, NULL, &pairs);
            if (pair_count) {
                emit("static const RcSpan lv_%" PRIx64 "%s[] = {", x->off, x->suffix);
                for (size_t k = 0; k < pair_count; ++k) emit("{0x%" PRIx64 ", %" PRIu64 "}, ", pairs[2 * k], pairs[2 * k + 1]);
                emit("};\n");
            }
            free(pairs);
        }
        if (!x->span_count) continue;
        emit("static const RcSpan sp_%" PRIx64 "%s[] = {", x->off, x->suffix);
        for (size_t k = 0; k < x->span_count; ++k)
            emit("{0x%" PRIx64 ", %" PRIu64 "}, ", x->spans[k].start, x->spans[k].end - x->spans[k].start);
        emit("};\n");
    }
    for (size_t i = 0; i < function_count; ++i) {
        uint64_t *pairs;
        const size_t pair_count = hash_pairs(functions[i].offset, functions[i].size, plan_of(i), &pairs);
        if (pair_count) {
            emit("static const RcSpan lv_%" PRIx64 "[] = {", functions[i].offset);
            for (size_t k = 0; k < pair_count; ++k) emit("{0x%" PRIx64 ", %" PRIu64 "}, ", pairs[2 * k], pairs[2 * k + 1]);
            emit("};\n");
        }
        free(pairs);
    }
    emit("\nstatic const RcFunction table[] = {\n");
    size_t next_version = 0;
    uint64_t *order = malloc((version_count + 1) * sizeof(*order)); /* versions by offset, then as written */
    for (size_t v = 0; v < version_count; ++v) order[v] = v;
    for (size_t a = 1; a < version_count; ++a)
        for (size_t b = a; b > 0 && versions[order[b - 1]].off > versions[order[b]].off; --b) {
            const uint64_t t = order[b];
            order[b] = order[b - 1];
            order[b - 1] = t;
        }
    for (size_t i = 0; i <= function_count; ++i) {
        /* Functions of patches below this function, then it, then its versions. */
        while (next_version < version_count && (i == function_count || versions[order[next_version]].off < functions[i].offset)) {
            const Version *x = &versions[order[next_version++]];
            uint64_t *pairs;
            const size_t pair_count = hash_pairs(x->off, x->size, NULL, &pairs);
            free(pairs);
            emit("    {0x%" PRIx64 ", %" PRIu64 ", 0x%" PRIx64 "ull, f_%" PRIx64 "%s, NULL, ", x->off, x->size, x->hash, x->off,
                 x->suffix);
            if (x->span_count) emit("sp_%" PRIx64 "%s, ", x->off, x->suffix);
            else emit("NULL, ");
            if (pair_count) emit("lv_%" PRIx64 "%s, ", x->off, x->suffix);
            else emit("NULL, ");
            emit("%zu, %zu, RC_PATCH_CODE},\n", x->span_count, pair_count);
        }
        if (i == function_count) break;
        const Plan *plan = plan_of(i);
        uint64_t *pairs;
        const size_t pair_count = hash_pairs(functions[i].offset, functions[i].size, plan, &pairs);
        if (pair_count) {
            emit("    {0x%" PRIx64 ", %" PRIu64 ", 0x%" PRIx64 "ull, f_%" PRIx64 ", &d_%" PRIx64 ", NULL, lv_%" PRIx64 ", 0, %zu},\n",
                 functions[i].offset, functions[i].size,
                 hook_code ? guest_cpu_recomp_hook_hash(base + functions[i].offset, functions[i].offset, functions[i].size,
                                                        pairs, pair_count)
                           : guest_cpu_recomp_hash_parts(base, functions[i].offset, functions[i].size, NULL, 0, pairs,
                                                         pair_count),
                 functions[i].offset, functions[i].offset, functions[i].offset, pair_count);
        } else {
            emit("    {0x%" PRIx64 ", %" PRIu64 ", 0x%" PRIx64 "ull, f_%" PRIx64 ", &d_%" PRIx64 "},\n", functions[i].offset,
                 functions[i].size,
                 hook_code ? guest_cpu_recomp_hook_hash(base + functions[i].offset, functions[i].offset, functions[i].size,
                                                        NULL, 0)
                           : guest_cpu_recomp_hash(base + functions[i].offset, functions[i].size),
                 functions[i].offset, functions[i].offset);
        }
        free(pairs);
        while (next_version < version_count && versions[order[next_version]].off == functions[i].offset) {
            const Version *x = &versions[order[next_version++]];
            const int has_live = pair_count != 0; /* the function's own (x->size is its size) */
            emit("    {0x%" PRIx64 ", %" PRIu64 ", 0x%" PRIx64 "ull, f_%" PRIx64 "%s, &d_%" PRIx64 ", ", x->off, x->size, x->hash,
                 x->off, x->suffix, x->off);
            if (x->span_count) emit("sp_%" PRIx64 "%s, ", x->off, x->suffix);
            else emit("NULL, ");
            if (has_live) emit("lv_%" PRIx64 ", ", x->off);
            else emit("NULL, ");
            emit("%zu, %zu},\n", x->span_count, pair_count);
        }
    }
    free(order);
    emit("};\n\n__attribute__((visibility(\"default\"))) const RcFunction *guest_recomp_init(const RcApi *api, size_t *count) {\n");
    /* The code's addresses are fixed_base's: the image anywhere else would be written over. */
    if (fixed_base) emit("    if (api->image_base != 0x%" PRIx64 "ull) __builtin_trap();\n", fixed_base);
    emit("    rc = api;\n    *count = sizeof(table) / sizeof(table[0]);\n    return table;\n}\n");
    fclose(out);
    printf("%zu functions, %" PRIu64 " instructions: %" PRIu64 " in C (%.2f%%), %" PRIu64 " stop the game or trap as on the hardware, %" PRIu64 " without C (OUT.not-compiled.tsv); %d files; %" PRIu64 " jump tables decoded, %" PRIu64 " indirect jumps left\n",
           function_count, stats[2], stats[0], stats[2] ? 100.0 * (double)stats[0] / (double)stats[2] : 0.0,
           stats[2] - stats[0] - base_not_compiled, base_not_compiled, files, base_tables, base_jumps);
    if (version_count)
        printf("versions: %" PRIu64 " instructions, %" PRIu64 " in C, %" PRIu64 " without C\n", version_stats[2],
               version_stats[0], not_compiled - base_not_compiled);
    if (hook_file) printf("hooks: %zu sites, %" PRIu64 " hook points (versions included)\n", hook_site_count, hook_points);
    if (setjmp_calls_total) printf("%zu calls of setjmp in C (versions included)\n", setjmp_calls_total);
    if (not_compiled_report) fclose(not_compiled_report);
    if (abi_mode) copy_short_functions();
    return 0;
}
