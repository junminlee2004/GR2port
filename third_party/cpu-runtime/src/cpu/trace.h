/* guest-cpu: running one guest function instruction by instruction with its memory accesses seen
 * (record.c): the recorder of GUEST_RECORD and tools/recomp/replay.c. */
#ifndef GUEST_CPU_TRACE_H
#define GUEST_CPU_TRACE_H
#include "cpu_internal.h"
#include "record.h"

/* Memory accesses: PRE before the instruction runs (its operands), POST after it (the same
 * operands: changed bytes are writes); READ and WRITE after a string instruction. */
enum { GUEST_TRACE_PRE, GUEST_TRACE_POST, GUEST_TRACE_READ, GUEST_TRACE_WRITE };
/* How the function leaves its code: a call, a tail call (jump out of [start, end)), an int3 hook. */
enum { GUEST_TRACE_CALL, GUEST_TRACE_TAIL, GUEST_TRACE_TRAP };

typedef struct GuestTrace GuestTrace;
struct GuestTrace {
    GuestCpu *cpu;
    uint64_t start, end; /* the function's code */
    uint64_t entry_rsp;
    uint64_t instructions;
    uint64_t limit; /* stop after this many instructions (0: none); `stopped` is then set */
    int stopped;
    void (*access)(GuestTrace *t, int kind, uint64_t address, uint32_t length);
    /* Runs the call (CALL: cpu->rip is the return point; TAIL: the function's return address is
     * on the stack; TRAP: cpu->rip is the int3) and leaves the cpu where the function resumes. */
    void (*call)(GuestTrace *t, const GuestInsn *in, uint64_t target, int how);
    /* An instruction whose accesses the tracer cannot list (the record is not usable). */
    void (*unsupported)(GuestTrace *t, const GuestInsn *in);
    /* Optional: before each instruction of the function (replay --lockstep). */
    void (*before)(GuestTrace *t, uint64_t rip);
};

/* Runs the function at cpu->rip (its return address on top of the stack) until it returns; with
 * t->entry_rsp set, from the middle of it (its return address at entry_rsp). */
void guest_cpu_trace(GuestTrace *t);
void guest_cpu_trace_regs(const GuestCpu *cpu, GuestRecRegs *regs);
void guest_cpu_trace_set_regs(GuestCpu *cpu, const GuestRecRegs *regs);

/* GUEST_RECORD (record.c): whether `rip` starts a function being recorded (the JIT leaves its first
 * instruction to guest_cpu_step), whether this call is to be recorded, and the recording itself
 * (returns the rip to continue at, after the function returned). */
extern int guest_cpu_record_armed;
int guest_cpu_record_target(uint64_t rip);
int guest_cpu_record_wants(uint64_t rip);
uint64_t guest_cpu_record_call(GuestCpu *cpu);

/* Native versions of game functions (native.c): the host function replacing `address`, or NULL. */
extern int guest_cpu_native_count;
extern uint64_t guest_cpu_native_calls; /* calls run natively (statistics) */
const void *guest_cpu_native_at(uint64_t address);
/* Runs native function `fn` for a guest call about to be made (its arguments in the registers,
 * no return address pushed; interp.c). */
void guest_cpu_call_native(GuestCpu *cpu, const void *fn);

/* Probes (native.c): run the probes at cpu->rip, if any (guest_cpu_step, and translated code at the
 * start of a probed block; decode.c starts a block at each probed address). */
extern int guest_cpu_probe_count;
int guest_cpu_probe_at(uint64_t address);
void guest_cpu_run_probe(GuestCpu *cpu);
/* A guarded call's return lands on GUEST_GUARD_RETURN (not guest code): the dispatchers call
 * guest_cpu_guard_return, which runs the guard's leave and continues at the real return address. */
#define GUEST_GUARD_RETURN UINT64_C(0xbb6a7d0000000000)
void guest_cpu_guard_return(GuestCpu *cpu);

/* Recompiled game functions (recomp.c, src/recomp/rc.h): the one at `address`, or NULL. */
typedef void (*RcFn)(GuestCpu *cpu);
extern int guest_cpu_recomp_count;
extern uint64_t guest_cpu_recomp_calls;
/* Counts a call of a recompiled function: per thread (in its GuestCpu, not TLS: a lookup per call),
 * added to guest_cpu_recomp_calls in batches (one shared counter costs as much as a short function
 * when every thread calls them). */
static inline void guest_cpu_recomp_counted(GuestCpu *cpu) {
    if (++cpu->recomp_pending == 4096) {
        __atomic_add_fetch(&guest_cpu_recomp_calls, cpu->recomp_pending, __ATOMIC_RELAXED);
        cpu->recomp_pending = 0;
    }
}
RcFn guest_cpu_recomp_at(uint64_t address);
/* The functions of the hook code's recompiled C (guest_cpu_recomp_load_hook_code), and the one that
 * is the hook code at `address`, or NULL: it runs with cpu->rip there and returns where the code
 * left it, cpu->rip where it went on. */
extern int guest_cpu_recomp_hook_count;
RcFn guest_cpu_recomp_hook_at(uint64_t address);
/* A host call of guest code (run_call) that starts at an import stub (jmp [rip+slot]): the function
 * its slot holds, as compiled code's calls run one. 1: done, cpu->rip is where it returned to. */
int guest_cpu_recomp_call_import(GuestCpu *cpu);
/* The hash of a function of hook code at `start`, `offset` its place in the dump its library was
 * written from: its bytes, the `live` ones (pairs of an offset in the dump and a size, in order)
 * as zeros. */
uint64_t guest_cpu_recomp_hook_hash(uint64_t start, uint64_t offset, uint64_t size, const uint64_t *live,
                                    size_t live_count);
/* The guest went on at cpu->rip after a longjmp: when a call of setjmp of compiled code left it
 * there, that code goes on (in its C frame); otherwise returns. */
void guest_cpu_recomp_landed(GuestCpu *cpu);
/* GUEST_RECOMP_TARGETS=1 with a library loaded: the census also counts every instruction the
 * interpreter runs (guest_cpu_step_insn, whatever path led there) and every host call of guest
 * code (run_call), by address. */
extern int guest_cpu_recomp_census;
void guest_cpu_recomp_note_interpreted(uint64_t rip);
void guest_cpu_recomp_note_entry(uint64_t fn);
/* Whether this thread runs recompiled functions (GUEST_RECOMP_THREADS, diagnostics). Without a
 * thread filter (guest_cpu_recomp_load sets the flag) only the flag is read. */
extern int guest_cpu_recomp_threads_filtered;
int guest_cpu_recomp_thread_named(void);
static inline int guest_cpu_recomp_thread_ok(void) {
    return !guest_cpu_recomp_threads_filtered || guest_cpu_recomp_thread_named();
}
/* The hash of the code at [start, start + size) the recompiler goes by (recomp.c): instruction
 * bytes, the loader's TCB loads (mov reg, fs:[0], rewritten as gs loads) as one token. */
uint64_t guest_cpu_recomp_hash(uint64_t start, uint64_t size);
/* A recompiled function's hash (RcFunction): [offset, offset + size) then each of `spans`, image
 * offsets and sizes in pairs, with the bytes of `live` (pairs, by offset) as zeros. */
uint64_t guest_cpu_recomp_hash_parts(uint64_t image_base, uint64_t offset, uint64_t size, const uint64_t *spans,
                                 size_t span_count, const uint64_t *live, size_t live_count);
/* Whether `in` loads the TCB pointer: mov reg, fs:[0] or the loader's gs form of it. */
int guest_cpu_is_tcb_load(const GuestInsn *in);
/* One instruction in the interpreter, without guest_cpu_step's hooks (record, native, recompiled). */
uint64_t guest_cpu_step_insn(GuestCpu *cpu, const GuestInsn *in);

#endif
