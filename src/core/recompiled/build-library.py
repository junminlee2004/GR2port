#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 GR2port
# SPDX-License-Identifier: GPL-2.0-or-later
"""build-library.py: recompiled mode's libraries, built on this machine from the player's own game,
for this machine or for another target.

  build-library.py --game GAME --work WORK [--target TARGET] [--inputs IN] [--jobs N]
                   [--toolchain LLVM] [--sysroot DIR] [--image-base ADDRESS] [--debug] [--check]
                   [--progress] [--stop-with-parent]
  build-library.py --hook-code PREFIX --work WORK [--target TARGET] [--jobs N] [--toolchain LLVM]
                   [--sysroot DIR] [--debug] [--progress] [--stop-with-parent]

GAME holds eboot.bin, sce_module/ (libc, libSceFios2, libSceJobManager, libSceNpToolkit) and
sysdata/libSceSecure.prx. IN holds GR2port's side of the build: patches-all.xml (patches-all.sh
from GR2port's patch file), hooks.txt (the code GR2port writes) and, if there is one,
hookdump.bin/.txt (a run's GUEST_RECOMP_HOOK_DUMP with the same stub layout); default: inputs/
beside this script.

WORK/library-TARGET receives librecomp.so, the modules' NAME.prx.so, hooks.so when IN has a hook
dump (GR2port's Debug.recompiled_library names librecomp.so and loads the others from its folder;
the names are the same on every target) and library.json. --hook-code builds only the hook code's
library, WORK/hooks/hooks.so, from PREFIX.bin and PREFIX.txt (a hook dump). WORK/build keeps the generated C and the objects: the
game's code, which never leaves this machine. A later build generates again only when the game, an
input or the generator changed, and compiles only the files whose C changed. --check then tests the
generated code on this machine (WORK/check): the fuzz test and, on an arm64 Mac, guest-cpu's test of
its interpreter and JIT against the x86-64 results Rosetta gives; it needs the full checkouts of the
generator and the runtime (RECOMP, GUEST_CPU_DIR), whose tests GR2port does not carry.

TARGET: host (default), arm64-apple-macos, aarch64-linux, x86_64-linux. A macOS target builds
without an SDK (clang's own headers and the generator's stand-ins in tools/recomp/sdkless; ld64.lld
off a Mac), so a Linux machine can check a Mac build; aarch64-linux off an ARM machine needs
--sysroot (the ARM64 test bench). LLVM: an LLVM release (bin/clang, bin/ld64.lld), else CLANG or
the clang on PATH. The generator is RECOMP (default: third_party/recompiler of the GR2port tree this
script is in) and the CPU runtime GUEST_CPU_DIR (default: third_party/cpu-runtime). Zydis, the x86
decoder, is RECOMP's deps/ when a build script made one, else compiled here from Zydis's sources
(ZYDIS_SRC, default: externals/zydis-4.1 with Zycore in dependencies/zycore), which needs no cmake.
The app carries the same tree. --progress prints a line for each file compiled, and
--stop-with-parent is for a program that runs the build: a SIGTERM or the program's end stops it,
its compiles included."""
import argparse
import collections
import concurrent.futures
import datetime
import hashlib
import json
import os
import pathlib
import platform
import re
import shutil
import signal
import subprocess
import sys
import threading
import time

HERE = pathlib.Path(__file__).resolve().parent
# The GR2port tree: this script is src/core/recompiled/build-library.py in it.
TREE = HERE.parents[2]
RECOMP = pathlib.Path(os.environ.get('RECOMP', TREE / 'third_party' / 'recompiler')).resolve()
GUEST = pathlib.Path(os.environ.get('GUEST_CPU_DIR', TREE / 'third_party' / 'cpu-runtime')).resolve()
TOOLS = RECOMP / 'tools' / 'recomp'
FORMAT = 1
TARGETS = ('arm64-apple-macos', 'aarch64-linux', 'x86_64-linux')
# The game's modules recompiled mode loads as libraries: name, file in GAME, register convention.
MODULES = [('libc', 'sce_module/libc.prx', True),
           ('libSceJobManager', 'sce_module/libSceJobManager.prx', True),
           ('libSceFios2', 'sce_module/libSceFios2.prx', False),
           ('libSceNpToolkit', 'sce_module/libSceNpToolkit.prx', False),
           ('libSceSecure', 'sysdata/libSceSecure.prx', False)]
CPU_SOURCES = ['cpu', 'decode', 'interp', 'interp_vec', 'interp_x87', 'jit_arm64', 'record', 'native', 'recomp']
CFLAGS = ['-O2', '-std=gnu11', '-ffp-contract=off', '-fno-strict-aliasing', '-fvisibility=hidden', '-w',
          '-DHAVE_GUEST_CPU=1']
# What a macOS library may leave to the process, all in libSystem: the memory functions clang calls,
# the 128-bit division helpers, the stack probe, _setjmp (RcResume in src/recomp/rc.h) and the maths
# functions the generator (x87) and rc.h use, where the target has no instruction for them.
LIBSYSTEM = ['memcpy', 'memmove', 'memset', 'bzero', 'memset_pattern4', 'memset_pattern8', 'memset_pattern16',
             '__divti3', '__udivti3', '__modti3', '__umodti3', '__chkstk_darwin', '_setjmp',
             'sin', 'cos', 'tan', 'atan2', 'log2', 'log10', 'exp2', 'ldexp', 'frexp', 'fmod', 'remainder', 'sqrt',
             'fabs', 'trunc', 'floor', 'ceil', 'nearbyint', 'truncf', 'floorf', 'ceilf', 'nearbyintf']


def say(text):
    print(text, flush=True)


def fail(text):
    print(f'build-library: {text}', file=sys.stderr, flush=True)
    sys.exit(1)


def run(command, step, env=None):
    result = subprocess.run([str(c) for c in command], capture_output=True, text=True, env=env)
    if result.returncode:
        tail = (result.stderr or result.stdout).strip().splitlines()[-6:]
        fail(f'{step} failed:\n  ' + '\n  '.join(tail))
    return result


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            digest.update(chunk)
    return digest.hexdigest()


def files_digest(paths, salt=''):
    """One hash over the files' names and contents, in order, and salt."""
    digest = hashlib.sha256(salt.encode())
    for path in paths:
        digest.update(f'{path.name}\0{sha256(path)}\0'.encode())
    return digest.hexdigest()


def host_target():
    system, machine = platform.system(), platform.machine().lower()
    arm = machine in ('arm64', 'aarch64')
    if system == 'Darwin' and arm:
        return 'arm64-apple-macos'
    if system == 'Linux':
        return 'aarch64-linux' if arm else 'x86_64-linux'
    fail(f'no target for {system} {machine}: name one with --target')


class Toolchain:
    def __init__(self, llvm):
        if llvm:
            self.clang = llvm / 'bin' / 'clang'
            if not self.clang.exists():
                fail(f'{llvm} is not an LLVM toolchain (no bin/clang)')
        else:
            found = os.environ.get('CLANG') or shutil.which('clang')
            if not found:
                fail('no clang: set CLANG, put one on PATH, or name an LLVM release with --toolchain '
                     '(macOS: xcode-select --install)')
            self.clang = pathlib.Path(found)
        release_ar = llvm / 'bin' / 'llvm-ar' if llvm else None
        self.ar = release_ar if release_ar and release_ar.exists() else shutil.which('ar') or shutil.which('llvm-ar') or 'ar'
        # Xcode's clang links with Apple's linker; any other clang links Mach-O with ld64.lld.
        self.apple_ld = platform.system() == 'Darwin' and not llvm
        self.version = run([self.clang, '--version'], 'Reading the compiler version').stdout.splitlines()[0]
        resource = run([self.clang, '-print-resource-dir'], 'Finding the compiler headers').stdout.strip()
        self.resource = pathlib.Path(resource) / 'include'


def target_flags(target, toolchain, sysroot, headers):
    """The compile flags and the shared library link's leading and trailing flags for target;
    headers: Zydis's include directories (the runtime's headers include it)."""
    include = ['-I', GUEST / 'src'] + [x for path in headers for x in ('-I', path)]
    if target == 'arm64-apple-macos':
        triple = ['--target=arm64-apple-macos13']
        compile_flags = triple + ['-nostdinc', '-isystem', toolchain.resource, '-isystem', TOOLS / 'sdkless',
                                  '-fno-stack-protector'] + include
        link = triple + ['-dynamiclib', '-nostdlib', '-Wl,-undefined,error'] + [f'-Wl,-U,_{s}' for s in LIBSYSTEM]
        if not toolchain.apple_ld:
            link.append('-fuse-ld=lld')
        # Apple's linker links a library only with libSystem, which it finds in the SDK.
        return compile_flags, link, ['-lSystem'] if toolchain.apple_ld else []
    # Another architecture's Linux: its triple, and lld, which links any of them.
    cross = target != host_target()
    triple = ([f'--target={target.split("-")[0]}-linux-gnu'] if cross else []) + \
             ([f'--sysroot={sysroot}'] if sysroot else [])
    link = triple + ['-shared'] + (['-fuse-ld=lld'] if cross else [])
    if target == 'aarch64-linux':
        # apple-m1: Apple Silicon's baseline (LSE atomics, RCpc loads for the guest's memory order).
        return triple + ['-mcpu=apple-m1', '-fPIC', '-D_GNU_SOURCE'] + include, link, ['-lm']
    return triple + ['-mcx16', '-fPIC', '-D_GNU_SOURCE'] + include, link, ['-lm']


def host_flags():
    """Flags for programs this machine runs (the tools, Zydis)."""
    if platform.system() == 'Darwin':
        return []  # guest_cpu.h sets HAVE_GUEST_CPU on macOS
    return ['-DHAVE_GUEST_CPU=1', '-D_GNU_SOURCE', '-fPIC'] + \
        (['-mcx16'] if platform.machine().lower() in ('x86_64', 'amd64') else [])


def zydis(work, toolchain):
    """Zydis's prefix (include/, lib/libZydis.a, lib/libZycore.a): RECOMP's deps/ (build.sh), or
    WORK/tools/deps compiled from ZYDIS_SRC (set, or no deps/), which guest-cpu's own scripts also
    take (GUEST_DEPS)."""
    deps = RECOMP / 'deps'
    if 'ZYDIS_SRC' not in os.environ and all((deps / 'lib' / f'lib{n}.a').exists() for n in ('Zydis', 'Zycore')):
        return deps
    src = pathlib.Path(os.environ.get('ZYDIS_SRC', TREE / 'externals' / 'zydis-4.1')).resolve()
    core = src / 'dependencies' / 'zycore'
    if not (src / 'src' / 'Decoder.c').exists() or not (core / 'src' / 'Zycore.c').exists():
        fail(f'no Zydis: neither {deps / "lib"} (build.sh) nor its sources at {src} (ZYDIS_SRC)')
    out = work / 'tools' / 'deps'
    if all((out / 'lib' / f'lib{n}.a').exists() for n in ('Zydis', 'Zycore')):
        return out
    say('== building Zydis')
    shutil.rmtree(out, ignore_errors=True)
    for tree in (src, core):
        shutil.copytree(tree / 'include', out / 'include', dirs_exist_ok=True)
    (out / 'obj').mkdir(parents=True)
    (out / 'lib').mkdir()
    flags = ['-O2', '-std=gnu11', '-w', '-DZYDIS_STATIC_BUILD', '-DZYCORE_STATIC_BUILD', '-I', out / 'include',
             '-I', src / 'src', *host_flags()]
    for name, sources in (('Zycore', sorted((core / 'src').rglob('*.c'))), ('Zydis', sorted((src / 'src').glob('*.c')))):
        def one(source):
            obj = out / 'obj' / f'{name}-{source.stem}.o'
            run([toolchain.clang, *flags, '-c', source, '-o', obj], f'Compiling {name} {source.name}')
            return obj

        with concurrent.futures.ThreadPoolExecutor() as pool:
            objects = list(pool.map(one, sources))
        run([toolchain.ar, 'rcs', out / 'lib' / f'lib{name}.a', *objects], f'Archiving {name}')
    return out


def build_tools(work, toolchain, headers, decoder):
    """scan and recompile for this machine in WORK/tools, again whenever their sources change."""
    out = work / 'tools'
    out.mkdir(parents=True, exist_ok=True)
    libs = [] if platform.system() == 'Darwin' else ['-lm']
    include = [x for path in headers for x in ('-I', path)]
    sources_h = sorted((GUEST / 'src').rglob('*.h'))
    cpu = [GUEST / 'src' / 'cpu' / f'{name}.c' for name in CPU_SOURCES] + [GUEST / 'src' / 'cpu' / 'hostcall.S']
    tools = {
        'scan': ([TOOLS / 'scan.c'], ['-O2', '-std=gnu11']),
        'recompile': ([TOOLS / 'recompile.c'] + cpu, ['-O1', '-g', '-std=gnu11', '-I', GUEST / 'src']),
    }
    for name, (sources, flags) in tools.items():
        command = [toolchain.clang, *flags, *host_flags(), *include, *sources, *decoder, *libs, '-o', out / name]
        stamp = out / f'{name}.fp'
        fp = files_digest(sources + sources_h + decoder, toolchain.version + ' '.join(map(str, command)))
        if (out / name).exists() and stamp.exists() and stamp.read_text() == fp:
            continue
        say(f'== building {name}')
        run(command, f'Building {name}')
        stamp.write_text(fp)
    return out, files_digest([out / 'scan', out / 'recompile'])


def generator_env():
    return {**os.environ, 'GUEST_JIT': '0', 'GUEST_RECOMP_ANNOTATIONS': ''}


def generate(build, game, inputs, tools, key, image_base):
    """The C of the executable, the modules and the hook code, unless the last run made it from the
    same game, inputs and generator."""
    stamp = build / 'generated.json'
    if stamp.exists() and json.loads(stamp.read_text()) == key and any(build.glob('gen/recomp*.c')):
        say('== generated C up to date')
        return
    stamp.unlink(missing_ok=True)
    for sub in ('scan', 'modules', 'gen'):
        shutil.rmtree(build / sub, ignore_errors=True)
        (build / sub).mkdir(parents=True)
    py = [sys.executable]
    env = generator_env()
    scan = build / 'scan'
    say('== reading the executable')
    run(py + [TOOLS / 'ps4elf.py', game / 'eboot.bin', build / 'eboot.elf'], 'Reading the executable')
    run(py + [TOOLS / 'pointers.py', game / 'eboot.bin', scan / 'pointers.txt'], 'Finding code pointers')
    run([tools / 'scan', build / 'eboot.elf', scan, scan / 'pointers.txt'], 'Finding functions')
    run(py + [TOOLS / 'imports.py', game / 'eboot.bin', scan], 'Naming imports')
    offsets = [line.split('\t', 1)[0] for line in (scan / 'functions.tsv').read_text().splitlines()[1:] if line.strip()]
    (scan / 'all.txt').write_text('\n'.join(offsets) + '\n')
    say(f'== generating the executable\'s C ({len(offsets)} functions)')
    base = ['--image-base', f'{image_base:#x}'] if image_base else []
    result = run([tools / 'recompile', '--abi', *base, '--patches', inputs / 'patches-all.xml',
                  '--hooks', inputs / 'hooks.txt', build / 'eboot.elf', scan / 'functions.tsv',
                  build / 'gen' / 'recomp.c', '@' + str(scan / 'all.txt')], 'Generating the executable\'s C', env)
    (build / 'generate.log').write_text(result.stdout + result.stderr)
    for name, path, abi in MODULES:
        say(f'== generating {name}')
        out = build / 'modules' / name
        run(py + [TOOLS / 'module.py', game / path, out], f'Reading {name}', {**os.environ, 'GUEST_SCAN': str(tools / 'scan')})
        gen = build / 'gen' / 'modules' / name
        gen.mkdir(parents=True)
        result = run([tools / 'recompile', *(['--abi'] if abi else []), out / 'module.elf', out / 'functions.tsv',
                      gen / 'recomp.c', '@' + str(out / 'offsets.txt')], f'Generating {name}', env)
        with open(build / 'generate.log', 'a') as log:
            log.write(result.stdout + result.stderr)
    if (inputs / 'hookdump.bin').exists():
        with open(build / 'generate.log', 'a') as log:
            log.write(generate_hook_code(tools, inputs / 'hookdump', build / 'gen'))
    stamp.write_text(json.dumps(key))


def generate_hook_code(tools, dump, gen):
    """The C of the hook code from the dump DUMP.bin and DUMP.txt into gen/hooks; returns the
    generator's output."""
    say('== generating the hook code')
    (gen / 'hooks').mkdir(parents=True)
    result = run([tools / 'recompile', '--hook-code', f'{dump}.bin', f'{dump}.txt', gen / 'hooks' / 'hooks.c'],
                 'Generating the hook code', generator_env())
    return result.stdout + result.stderr


COMMENT = re.compile(r'/\*.*?\*/', re.S)


def fingerprint(source, salt):
    """The C a file compiles, without its comments, with the compiler and its flags: equal
    fingerprints give the same object."""
    digest = hashlib.sha256(salt.encode())
    digest.update(COMMENT.sub('', source.read_text(errors='replace')).encode())
    return digest.hexdigest()


def compile_all(toolchain, flags, gen, obj, jobs, headers, progress=False):
    """Every generated file into obj (the same tree), the largest first; a file whose fingerprint
    (with the compiler, the flags and the headers' contents) is unchanged keeps its object."""
    sources = sorted(gen.glob('recomp*.c')) + sorted(gen.glob('modules/*/recomp*.c')) + sorted(gen.glob('hooks/hooks*.c'))
    target = {s: obj / (str(s.relative_to(gen)) + '.o') for s in sources}
    wanted = set(target.values())
    for stale in obj.rglob('*.o'):  # files the generator no longer writes
        if stale not in wanted:
            stale.unlink()
            stale.with_suffix('.fp').unlink(missing_ok=True)
    for leftover in obj.rglob('*.o-partial'):  # an interrupted compile
        leftover.unlink()
    salt = toolchain.version + '\n' + ' '.join(map(str, flags)) + '\n' + files_digest(headers)
    prints = {s: fingerprint(s, salt) for s in sources}

    def current(source):
        stamp = target[source].with_suffix('.fp')
        return target[source].exists() and stamp.exists() and stamp.read_text() == prints[source]

    todo = sorted((s for s in sources if not current(s)), key=lambda s: -s.stat().st_size)
    say(f'== compiling {len(todo)} of {len(sources)} files ({len(sources) - len(todo)} unchanged)')

    def one(source):
        out = target[source]
        out.parent.mkdir(parents=True, exist_ok=True)
        partial = out.with_suffix('.o-partial')
        result = subprocess.run([str(toolchain.clang), *map(str, flags), '-c', str(source), '-o', str(partial)],
                                capture_output=True, text=True)
        if result.returncode:
            return source, result.stderr.strip().splitlines()[-4:]
        partial.rename(out)
        out.with_suffix('.fp').write_text(prints[source])
        return source, None

    done, started = 0, time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for future in concurrent.futures.as_completed([pool.submit(one, s) for s in todo]):
            source, error = future.result()
            if error:
                fail(f'compiling {source.relative_to(gen)} failed:\n  ' + '\n  '.join(error))
            done += 1
            if progress or done % 10 == 0 or done == len(todo):
                say(f'   {done}/{len(todo)} ({round(time.monotonic() - started)} s)')
    return target


def nan_choice(line):
    """Whether a fuzz test FAIL line differs only in vector lanes that are NaN in both results:
    which NaN an operation keeps (x86 its first operand's; the compiler may swap the operands)."""
    if 'differs' in line:
        return False
    found = re.findall(r'(\w+): interp ([0-9a-f]+) recomp ([0-9a-f]+)', line)
    nan = lambda v: (v & 0x7f800000) == 0x7f800000 and v & 0x7fffff
    for register, a, b in found:
        if not register.startswith(('xmm', 'ymm')) or len(a) != len(b) or len(a) % 8:
            return False
        for i in range(0, len(a), 8):
            x, y = int(a[i:i + 8], 16), int(b[i:i + 8], 16)
            if x != y and not (nan(x) and nan(y)):
                return False
    return bool(found)


def check(work, build, toolchain, deps, tools):
    """This machine's fuzz test of the generated C (tests/fuzz_recomp.c, as tools/recomp/fuzz.sh
    runs it: every distinct encoding of the executable, at most 30 per mnemonic, from random states
    through the interpreter and through its generated function) and, on an arm64 Mac, guest-cpu's
    test of its interpreter and JIT against the x86-64 results Rosetta gives
    (tools/cpu/test_jit.sh)."""
    out = work / 'check'
    out.mkdir(parents=True, exist_ok=True)
    darwin = platform.system() == 'Darwin'
    libs = [] if darwin else ['-lm']
    include = ['-I', deps / 'include', '-I', GUEST / 'src']
    decoder = [deps / 'lib' / 'libZydis.a', deps / 'lib' / 'libZycore.a']
    cpu = [GUEST / 'src' / 'cpu' / f'{name}.c' for name in CPU_SOURCES] + [GUEST / 'src' / 'cpu' / 'hostcall.S']
    env = {**os.environ, 'GUEST_JIT': '0'}
    say('== fuzz test: building')
    run([toolchain.clang, '-O2', '-std=gnu11', *host_flags(), *include, GUEST / 'tools' / 'cpu' / 'encodings.c', *decoder,
         *libs, '-o', out / 'encodings'], 'Building the encoding lister')
    (out / 'encodings.txt').write_text(run([out / 'encodings', build / 'eboot.elf', 'all', '30'], 'Listing encodings').stdout)
    run([tools / 'recompile', '--snippets', out / 'encodings.txt', out / 'snippets.c'], 'Generating the snippets', env)
    run([toolchain.clang, '-O1', '-std=gnu11', '-w', '-ffp-contract=off', '-fno-strict-aliasing', *host_flags(),
         *(['-dynamiclib'] if darwin else ['-shared']), *include, out / 'snippets.c', *libs, '-o', out / 'libsnippets.so'],
        'Building the snippets')
    run([toolchain.clang, '-O2', '-g', '-std=gnu11', *host_flags(), *include, RECOMP / 'tests' / 'fuzz_recomp.c', *cpu,
         *decoder, *libs, '-o', out / 'fuzz_recomp'], 'Building the fuzz test')
    say('== fuzz test: running')
    result = subprocess.run([str(out / 'fuzz_recomp'), str(out / 'encodings.txt'), str(out / 'libsnippets.so'), '200',
                             '1000000'], capture_output=True, text=True, env=env)
    (out / 'fuzz.log').write_text(result.stdout + result.stderr)
    lines = result.stdout.splitlines()
    summary = [line for line in lines if 'instructions tested' in line]
    say('   ' + (summary[-1] if summary else f'no summary (exit {result.returncode}): see {out / "fuzz.log"}'))
    fails = [line for line in lines if line.startswith(('FAIL', 'CRASH', 'STOP'))]
    nans = [line for line in fails if line.startswith('FAIL') and nan_choice(line)]
    if nans:
        say(f'   {len(nans)} of the failures only in which NaN a result keeps')
    for what, n in collections.Counter(' '.join(line.split()[:2]) for line in fails if line not in nans).most_common():
        say(f'   {n:5} {what}')
    say(f'   details: {out / "fuzz.log"}')
    if darwin and platform.machine() == 'arm64':
        say('== guest-cpu test: interpreter and JIT against x86-64 under Rosetta (tools/cpu/test_jit.sh)')
        (GUEST / 'out').mkdir(exist_ok=True)
        result = subprocess.run(['bash', str(GUEST / 'tools' / 'cpu' / 'test_jit.sh')], capture_output=True, text=True,
                                env={**os.environ, 'GUEST_DEPS': str(deps)})
        (out / 'test_jit.log').write_text(result.stdout + result.stderr)
        tail = (result.stdout + result.stderr).strip().splitlines()[-10:]
        say(f'   exit {result.returncode}; last lines:\n     ' + '\n     '.join(tail))
        say(f'   details: {out / "test_jit.log"}')


def link(toolchain, lead, tail, objects, out, apple):
    partial = out.with_name(out.name + '-partial')
    name = ['-install_name', f'@rpath/{out.name}'] if apple else []
    run([toolchain.clang, *lead, *name, *objects, *tail, '-o', partial], f'Linking {out.name}')
    partial.rename(out)
    say(f'   {out.name}: {out.stat().st_size:,} bytes')


def stop_with_parent():
    """The build in a process group of its own, which a SIGTERM (the program's Cancel) or the end
    of the program that started it (the process then has another parent) ends, compiles
    included."""
    parent = os.getppid()
    os.setpgrp()

    def end(*_):
        os.killpg(0, signal.SIGKILL)

    signal.signal(signal.SIGTERM, end)

    def watch():
        while os.getppid() == parent:
            time.sleep(1)
        end()

    threading.Thread(target=watch, daemon=True).start()


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--game', type=pathlib.Path)
    parser.add_argument('--hook-code', type=pathlib.Path, metavar='PREFIX',
                        help='build only the hook code\'s library from PREFIX.bin and PREFIX.txt')
    parser.add_argument('--work', type=pathlib.Path, required=True)
    parser.add_argument('--target', default='host', choices=('host',) + TARGETS)
    parser.add_argument('--inputs', type=pathlib.Path, default=HERE / 'inputs')
    parser.add_argument('--toolchain', type=pathlib.Path)
    parser.add_argument('--sysroot', type=pathlib.Path)
    parser.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) - 2))
    parser.add_argument('--debug', action='store_true', help='compile with -g')
    parser.add_argument('--image-base', default='0x400000',
                        help='where GR2port maps the executable, a constant in its code (0x400000); '
                             '"run-time" makes the code read the address from the runtime instead')
    parser.add_argument('--progress', action='store_true', help='a line for each file compiled')
    parser.add_argument('--stop-with-parent', action='store_true',
                        help='stop when the program that started the build ends')
    parser.add_argument('--check', action='store_true',
                        help='then test the generated code on this machine (fuzz test; arm64 Mac: guest-cpu test)')
    args = parser.parse_args()
    if args.stop_with_parent:
        stop_with_parent()
    started = time.monotonic()
    target = host_target() if args.target == 'host' else args.target
    if args.hook_code:
        for path in (f'{args.hook_code}.bin', f'{args.hook_code}.txt'):
            if not pathlib.Path(path).exists():
                fail(f'{path} is missing')
    elif not args.game:
        fail('--game or --hook-code is required')
    else:
        for path in ['eboot.bin'] + [m[1] for m in MODULES]:
            if not (args.game / path).exists():
                fail(f'{args.game / path} is missing')
        for path in ('patches-all.xml', 'hooks.txt'):
            if not (args.inputs / path).exists():
                fail(f'{args.inputs / path} is missing')
        if (args.inputs / 'hookdump.bin').exists() != (args.inputs / 'hookdump.txt').exists():
            fail(f'{args.inputs} has one of hookdump.bin and hookdump.txt without the other')
    if not (GUEST / 'src' / 'recomp' / 'rc.h').exists():
        fail(f'no guest-cpu at {GUEST} (GUEST_CPU_DIR)')
    api = re.search(r'#define\s+GUEST_RECOMP_API_VERSION\s+(\d+)', (GUEST / 'src' / 'recomp' / 'rc.h').read_text())
    toolchain = Toolchain(args.toolchain)
    build = args.work / 'build'
    build.mkdir(parents=True, exist_ok=True)
    deps = zydis(args.work, toolchain)
    headers, decoder = [deps / 'include'], [deps / 'lib' / 'libZydis.a', deps / 'lib' / 'libZycore.a']
    tools, tools_digest = build_tools(args.work, toolchain, headers, decoder)
    flags, lead, tail = target_flags(target, toolchain, args.sysroot, headers)
    flags = CFLAGS + (['-g'] if args.debug else []) + flags
    # What the generated C can include: the runtime's headers, Zydis's, on macOS the SDK stand-ins.
    dirs = [GUEST / 'src', deps / 'include'] + ([TOOLS / 'sdkless'] if target.endswith('apple-macos') else [])
    included = [p for d in dirs for p in sorted(d.rglob('*')) if p.suffix in ('.h', '.inc') and p.is_file()]
    apple = target.endswith('apple-macos')
    if args.hook_code:
        hooks = args.work / 'hooks'
        shutil.rmtree(hooks / 'gen', ignore_errors=True)
        (hooks / 'generate.log').write_text(generate_hook_code(tools, args.hook_code, hooks / 'gen'))
        objects = compile_all(toolchain, flags, hooks / 'gen', hooks / f'obj-{target}', args.jobs, included,
                              args.progress)
        say(f'== linking for {target}')
        link(toolchain, lead, tail, list(objects.values()), hooks / 'hooks.so', apple)
        say(f'== done in {round(time.monotonic() - started)} s: {hooks / "hooks.so"}')
        return
    game_files = [args.game / 'eboot.bin'] + [args.game / m[1] for m in MODULES]
    inputs = [args.inputs / p for p in ('patches-all.xml', 'hooks.txt', 'hookdump.bin', 'hookdump.txt')]
    inputs = [p for p in inputs if p.exists()]
    image_base = 0 if args.image_base == 'run-time' else int(args.image_base, 0)
    key = {'format': FORMAT, 'tools': tools_digest, 'game': files_digest(game_files), 'inputs': files_digest(inputs),
           'scripts': files_digest(sorted(TOOLS.glob('*.py'))), 'image_base': image_base}
    generate(build, args.game, args.inputs, tools, key, image_base)
    objects = compile_all(toolchain, flags, build / 'gen', build / f'obj-{target}', args.jobs, included,
                          args.progress)
    out = args.work / f'library-{target}'
    out.mkdir(exist_ok=True)
    say(f'== linking for {target}')
    groups = [('librecomp.so', 'recomp*.c')] + [(f'{m[0]}.prx.so', f'modules/{m[0]}/recomp*.c') for m in MODULES]
    if any((build / 'gen').glob('hooks/hooks*.c')):
        groups.append(('hooks.so', 'hooks/hooks*.c'))
    else:
        (out / 'hooks.so').unlink(missing_ok=True)
    for name, pattern in groups:
        link(toolchain, lead, tail, [objects[s] for s in sorted((build / 'gen').glob(pattern))], out / name, apple)
    manifest = {**key, 'target': target, 'api': int(api.group(1)) if api else None, 'compiler': toolchain.version,
                'flags': ' '.join(map(str, flags)),
                'built': datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds'),
                'seconds': round(time.monotonic() - started)}
    (out / 'library.json').write_text(json.dumps(manifest, indent=1) + '\n')
    say(f'== done in {manifest["seconds"]} s: {out}')
    if args.check:
        check(args.work, build, toolchain, deps, tools)


if __name__ == '__main__':
    main()
