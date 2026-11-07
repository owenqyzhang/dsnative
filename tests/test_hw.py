"""End-to-end test: build the homebrew test ROM, recompile it, run it headless and check
its debug output against the host's run of the same suite and the expected hardware
results; writes the screens to build/hwtest/screen.png.

    python3 tests/test_hw.py
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, 'build', 'hwtest')


def main():
    sys.path.insert(0, os.path.join(HERE, 'hw'))
    import build as hwbuild
    rom = hwbuild.main()
    gen = os.path.join(OUT, 'out')
    subprocess.run([sys.executable, '-m', 'recomp', rom, '-o', gen, '--build'], cwd=ROOT, check=True,
                   stdout=subprocess.DEVNULL)
    png = os.path.join(OUT, 'screen.png')
    res = subprocess.run([os.path.join(gen, 'dsn_headless'), rom, os.path.join(gen, 'arm9.bin'), '--frames', '100',
                          '--png', png, '--keys', '35:E,45:ED,55:', '--stats'],
                         cwd=gen, capture_output=True, text=True)
    lines = [l[len('[debug] '):] for l in res.stdout.splitlines() if l.startswith('[debug] ')]
    ref = open(os.path.join(OUT, 'host_ref.txt')).read().splitlines()
    failures = []
    got = {l.split()[0]: l for l in lines if l.split()}
    for r in ref:
        k = r.split()[0]
        if got.get(k) != r:
            failures.append(f'suite: expected "{r}", got "{got.get(k)}"')
    expect = {
        'hwmath': 'hwmath ok 00000000',
        'card': 'card Hello from the cartridge!',
        'ipc': 'ipc 00000004 00000007 0000000a 0000000d 00000010',
        'sprite': 'sprite 000000a0 0000006c',       # right held 20 frames, down 10: (120, 88) + (40, 20)
        'DONE': 'DONE',
    }
    for k, v in expect.items():
        if got.get(k) != v:
            failures.append(f'expected "{v}", got "{got.get(k)}"')
    if not got.get('timer', '').startswith('timer ok'):
        failures.append(f'timer: {got.get("timer")}')
    h = 0x811C9DC5      # itcm_hash over i * 0x9E3779B9, computed here
    for i in range(64):
        h = ((h ^ ((i * 0x9E3779B9) & 0xFFFFFFFF)) * 0x01000193) & 0xFFFFFFFF
    if got.get('itcm') != f'itcm {h:08x}':
        failures.append(f'expected "itcm {h:08x}", got "{got.get("itcm")}"')
    print('\n'.join(lines))
    print(res.stderr.strip())
    if failures:
        print('FAILED:\n  ' + '\n  '.join(failures))
        return 1
    print(f'hardware test passed; screens in {png}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
