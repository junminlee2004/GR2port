# CPU runtime

Recompiled mode, the way GR2port runs the game on Apple Silicon, builds this folder into the static
library `cpu_runtime` (`ENABLE_RECOMPILED_MODE` in `CMakeLists.txt`). It loads the recompiled
libraries and runs the x86-64 code their C does not cover.

- Origin: bbcpu (https://github.com/bloodborne-native/bbcpu) at commit 3f9b6c1a11d5 of 2026-11-02,
  by the authors `docs/CREDITS.md` names, under the GNU GPL v2 or later (`LICENSE`).
- GR2port's changes on top of that commit: the names (`bbcpu_` became `guest_cpu_`, and the
  library guest-cpu), a context API for fibers (`guest_cpu_context_create`, `_switch` and
  `_destroy`), SSE4.2's string compares (PCMPxSTRx) in the interpreter, host calls of an import
  stub that run the function its slot holds, and the recompiled-mode work of `src/cpu/recomp.c`
  (with a library of hook code for each layout of the host's stubs: `guest_cpu_recomp_hook_fits`
  and `guest_cpu_recomp_dump_hook_code`).
- Here: `src/cpu`, `src/recomp/rc.h`, the license, the readme and the documents. Not here: the
  build script, tests, tools and examples that the readme and the documents name.
- It holds no game code, addresses or data.
