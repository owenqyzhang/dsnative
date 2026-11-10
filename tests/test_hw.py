"""End-to-end test: build the homebrew test ROM, recompile it, run it headless and check
its debug output against the host's run of the same suite and the expected hardware
results, then runs it again with every instruction interpreted and expects the same output;
writes the screens to build/hwtest/screen.png.

    python3 tests/test_hw.py
"""
import glob
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, 'build', 'hwtest')


def fresh_save():
    """Both runs start from an erased save chip: each one's savein must match, and the compiled
    run's shutdown write must not reach the interpreted run. The save file's persistence across
    runs is test_save.py's job."""
    for name in ('save.sav', 'save.sav.tmp'):
        try:
            os.remove(os.path.join(OUT, name))
        except FileNotFoundError:
            pass


def main():
    sys.path.insert(0, os.path.join(HERE, 'hw'))
    import build as hwbuild
    rom = hwbuild.main()
    gen = os.path.join(OUT, 'out')
    subprocess.run([sys.executable, '-m', 'recomp', rom, '-o', gen, '--build'], cwd=ROOT, check=True,
                   stdout=subprocess.DEVNULL)
    # the machine boots the ARM9 image as the cartridge stores it (BLZ-compressed here): the test
    # program's own crt0 decompresses it, as a game's does, and a reload from the cartridge restarts it
    sys.path.insert(0, os.path.join(ROOT, 'tools'))
    from nds import Rom
    stored = Rom.load(rom).arm9_stored
    assert open(os.path.join(gen, 'arm9.bin'), 'rb').read() == stored, 'arm9.bin is not the cartridge\'s ARM9 image'
    png = os.path.join(OUT, 'screen.png')
    # Each subsystem's tests/hw/expect_<name>.txt: "args: ..." lines add headless options, the
    # other lines (not starting with #) are debug lines expected verbatim, keyed by first word.
    extra_args, extra_lines = [], []
    for path in sorted(glob.glob(os.path.join(HERE, 'hw', 'expect_*.txt'))):
        for line in open(path).read().splitlines():
            if line.startswith('args:'):
                extra_args += line[5:].split()
            elif line.strip() and not line.startswith('#'):
                extra_lines.append(line)
    fresh_save()
    res = subprocess.run([os.path.join(gen, 'dsn_headless'), rom, os.path.join(gen, 'arm9.bin'), '--frames', '100',
                          '--png', png, '--keys', '35:E,45:ED,55:', '--stats', '--no-interp', *extra_args],
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
    for line in extra_lines:
        expect[line.split()[0]] = line
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
    # the same ROM with every instruction interpreted (no compiled code on either CPU) must say the same
    # (the timer test counts cycles, which compiled code charges a block at a time: it just has to pass)
    fresh_save()
    ires = subprocess.run([os.path.join(gen, 'dsn_headless'), rom, os.path.join(gen, 'arm9.bin'), '--frames', '100',
                           '--png', os.path.join(OUT, 'screen_interp.png'), '--keys', '35:E,45:ED,55:', '--stats',
                           '--interp-only', *extra_args], cwd=gen, capture_output=True, text=True)
    igot = {l.split()[0]: l for l in (l[len('[debug] '):] for l in ires.stdout.splitlines() if l.startswith('[debug] ')) if l.split()}
    for k, v in got.items():
        if k == 'timer':
            if not igot.get(k, '').startswith('timer ok'):
                failures.append(f'interpreted: timer: {igot.get(k)}')
        elif igot.get(k) != v:
            failures.append(f'interpreted: expected "{v}", got "{igot.get(k)}"')
    for k in igot:
        if k not in got:
            failures.append(f'interpreted: unexpected "{igot[k]}"')
    if 'ARM9 0 instructions' in ires.stderr or 'ARM7 0 instructions' in ires.stderr or ires.returncode:
        failures.append(f'interpreted run: {ires.stderr.strip().splitlines()[-2:]}')
    print('\n'.join(lines))
    print(res.stderr.strip())
    if failures:
        print('FAILED:\n  ' + '\n  '.join(failures))
        return 1
    print(f'hardware test passed; screens in {png}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
