"""The Mac app, end to end on the homebrew test ROM: build_app.py makes the .app, the bundle runs
its own game headless (--smoke) and draws the same two screens dsn_headless does, the app's
save file and clock options work, and the Swift package's tests pass (screen layout, input
mapping, the on-screen pad's geometry, the Metal renderer on the default device, the paced
emulation loop). The iOS and tvOS side is checked as far as it goes without Xcode builds or
simulators: the deployment targets of build_app.py, Package.swift and project.yml agree, and
XcodeGen turns project.yml into a project with the three apps (when xcodegen is installed).
`python3 tools/build_app.py ROM -o OUT --platform ios-sim --run` is the simulator check.

    python3 tests/test_app.py

Skipped (exit 0, with a message) where Swift or macOS aren't there. Doesn't need a simulator.
"""
import os
import plistlib
import re
import shutil
import struct
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, 'build', 'apptest')
KEYS = '35:E,45:ED,55:'     # as test_hw.py: right held 10 frames, then right and down
SAVE_SIZE = 0x80000         # --save-type flash512k, which tests/hw/save_9.c writes
OPTIONS = ['--save-type', 'flash512k', '--rtc', '2026-11-07T12:34:56']


def read_png(path):
    """(width, height, pixels as 0xRRGGBB words) of a non-interlaced 8-bit RGB or RGBA PNG."""
    data = open(path, 'rb').read()
    assert data[:8] == b'\x89PNG\r\n\x1a\n', 'not a PNG'
    pos, idat, ihdr = 8, b'', None
    while pos < len(data):
        n, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b'IHDR':
            ihdr = struct.unpack('>IIBBBBB', body)
        elif kind == b'IDAT':
            idat += body
        pos += 12 + n
    w, h, depth, ctype, _, _, interlace = ihdr
    assert depth == 8 and ctype in (2, 6) and interlace == 0, f'unsupported PNG {ihdr}'
    bpp = 3 if ctype == 2 else 4
    raw, stride = zlib.decompress(idat), w * bpp
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    px = [line[x * bpp] << 16 | line[x * bpp + 1] << 8 | line[x * bpp + 2] for line in rows for x in range(w)]
    return w, h, px


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def check_projects(check, rom):
    """build_app.py, Package.swift and project.yml name the same deployment targets, and XcodeGen
    makes a project with the macOS, iOS and tvOS apps from project.yml and the xcconfig."""
    sys.path.insert(0, os.path.join(ROOT, 'tools'))
    import build_app
    from nds import Rom
    package = open(os.path.join(ROOT, 'app', 'Package.swift')).read()
    spec = open(os.path.join(ROOT, 'app', 'project.yml')).read()
    for family, swift, minimum in (('macOS', 'macOS', build_app.MIN_MACOS), ('iOS', 'iOS', build_app.MIN_IOS),
                                   ('tvOS', 'tvOS', build_app.MIN_TVOS)):
        major = minimum.split('.')[0]
        check(re.search(rf'\.{swift}\(\.v{major}\)', package) is not None, f'Package.swift: {family} is not {minimum}')
        check(re.search(rf'^\s+{family}: "{re.escape(minimum)}"', spec, re.M) is not None, f'project.yml: {family} is not {minimum}')
    for plat in build_app.PLATFORMS.values():
        flag = plat.min_flag.split('=')[1]
        check(flag == {'macOS': build_app.MIN_MACOS, 'iOS': build_app.MIN_IOS, 'tvOS': build_app.MIN_TVOS}[plat.family],
              f'{plat.name}: C compiled for {flag}')
    if not shutil.which('xcodegen'):
        print('note: xcodegen is not installed, so project.yml was not generated (brew install xcodegen)')
        return
    out = os.path.join(OUT, 'xcode-check')
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    xcconfig = os.path.join(out, 'Game.xcconfig')
    build_app.write_xcconfig(xcconfig, out, os.path.join(out, 'game'), Rom.load(rom), 'DSNTest', 'com.dsnative.game.dsnt', 'flash512k')
    build_app.generate_project(os.path.join(out, 'xcode'), xcconfig)
    pbx = os.path.join(out, 'xcode', 'DSNative.xcodeproj', 'project.pbxproj')
    text = open(pbx).read() if os.path.exists(pbx) else ''
    for target in ('DSNative-macOS', 'DSNative-iOS', 'DSNative-tvOS'):
        check(f'name = "{target}";' in text or f'name = {target};' in text, f'the generated project has no {target} target')
    check('Game.xcconfig' in text, 'the generated project does not use the xcconfig')


def main():
    if sys.platform != 'darwin':
        print('SKIP test_app.py: the app is for Apple platforms (this is not macOS)')
        return 0
    if not shutil.which('swift'):
        print('SKIP test_app.py: swift is not installed (Xcode or the Command Line Tools)')
        return 0
    failures = []

    def check(cond, what):
        if not cond:
            failures.append(what)

    subprocess.run([sys.executable, os.path.join(HERE, 'hw', 'build.py'), OUT], check=True, stdout=subprocess.DEVNULL)
    rom = os.path.join(OUT, 'test.nds')
    mac = os.path.join(OUT, 'mac')
    built = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'build_app.py'), rom, '-o', mac, '--name', 'DSNTest',
                            '--save-type', 'flash512k'], capture_output=True, text=True)
    if built.returncode:
        print(built.stdout[-3000:] + built.stderr[-3000:])
        print('FAILED: build_app.py')
        return 1
    bundle = os.path.join(mac, 'DSNTest.app')
    contents = os.path.join(bundle, 'Contents')
    exe = os.path.join(contents, 'MacOS', 'DSNative')
    for rel in ('Info.plist', 'MacOS/DSNative', 'Resources/game.nds', 'Resources/arm9.bin'):
        check(os.path.exists(os.path.join(contents, rel)), f'bundle lacks {rel}')
    if os.path.exists(os.path.join(contents, 'Info.plist')):
        info = plistlib.load(open(os.path.join(contents, 'Info.plist'), 'rb'))
        check(info.get('CFBundleExecutable') == 'DSNative' and info.get('DSNGameCode') == 'DSNT', f'Info.plist {info}')
    if 'no precompiled shaders' in built.stdout:
        print('note: no Metal compiler here, so the bundle has no default.metallib (the app compiles its shaders)')
    else:
        check(os.path.exists(os.path.join(contents, 'Resources', 'default.metallib')), 'bundle lacks default.metallib')

    # The bundle's own game, run without --rom, against dsn_headless on the same ROM: the same
    # frames. The save type comes from the bundle (Info.plist), the save file from --save.
    game = os.path.join(mac, 'game')
    make = run(['make', '-C', game, '-j4', '--no-print-directory', 'dsn_headless'])
    check(make.returncode == 0, 'dsn_headless did not build: ' + make.stderr[-500:])
    app_png, ref_png = os.path.join(OUT, 'app.png'), os.path.join(OUT, 'ref.png')
    save = os.path.join(OUT, 'app.sav')
    for f in (save, app_png, ref_png):
        if os.path.exists(f):
            os.remove(f)
    app = run([exe, '--smoke', '100', '--png', app_png, '--keys', KEYS, '--save', save, OPTIONS[2], OPTIONS[3]], cwd=OUT)
    ref = run([os.path.join(game, 'dsn_headless'), rom, os.path.join(game, 'arm9.bin'), '--frames', '100', '--png', ref_png,
              '--keys', KEYS, *OPTIONS], cwd=OUT)
    debug = lambda r: [l for l in r.stdout.splitlines() if l.startswith('[debug] ')]   # noqa: E731
    check(app.returncode == 0, f'smoke run exited {app.returncode}: {app.stderr[-500:]}')
    check(debug(app) == debug(ref) and 'DONE' in ''.join(debug(app)), 'smoke debug output differs from dsn_headless')
    check('[debug] sprite 000000a0 0000006c' in debug(app), 'the scripted keys did not move the sprite')
    check(os.path.exists(save) and os.path.getsize(save) == SAVE_SIZE, 'smoke run left no 512 KB save file')
    if os.path.exists(app_png) and os.path.exists(ref_png):
        w, h, px = read_png(app_png)
        check((w, h) == (256, 384), f'PNG is {w}x{h}, expected 256x384')
        top, bottom = px[:256 * 192], px[256 * 192:]
        check(len(set(top)) > 4 and len(set(bottom)) > 4, f'screens look empty: {len(set(top))} and {len(set(bottom))} colours')
        check(read_png(ref_png)[2] == px, 'the app\'s screens differ from dsn_headless\'s')
    else:
        failures.append('smoke run wrote no PNG')

    check_projects(check, rom)

    # the package's tests, with the game linked in (one test drives the emulation loop on it)
    env = dict(os.environ, DSN_TEST_ROM=rom, DSN_TEST_ARM9=os.path.join(game, 'arm9.bin'))
    libdir = os.path.join(mac, 'game-macos')
    t = run(['swift', 'test', '--package-path', os.path.join(ROOT, 'app'), '--scratch-path', os.path.join(OUT, 'swiftpm-test'),
             '-Xlinker', '-L' + libdir], env=env)
    check(t.returncode == 0, 'swift test failed:\n' + (t.stdout + t.stderr)[-4000:])
    summary = [l for l in (t.stdout + t.stderr).splitlines() if 'Test run with' in l]
    check(bool(summary) or t.returncode, 'swift test ran no tests')
    print(summary[-1][2:] if summary else '')

    if failures:
        print('FAILED:\n  ' + '\n  '.join(failures))
        return 1
    print(f'app test passed; {bundle}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
