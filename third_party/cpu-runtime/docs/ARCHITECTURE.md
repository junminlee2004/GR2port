# Architecture

[← Documentation](README.md)

A PS4 game is x86-64 machine code with no source. guest-cpu runs it inside an arm64 process: it was
written so that Bloodborne's macOS port could drop Rosetta, and everything else in that port is
built for arm64. The text below describes that use; the pieces a host needs are in
[API.md](API.md).

## Why our own translator

Box64 and FEX are Linux programs: their ELF loaders, syscall layers and signal handling are built
around Linux. This port needs much less: user-mode x86-64 (no syscalls: the game reaches the OS
only through imports the runtime already implements), no self-modifying code, one fixed image.

## Shape

- **Guest memory is identity-mapped.** Guest pointers are host pointers, as today; host code reads
  and writes guest memory directly.
- **Guest → host:** a call or jump to an address outside the guest image is a host function. The
  bridge passes rdi, rsi, rdx, rcx, r8, r9 and the stack slots after the return address as integer
  arguments (arm64: x0-x5, then x6, x7 from the stack), xmm0-7 as vector arguments (q0-q7), and
  returns rax/rdx/xmm0/xmm1. SysV and AAPCS classify integer and floating-point arguments
  independently, so this is exact for up to 8 of each; variadic functions and struct returns are
  listed exceptions.
- **Host → guest:** `guest_call(fn, args...)` runs a guest function on the calling thread until
  it returns (entry, module init, atexit, once, TLS key destructors, thread entry, AvPlayer
  callbacks).
- **Threads:** each guest thread is a host thread with its own `GuestCpu` (registers, flags, YMM,
  MXCSR, fs/gs bases, x87).
- **TLS:** the translator models fs/gs bases; the guest TCB is the fs base (the Rosetta build's
  fs→gs rewrite is not applied).

## Stages

1. Interpreter (portable C, Zydis decoder), run in the x86 build with `GUEST_CPU=interp`: validates
   instruction semantics and the bridge on the known-good host. Unimplemented instructions stop
   with the guest address and bytes.
2. Host ported to arm64 (runtime, GPU library: Xbyak SRT walker, rdtsc, SSE intrinsics; arm64
   dependencies), guest code interpreted.
3. arm64 JIT (block cache, guest registers in host registers, lazy flags, block chaining, x86-TSO
   memory ordering mode), checked against the interpreter.
4. Performance: translation cache across runs, hot paths.

## Status

- Stages 1–3 done: the native arm64 build is the default on Apple Silicon (bloodborne-runtime's `build.sh`, `run.sh`;
  `GUEST_ARCH=x86_64` builds the Rosetta 2 one). The game boots, loads saves and plays.
- The JIT (`src/cpu/jit_arm64.c`) translates the integer, SSE/AVX and atomic
  instructions the game uses; anything else runs in the interpreter, in-block where possible.
  - Guest registers in host registers; xmm0–15 low halves in v16–v31 (upper ymm halves in memory).
  - Flags: per-flag liveness, looking into the next block; flags are dead across call/ret
    (`GUEST_JIT_FLAGS_ABI=0` turns that off).
  - x86-TSO: loads and stores with acquire/release (ldapr/stlr); misaligned accesses take an
    out-of-line slow path. Locked operations use LSE atomics between full barriers.
  - `rdtsc` reads the arm64 virtual counter, scaled to the PS4 clock.
- Performance: the game is GPU-bound at 1080p; the main guest thread is ~85% busy. Memory
  ordering showed no measurable cost (`GUEST_JIT_UNORDERED`, measurement only).

## Verification

- `tools/cpu/fuzz_jit.sh FILE`: each instruction run translated and interpreted with random
  inputs, results compared. `FILE` comes from `tools/cpu/encodings.c` (all encodings in
  `out/eboot.elf`) or from a game run with `GUEST_JIT_DUMP=file`.
- `tests/test_a64.c`: the emitter's encodings against the system assembler.
- `tools/cpu/test_jit.sh`: translator unit tests.

## Debugging switches

`GUEST_JIT_DENY=tokens` (interpret chosen instruction kinds), `GUEST_JIT_MAP=file` (guest block → host
code map, for bloodborne-recomp's `tools/recomp/sample_guest.py` with macOS `sample`), `GUEST_JIT_DUMP=file` (instructions seen).
Hosts can watch guest functions: `guest_cpu_set_probe` (a host function at a guest address, with the
registers) and `guest_cpu_set_guard` (one on entry and one on return: the return address is kept on a
per-thread stack). bloodborne-runtime's `BB_CB_PROBE` and command buffer guards use them.
An `int3` in guest code goes to the trap handlers (`guest_cpu_set_trap_handler`, tried in order)
with the general registers; `guest_cpu_trap_state` gives the handler the flags, MXCSR and YMM
registers too. bloodborne-runtime's game-code hooks (bbport 0.6's interface) jump to stubs
`int3; replaced instructions; jmp back` in a guest code range, so a hook sees and may change the
whole register state at any instruction.

`GUEST_JIT_FAULT_MAP=1` retains instruction ranges during translation so a terminating fault
report identifies the exact guest instruction, including out-of-line unaligned accesses. It
adds no instructions or callbacks to execution. `guest_cpu_fault_guest_regs` exposes that location
and the guest registers to the host's fault handler; without the map its location is zero.

`GUEST_JIT_WATCH=address:displacement[:displacement...]` records selected 64-bit MOV loads and
stores to one address in a 64-entry ring per thread. `GUEST_JIT_WATCH_LOAD=address` also records
that instruction's load when its effective address differs. Recording is inline, with no host
calls, allocation or locks on the execution path. The terminating report prints the last
records with thread labels and virtual-counter ticks. The host may use `guest_cpu_watch_write` to
record memory writes made by its libc wrappers. These diagnostics change timing; use them to
identify a transition, then validate the fix with them off. Never commit their game-derived
output. `tools/cpu/test_jit.sh` includes synthetic fault, slow-access and ring-wrap checks.

## Pitfalls found

- 16 KiB host pages: the GPU write tracker uses 16 KiB pages (`TRACKER_PAGE_BITS=14`).
- The native address space reserves 0x180000000–0x7000000000 (shared region): the guest lives
  above 0x8000000000.
- A heap panic during save load ("DLRegularHeap.cpp(710) improper or freed") appeared while
  vector code was much slower than integer code. It was the interpreter's `lock cmpxchg`: after a
  failed exchange it reported the value of a separate load, which another thread could have
  changed back, so a failed exchange looked successful and two threads took the same lock
  (found with the recompiler, [bloodborne-recomp](https://github.com/bloodborne-native/bloodborne-recomp)). Uneven thread speeds made it likely.
- "Unimplemented PM4 type 0" while loading (most loads in Brazilian Portuguese) was not the CPU
  layer: the game's main thread and its render thread wrote one GPU command buffer at once, which
  the PS4's timing hid (bloodborne-runtime: command buffer guards).
