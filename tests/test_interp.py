"""Self-modifying and never-compiled code, end to end: tests/interp/selfmod.s is assembled,
its static part recompiled (the function the program patches in an overlay-kind segment, as
code that several overlays share is: the dispatcher checks its bytes), and run with the
runtime's interpreter. It covers

  - a compiled function patched at run time and called from compiled code: as compiled, with
    its head replaced (interpreted, back into compiled code through its return), restored
    (compiled again), and with part of its head replaced (the interpreter falls into the
    compiled loop);
  - ARM and Thumb functions copied to RAM, which were never compiled, calling compiled code;
  - compiled and interpreted code calling each other four deep;
  - code written and executed twice at one address;
  - the interpreter's instruction count, the log of missing addresses (each once), and
    --no-interp, which stops at the patched function as the dispatcher used to.

    python3 tests/test_interp.py
Needs arm-none-eabi-gcc.
"""
import os
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, 'build', 'selfmod')
sys.path.insert(0, ROOT)
from recomp.program import Program      # noqa: E402

BASE = 0x02000000


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw)


def f_rec(n):   # the two functions of the program that call each other
    return 100 if n == 0 else g_rec(n - 1) + 1


def g_rec(n):
    return 200 if n == 0 else f_rec(n - 1) + 2


EXPECT = [56, 0xAA, 56, 58, 2 * 55 + 1, 4 * 5 * 2 + 1 + 7, f_rec(4), 0xAA, 0xBB]


def main():
    os.makedirs(OUT, exist_ok=True)
    src = os.path.join(HERE, 'interp')
    obj, elf, binf = (os.path.join(OUT, 'selfmod' + e) for e in ('.o', '.elf', '.bin'))
    run(['arm-none-eabi-gcc', '-mcpu=arm946e-s', '-x', 'assembler', '-c', os.path.join(src, 'selfmod.s'), '-o', obj])
    run(['arm-none-eabi-ld', '-T', os.path.join(src, 'selfmod.ld'), '-o', elf, obj])
    run(['arm-none-eabi-objcopy', '-O', 'binary', elf, binf])
    syms = {}
    for line in run(['arm-none-eabi-nm', elf]).stdout.splitlines():
        p = line.split()
        syms[p[2]] = int(p[0], 16)
    image = open(binf, 'rb').read()
    # the static code is compiled; the patched function sits in a segment that is checked at
    # dispatch; the code copied to RAM is data as far as the recompiler can tell
    segs = [('main', BASE, image[:0x1000], 'static'), ('ovl', BASE + 0x1000, image[0x1000:0x1800], 'overlay')]
    prog = Program(segs, arch=5, prefix='g9')
    prog.discover([(BASE, False), (syms['helper_rec'], False)])
    gen = os.path.join(OUT, 'gen')
    prog.emit(gen)
    srcs = [os.path.join(gen, f) for f in sorted(os.listdir(gen)) if f.endswith('.c')]
    exe = os.path.join(OUT, 'selfmod')
    rt = os.path.join(ROOT, 'runtime')
    run(['cc', '-O1', '-w', '-I', rt, '-o', exe, os.path.join(src, 'selfmod_main.c'), os.path.join(rt, 'cpu.c'),
         os.path.join(rt, 'interp.c')] + srcs)
    args = [exe, binf, f'{syms["done"]:x}', '2100000']
    failures = []

    def check(out):
        got = {l.split()[0]: l.split()[1:] for l in out.splitlines()}
        return got

    got = check(run(args).stdout)
    results = [int(x, 16) for x in got['results']]
    if results != EXPECT:
        failures.append(f'results {[hex(x) for x in results]}, expected {[hex(x) for x in EXPECT]}')
    calls, insns = int(got['counters'][0]), int(got['counters'][1])
    # interpreted instructions: the patched head (2), the head with its second halfword replaced (3), ARM RAM
    # function (3 + 10 * 3 + 3 + 1), Thumb (3 + 2), f four deep (2 * 9 + 5), two written functions (2 * 2)
    want_insns = 2 + 3 + (3 + 30 + 3 + 1) + 5 + (2 * 9 + 5) + 4
    # entries: one for each patched call, two for each RAM function that calls out and is returned to, five for
    # f(4), f(2) and f(0) with the returns into f(4) and f(2), two for the written code
    want_calls = 1 + 1 + 2 + 2 + 5 + 2
    if insns != want_insns:
        failures.append(f'interpreted {insns} instructions, expected {want_insns}')
    if calls != want_calls:
        failures.append(f'{calls} entries into the interpreter, expected {want_calls}')
    missing = [(l.split()[1], l.split()[2]) for l in run(args).stdout.splitlines() if l.startswith('missing')]
    # each address the dispatcher handed to the interpreter, once: the patched function, the copies in RAM
    # and the places in them that compiled code returns to (after the call to helper_sum or helper_rec)
    want_missing = sorted([(f'{syms["target"] & ~1:08x}', 't'), ('02003000', 'a'), ('02003024', 'a'), ('02003100', 't'),
                           ('02003106', 't'), ('02003200', 'a'), ('0200321c', 'a'), ('02003300', 't')])
    if sorted(missing) != want_missing:
        failures.append(f'logged {sorted(missing)}, expected each of {want_missing} once')
    # without the interpreter: stops where the first patched call lands, as the dispatcher always did
    noint = run(args + ['--no-interp']).stdout
    got2 = check(noint)
    r2 = [int(x, 16) for x in got2['results']]
    if r2[0] != 56 or r2[1] != 0 or got2['counters'][4] != '1' or int(got2['counters'][1]) != 0 \
            or int(got2['counters'][6], 16) != syms['target'] & ~1:
        failures.append(f'--no-interp: {got2}')
    if failures:
        print('FAILED:\n  ' + '\n  '.join(failures))
        return 1
    print(f'self-modification test passed: {insns} instructions interpreted in {calls} entries, '
          f'results {[hex(x) for x in results]}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
