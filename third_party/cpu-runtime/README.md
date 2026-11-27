# guest-cpu

guest-cpu runs x86-64 PS4 game code inside an arm64 process. It has a decoder, an interpreter, an
x86-64 to arm64 translator (JIT), a record and replay tracer, and a runtime for recompiled code.
It was written for [Bloodborne on Apple Silicon](https://github.com/bloodborne-native/bloodborne-mac),
where it has run the game since the port dropped Rosetta, and it holds no game code, addresses or
data, so other PS4 ports can use it.

```c
#include "cpu/guest_cpu.h"

guest_cpu_add_guest_code(image, image_size);          /* this memory is x86-64 code */
uint64_t args[2] = {40, 2};
uint64_t result = guest_cpu_call(function, 2, args);  /* runs it on this thread */
```

`examples/hello.c` is a complete host in about 30 lines.

## What it does

- **Guest memory is the host's.** Guest pointers are host pointers, so host code reads and writes
  guest memory directly.
- **Guest code calls host code directly.** A call to an address outside the guest code ranges runs
  a host function, with the SysV arguments moved to the arm64 ones. There is no syscall layer: the
  host implements the PS4 libraries the game imports.
- **Every guest thread is a host thread,** with its own registers, flags, YMM, MXCSR and fs/gs bases.
- **x86 memory ordering is kept** (loads and stores with acquire and release).
- **Hosts can look inside:** probes, guards, `int3` trap handlers and native replacements of guest
  functions.

## What it does not do

- It runs on arm64 macOS. The translator is arm64; the interpreter also builds for x86-64, for
  comparing against native results. Other arm64 systems are not tested.
- It does not load executables, implement PS4 libraries or draw anything: that is the host's work.
- It implements the instructions the PS4's CPU has and Bloodborne's code has needed. A missing one
  stops with the guest address and bytes, and `tools/cpu/census.c` and `encodings.c` list what an
  executable uses.
- The time stamp counter runs at the PS4's 1.6 GHz, and thread-local access follows the PS4
  loader's `fs` rewrite (`guest_cpu_set_tls_displacement`).

## Build

Needs clang, cmake and ninja on macOS. Rosetta is needed for the tests, not for guest-cpu itself.

```sh
bash build.sh                # Zydis into deps/, out/libguestcpu.a, out/hello
out/hello                    # guest returned 42
bash tools/cpu/test_jit.sh   # interpreter and translator against native x86-64 results
```

Hosts link `out/libguestcpu.a` with Zydis and Zycore, and provide two symbols, described in
[docs/API.md](docs/API.md). Set `GUEST_DEPS` to use Zydis from another prefix.

## Documentation

All of it starts at [docs/README.md](docs/README.md), and the
[wiki](https://github.com/bloodborne-native/bbcpu/wiki) mirrors it.

## Who uses it

| Project | |
|---|---|
| [bloodborne-runtime](https://github.com/bloodborne-native/bloodborne-runtime) | Runs Bloodborne's code through guest-cpu |
| [bloodborne-recomp](https://github.com/bloodborne-native/bloodborne-recomp) | The static recompiler: generates C that runs on guest-cpu's recompiled-code runtime |

## Credits and license

guest-cpu grew out of [bbport](https://github.com/deadinside28/bloodborne_pc) by deadinside28 and
[shadPS4](https://github.com/shadps4-emu/shadPS4), and its authors are in
[docs/CREDITS.md](docs/CREDITS.md). Licensed under the [GNU GPL v2 or later](LICENSE).

Not affiliated with Sony Interactive Entertainment or FromSoftware. No game code or files are
included.
