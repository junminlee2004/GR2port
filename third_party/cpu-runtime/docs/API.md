# Writing a host

[← Documentation](README.md)

A host is the program that links guest-cpu: it loads the game, implements what the game imports, and
calls guest code. `src/cpu/guest_cpu.h` is the interface, with every call described there. This page
is the order to use it in. `examples/hello.c` is the smallest working host.

## What a host provides

| Symbol | |
|---|---|
| `void *runtime_low_map(size_t size, int prot)` | Memory for guest stacks |
| `uint64_t guest_image_base` | The guest image's base. Fault reports and recordings print offsets from it; 0 when there is no image |

## Starting

1. Map the guest image at its address. Guest pointers are host pointers, so the image and the
   guest's data must be reachable as they are.
2. Tell guest-cpu where the code is: `guest_cpu_add_guest_code`. Anything outside these ranges is host
   code, so an import is a host function pointer written into the guest's import table.
3. Set hooks before guest code runs: `guest_cpu_set_native`, `guest_cpu_set_probe`, `guest_cpu_set_guard`.
4. Install a fault handler that passes faults to `guest_cpu_handle_alignment_fault` first: on arm64,
   some misaligned accesses fault and are emulated there.
5. Call guest code: `guest_cpu_call`, `guest_cpu_call_full`, or `guest_cpu_call_on_stack` for a thread's entry
   on a guest stack.

## Threads

Each guest thread is a host thread. Give it its guest thread pointer with
`guest_cpu_set_thread_pointer` before it runs guest code. A new thread starts with its creator's
floating-point control words.

## Calling host functions

Integer arguments arrive in rdi, rsi, rdx, rcx, r8 and r9, then from the stack, and vector ones in
xmm0-7. They reach the host function as x0-x7 and q0-q7, so this is exact for up to 8 of each.
Variadic functions and struct returns need the host's care.

## Replacing guest code

| Way | Use |
|---|---|
| `guest_cpu_set_native` | A host function runs instead of a guest function |
| `guest_cpu_set_probe` | A host function sees the registers, then the guest code runs |
| `guest_cpu_set_guard` | Host functions run on entry and on return |
| `guest_cpu_set_trap_handler` | `int3` patched into guest code reaches the host with all registers |
| `guest_cpu_invalidate` | Guest code was patched while running: drop what was decoded |

## Recompiled code

`guest_cpu_recomp_load` loads the library named by `GUEST_RECOMP_LIB`: C generated ahead of time from the
executable. `src/recomp/rc.h` is that interface, and `GUEST_RECOMP_API_VERSION` changes whenever it
does. A generator must be built against the same version as the library that loads its output.
[bloodborne-recomp](https://github.com/bloodborne-native/bloodborne-recomp) is the generator for
Bloodborne, and it checks the version when it builds. Functions the library does not hold, and
those excluded with `guest_cpu_recomp_exclude`, stay with the translator.

## Reports

`guest_cpu_report` prints statistics. `guest_cpu_describe_fault` and `guest_cpu_fault_guest_regs` describe a
fault in translated code. The switches for diagnosing the translator are in
[Architecture](ARCHITECTURE.md#debugging-switches).
