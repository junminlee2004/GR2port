# Recompiler

The generator of recompiled mode's libraries: it turns the player's own copy of the game into C,
which the player's Mac compiles into the libraries recompiled mode loads. GR2port's builder,
`src/core/recompiled/build-library.py`, runs it, on the Mac from the app.

- Origin: bloodborne-recomp (https://github.com/bloodborne-native/bloodborne-recomp) at commit
  c60ccf8be752 of 2026-11-03, by the authors `docs/CREDITS.md` names, under the GNU GPL v2 or later
  (`LICENSE`). `tools/recomp/ps4elf.py` is bbport's, as `docs/CREDITS.md` says.
- GR2port's changes on top of that commit: `bbrecomp.c` became `recompile.c`, which gained the
  options `--abi`, `--patches` (versions of the functions GR2port's patches write), `--hooks`
  (hook points at the sites GR2port writes), `--hook-code` (GR2port's own stubs as C) and
  `--image-base`; `module.py` reads the game's modules; `imports.py` and `import_names.inc`
  changed; and the C calls the CPU runtime by the names guest-cpu gave it
  (`third_party/cpu-runtime`).
- Here: the generator (`recompile.c`, `scan.c` and `import_names.inc`), the scripts that read the
  game's executable and modules, and the standard headers the libraries build with
  (`tools/recomp/sdkless`). Not here: the rest of that repository, which `docs/CREDITS.md` names.
- It holds no game code, addresses or data. The game's code it reads stays on the player's Mac.
