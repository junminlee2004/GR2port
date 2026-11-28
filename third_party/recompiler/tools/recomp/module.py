#!/usr/bin/env python3
"""tools/recomp/module.py PRX OUTDIR: one of the game's modules (sce_module/*.prx) as a library of its
own, at the base the loader gives it: OUTDIR/module.elf and the tables recompile reads next to
functions.tsv (tools/recomp/scan.c, imports.py), and OUTDIR/offsets.txt, the functions to generate.
Functions that switch stacks (contexts, unwinding) stay with the translator, and so do the
module's callers of those that return twice (setjmp, getcontext): only a call of an import stub
of setjmp comes back to C from a longjmp (RcResume in src/recomp/rc.h). setjmp and longjmp are
plain code: the runtime follows a longjmp's return to another address. A call that never returns
(longjmp, a throw) ends its caller, which stays C. Needs GUEST_SCAN (out/recomp/scan)."""
import os
import pathlib
import re
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import ps4elf  # noqa: E402

STACK_JUMPERS = re.compile(r'^_*(get|set|make|swap)context$|^_*Unwind|^__cxa_(throw|rethrow)$')
RETURNS_TWICE = re.compile(r'^_*(sig)?setjmp$|^_*(get|swap)context$')
PT_DYNAMIC, DT_INIT, DT_FINI = 2, 12, 13


def rows(path):
    return [line.split('\t') for line in path.read_text().splitlines()[1:] if line.strip()]


def loader_calls(prx):
    """The module's init and fini functions (DT_INIT, DT_FINI), which the loader calls: nothing in
    the module points to them, and the start code at the init has no unwind entry."""
    elf, _, headers, _, _ = ps4elf.parse_self(prx.read_bytes())
    dynamic = next(p for p in headers if p['type'] == PT_DYNAMIC)
    tags = {}
    for pos in range(dynamic['offset'], dynamic['offset'] + dynamic['filesz'], 16):
        tag, value = ps4elf.unpack('<QQ', elf, pos)
        if not tag:
            break
        tags[tag] = value
    return [tags[tag] for tag in (DT_INIT, DT_FINI) if tag in tags]


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    prx, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    run = lambda *args: subprocess.run([str(a) for a in args], check=True, capture_output=True)
    run(sys.executable, HERE / 'ps4elf.py', prx, out / 'module.elf')
    run(sys.executable, HERE / 'pointers.py', prx, out / 'pointers.txt')
    with open(out / 'pointers.txt', 'a') as f:
        f.writelines(f'{address:#x}\n' for address in loader_calls(prx))
    run(os.environ['GUEST_SCAN'], out / 'module.elf', out, out / 'pointers.txt')
    run(sys.executable, HERE / 'imports.py', prx, out)
    known = {}
    for name, label in re.findall(r'\{"([^"]+)","([^"]+)"\}', (HERE / 'import_names.inc').read_text()):
        known[name.split('#')[0]] = label
    jumpers, twice = set(), set()
    for symbol in ps4elf.module(prx)['symbols']:
        name = known.get(symbol['name'].split('#')[0], symbol['name'])
        if symbol['value'] and STACK_JUMPERS.search(name):
            jumpers.add(symbol['value'])
        if symbol['value'] and RETURNS_TWICE.search(name):
            twice.add(symbol['value'])
    callers = {int(row[0], 16) for row in rows(out / 'calls.tsv') if int(row[1], 16) in twice}
    functions = [int(row[0], 16) for row in rows(out / 'functions.tsv')]
    kept = [f for f in functions if f not in jumpers | callers]
    (out / 'offsets.txt').write_text(''.join(f'{f:#x}\n' for f in kept))
    print(f'{prx.name}: {len(functions)} functions, {len(functions) - len(kept)} left to the translator '
          f'({len(jumpers)} that jump between stack frames, {len(callers - jumpers)} that call setjmp)')


main()
