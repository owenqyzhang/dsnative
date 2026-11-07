"""Save memory across runs: the homebrew test ROM (tests/hw/save_9.c) run headless several
times on one save file, as a player quits and comes back. Checks that the file appears at
shutdown holding what the game wrote, that the next run reads it back (save_9's "savein"
line), that what the game didn't touch survives, and that a run writing nothing leaves the
file alone.

    python3 tests/test_save.py
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, 'build', 'savetest')
SIZE = 0x80000          # --save-type flash512k
PAGE = 0x7F00           # what save_9.c erases and writes


def fnv(data):
    h = 0x811C9DC5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return f'{h:08x}'


def main():
    subprocess.run([sys.executable, os.path.join(HERE, 'hw', 'build.py'), OUT], check=True, stdout=subprocess.DEVNULL)
    rom, gen = os.path.join(OUT, 'test.nds'), os.path.join(OUT, 'out')
    subprocess.run([sys.executable, '-m', 'recomp', rom, '-o', gen, '--build'], cwd=ROOT, check=True,
                   stdout=subprocess.DEVNULL)
    expect = {}
    for line in open(os.path.join(HERE, 'hw', 'expect_save.txt')).read().splitlines():
        if line.strip() and not line.startswith(('#', 'args:')):
            expect[line.split()[0]] = line
    save = os.path.join(OUT, 'test.sav')
    failures = []

    def check(cond, what):
        if not cond:
            failures.append(what)

    def run(frames=100):
        res = subprocess.run([os.path.join(gen, 'dsn_headless'), rom, os.path.join(gen, 'arm9.bin'), '--frames', str(frames),
                              '--save-type', 'flash512k', '--save', save], cwd=gen, capture_output=True, text=True)
        check(res.returncode == 0, f'dsn_headless exited with {res.returncode}: {res.stderr.strip()}')
        lines = [l[len('[debug] '):] for l in res.stdout.splitlines() if l.startswith('[debug] ')]
        return {l.split()[0]: l for l in lines if l.split()}

    def contents():
        return open(save, 'rb').read() if os.path.exists(save) else b''

    page = bytearray((i * 37 + 11) & 0xFF for i in range(256))
    for i in range(0x10, 0x20):
        page[i] &= 0x0F

    # 1: no save file yet: the chip starts erased, and the file appears at shutdown
    if os.path.exists(save):
        os.remove(save)
    got = run()
    for k, v in expect.items():
        check(got.get(k) == v, f'run 1: expected "{v}", got "{got.get(k)}"')
    erased = fnv(b'\xff' * 256)
    check(got.get('savein') == f'savein {erased}', f'run 1 did not start erased: "{got.get("savein")}"')
    data = contents()
    check(len(data) == SIZE, f'run 1: save file holds {len(data)} bytes')
    check(data[PAGE:PAGE + 256] == page, 'run 1: the page in the file is not what the game wrote')
    check(data[:PAGE] == b'\xff' * PAGE and data[PAGE + 256:] == b'\xff' * (SIZE - PAGE - 256),
          'run 1: the file changed outside the page')
    check(not os.path.exists(save + '.tmp'), 'run 1 left the temporary file')

    # 2: the next run reads the file: its page as savein, and keeps what it doesn't write
    old = bytes((i * 91 + 5) & 0xFF for i in range(256))
    data = bytearray(data)
    data[PAGE:PAGE + 256] = old
    data[0x100:0x108] = b'dsnative'
    data[SIZE - 8:] = b'lastpage'
    open(save, 'wb').write(data)
    got = run()
    check(got.get('savein') == f'savein {fnv(old)}', f'run 2 did not read the file: "{got.get("savein")}", want {fnv(old)}')
    check(got.get('save') == expect['save'], f'run 2: "{got.get("save")}"')
    data = contents()
    check(len(data) == SIZE and data[PAGE:PAGE + 256] == page, 'run 2: the page in the file is not what the game wrote')
    check(data[0x100:0x108] == b'dsnative' and data[SIZE - 8:] == b'lastpage', 'run 2 lost what it did not write')

    # 3: a run that stops before the save test writes nothing, so the file stays as it was
    os.utime(save, (1000000000, 1000000000))
    got = run(frames=50)
    check('save' not in got, 'run 3 reached the save test')
    check(contents() == data and int(os.stat(save).st_mtime) == 1000000000, 'run 3 wrote the file back')

    if failures:
        print('FAILED:\n  ' + '\n  '.join(failures))
        return 1
    print('save test passed: the save file persists across runs')
    return 0


if __name__ == '__main__':
    sys.exit(main())
