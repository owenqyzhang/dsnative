"""C unit tests of the runtime: each tests/unit/test_*.c is compiled with the runtime (no
generated code) and run; it prints what it checked and exits non-zero on a failure.

    python3 tests/test_units.py [name ...]

DSN_SANITIZE=1 builds them with AddressSanitizer and UndefinedBehaviorSanitizer (shift checks on),
stopping at the first finding.
"""
import glob
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
RT = os.path.join(ROOT, 'runtime')
OUT = os.path.join(ROOT, 'build', 'unit')


def main():
    os.makedirs(OUT, exist_ok=True)
    runtime = sorted(f for f in glob.glob(os.path.join(RT, '*.c')) if not os.path.basename(f).startswith('main_'))
    tests = sorted(glob.glob(os.path.join(HERE, 'unit', 'test_*.c')))
    if len(sys.argv) > 1:
        tests = [t for t in tests if os.path.basename(t)[5:-2] in sys.argv[1:]]
    failed = 0
    for t in tests:
        name = os.path.basename(t)[:-2]
        exe = os.path.join(OUT, name)
        san = ['-fsanitize=undefined,address', '-fno-sanitize-recover=all'] if os.environ.get('DSN_SANITIZE') else []
        b = subprocess.run(['cc', '-O1', '-g', '-Wall', '-Wno-unused-function', *san, '-I', RT, '-o', exe, t] + runtime + ['-lm'],
                           capture_output=True, text=True)
        if b.returncode:
            failed += 1
            print(f'FAIL  {name}: does not build\n{b.stderr}')
            continue
        r = subprocess.run([exe], capture_output=True, text=True, cwd=OUT)
        out = (r.stdout + r.stderr).strip()
        print(f'{"ok   " if r.returncode == 0 else "FAIL "} {name}' + (f'\n      ' + out.replace('\n', '\n      ') if out and r.returncode else ''))
        failed += r.returncode != 0
    print(f'{len(tests) - failed}/{len(tests)} unit tests passed')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
