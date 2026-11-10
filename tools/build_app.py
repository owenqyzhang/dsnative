"""Package a DS game as an Apple app: recompile it, build the game and the runtime as a static
library (libdsn.a) for the platform, build the Swift app (app/) against it and, for the Mac,
assemble the .app bundle.

    python3 tools/build_app.py game.nds -o OUTDIR [--platform macos|ios|ios-sim|tvos|tvos-sim]
                               [--name NAME] [--bundle-id ID] [--save-type TYPE]
                               [--extra missing.txt] [--jobs N] [--opt O2] [--debug] [--xcode]
                               [--run [--device NAME] [--screenshot PNG]]

Writes OUTDIR/<name>.app (macOS) or OUTDIR/<name>-<platform>.app, with the ROM and the
ARM9 image as the cartridge stores it that the recompiler writes (arm9.bin) in its resources. The generated C is
in OUTDIR/game, each platform's objects and libdsn.a in OUTDIR/game-<platform>, SwiftPM's build
in OUTDIR/swiftpm (macOS) or Xcode's in OUTDIR/xcode-build-<platform>, with the project
XcodeGen made from app/project.yml in OUTDIR/xcode. The bundle holds your ROM: build it
locally, never commit it or pass it on.

macOS needs the Command Line Tools or Xcode (Swift 6, the macOS SDK). The Metal compiler is
used when it is there (Xcode: the Metal Toolchain component); the app falls back to compiling
its two small shaders at run time when it isn't. iOS and tvOS need Xcode (their SDKs, and
xcodebuild) and XcodeGen (`brew install xcodegen`). Nothing is signed (CODE_SIGNING_ALLOWED=NO):
the simulator runs the result as it is; for a device, open OUTDIR/xcode/DSNative.xcodeproj in
Xcode and pick your own team under Signing & Capabilities. See app/README.md.
"""
import argparse
import filecmp
import os
import platform
import plistlib
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, 'app')
SHADER = os.path.join(APP, 'Sources', 'DSNKit', 'Shaders', 'screen.metal')
sys.path.insert(0, os.path.join(ROOT, 'tools'))
from nds import Rom, RomError     # noqa: E402

# Deployment targets: as app/Package.swift and app/project.yml (tests/test_app.py checks).
MIN_MACOS = '14.0'
MIN_IOS = '17.0'
MIN_TVOS = '17.0'
EXECUTABLE = 'DSNative'
HOST_ARCH = platform.machine()      # arm64 on Apple silicon


class Platform:
    """How one platform is built: the SDK and minimum-version flag its C is compiled with, the
    architecture, and the Xcode target, destination and products for the platforms Xcode builds."""

    def __init__(self, name, sdk, arch, min_flag, target=None, destination=None):
        self.name, self.sdk, self.arch, self.min_flag = name, sdk, arch, min_flag
        self.target, self.destination = target, destination

    @property
    def cc(self):
        # the Mac's own compiler for the host architecture; the others cross-compile through xcrun
        if self.name == 'macos':
            return 'cc'
        return f'xcrun -sdk {self.sdk} clang -arch {self.arch}'

    def cflags(self, opt='O2'):
        return f'-{opt} {self.min_flag}'

    @property
    def simulator(self):
        return self.sdk.endswith('simulator')

    @property
    def family(self):
        return {'macosx': 'macOS', 'iphoneos': 'iOS', 'iphonesimulator': 'iOS',
                'appletvos': 'tvOS', 'appletvsimulator': 'tvOS'}[self.sdk]


# libdsn.a is arm64: the simulators run natively on Apple silicon, and no Intel device exists.
PLATFORMS = {
    'macos': Platform('macos', 'macosx', HOST_ARCH, f'-mmacosx-version-min={MIN_MACOS}',
                      'DSNative-macOS', 'generic/platform=macOS'),
    'ios': Platform('ios', 'iphoneos', 'arm64', f'-miphoneos-version-min={MIN_IOS}',
                    'DSNative-iOS', 'generic/platform=iOS'),
    'ios-sim': Platform('ios-sim', 'iphonesimulator', 'arm64', f'-mios-simulator-version-min={MIN_IOS}',
                        'DSNative-iOS', 'generic/platform=iOS Simulator'),
    'tvos': Platform('tvos', 'appletvos', 'arm64', f'-mtvos-version-min={MIN_TVOS}',
                     'DSNative-tvOS', 'generic/platform=tvOS'),
    'tvos-sim': Platform('tvos-sim', 'appletvsimulator', 'arm64', f'-mtvos-simulator-version-min={MIN_TVOS}',
                         'DSNative-tvOS', 'generic/platform=tvOS Simulator'),
}


def run(cmd, **kw):
    print('+ ' + ' '.join(cmd), flush=True)
    subprocess.run(cmd, check=True, **kw)


def recompile(rom, out, extra):
    """The game as C and arm9.bin, in out/game (every platform builds from it). The recompiler
    writes a fresh copy beside it, and only files whose bytes changed replace the old ones: its
    output is deterministic, and make would otherwise recompile hundreds of megabytes of C for a
    rerun that changed nothing."""
    game = os.path.join(out, 'game')
    fresh = os.path.join(out, 'game.new')
    shutil.rmtree(fresh, ignore_errors=True)
    cmd = [sys.executable, '-m', 'recomp', os.path.abspath(rom), '-o', fresh]
    if extra:
        cmd += ['--extra', os.path.abspath(extra)]
    run(cmd, cwd=ROOT)
    kept = set()
    for dirpath, _, names in os.walk(fresh):
        for name in names:
            rel = os.path.relpath(os.path.join(dirpath, name), fresh)
            kept.add(rel)
            old, new = os.path.join(game, rel), os.path.join(fresh, rel)
            if not os.path.exists(old) or not filecmp.cmp(old, new, shallow=False):
                os.makedirs(os.path.dirname(old), exist_ok=True)
                shutil.copyfile(new, old)
    gen = os.path.join(game, 'gen')
    for name in os.listdir(gen) if os.path.isdir(gen) else []:
        if name.endswith('.c') and os.path.join('gen', name) not in kept:
            os.remove(os.path.join(gen, name))
    shutil.rmtree(fresh)
    return game


def build_library(game, plat, jobs, opt='O2'):
    """libdsn.a for one platform. The generated Makefile leaves its objects next to the sources
    and doesn't notice a change of compiler or flags, so each platform builds in a directory of
    its own (the Makefile, and a symbolic link to every generated source), and the objects go
    when the flags they were built with change."""
    work = f'{game}-{plat.name}'
    os.makedirs(os.path.join(work, 'gen'), exist_ok=True)
    flags = f'{plat.cc} {plat.cflags(opt)}'
    stamp = os.path.join(work, 'flags.txt')
    if not os.path.exists(stamp) or open(stamp).read() != flags:
        for dirpath in (work, os.path.join(work, 'gen')):
            for f in os.listdir(dirpath):
                if f.endswith(('.o', '.a')):
                    os.remove(os.path.join(dirpath, f))
        with open(stamp, 'w') as f:
            f.write(flags)
    shutil.copyfile(os.path.join(game, 'Makefile'), os.path.join(work, 'Makefile'))
    shutil.copyfile(os.path.join(game, 'arm9.bin'), os.path.join(work, 'arm9.bin'))
    sources = os.listdir(os.path.join(game, 'gen'))
    for stale in os.listdir(os.path.join(work, 'gen')):
        if stale.endswith('.c') and stale not in sources:
            os.remove(os.path.join(work, 'gen', stale))
    for name in sources:
        if name.endswith('.c'):
            link = os.path.join(work, 'gen', name)
            if not os.path.islink(link):
                os.symlink(os.path.join(game, 'gen', name), link)
    # CC and CFLAGS as one make argument each: `make CC="..."` keeps the spaces
    run(['make', '-C', work, f'-j{jobs}', '--no-print-directory', 'libdsn.a',
         f'CC={plat.cc}', f'CFLAGS={plat.cflags(opt)}', 'AR=xcrun ar'])
    return work


def metallib(out, plat):
    """default.metallib from the screen shader, or None without a Metal compiler."""
    air, lib = os.path.join(out, 'screen.air'), os.path.join(out, 'default.metallib')
    try:
        subprocess.run(['xcrun', '-sdk', plat.sdk, 'metal', '-c', '-o', air, plat.min_flag, SHADER],
                       check=True, capture_output=True)
        subprocess.run(['xcrun', '-sdk', plat.sdk, 'metallib', '-o', lib, air], check=True, capture_output=True)
    except (subprocess.CalledProcessError, OSError) as e:
        err = getattr(e, 'stderr', None) or b''
        detail = err.decode(errors='replace').strip().splitlines()[-1:]
        print(f'note: no precompiled shaders ({" ".join(detail) or e}); the app will compile them at run time')
        return None
    return lib


def swift_build_args(libdir, scratch, debug):
    # SwiftPM's default build system needs Xcode's; with only the Command Line Tools the
    # (deprecated) native one is the one that starts.
    args = ['swift', 'build', '-c', 'debug' if debug else 'release', '--package-path', APP, '--scratch-path', scratch,
            '--product', EXECUTABLE, '-Xlinker', '-L' + libdir]
    dev = subprocess.run(['xcode-select', '-p'], capture_output=True, text=True).stdout.strip()
    if dev.endswith('CommandLineTools'):
        args += ['--build-system', 'native']
    return args


def mac_info_plist(name, bundle_id, rom, save_type):
    plist = {
        'CFBundleName': name[:15],
        'CFBundleDisplayName': name,
        'CFBundleIdentifier': bundle_id,
        'CFBundleExecutable': EXECUTABLE,
        'CFBundlePackageType': 'APPL',
        'CFBundleInfoDictionaryVersion': '6.0',
        'CFBundleShortVersionString': '1.0',
        'CFBundleVersion': '1',
        'CFBundleDevelopmentRegion': 'en',
        'LSMinimumSystemVersion': MIN_MACOS,
        'LSApplicationCategoryType': 'public.app-category.games',
        'NSPrincipalClass': 'NSApplication',
        'NSHighResolutionCapable': True,
        'NSSupportsAutomaticGraphicsSwitching': True,
        'GCSupportsControllerUserInteraction': True,
        'DSNGameCode': rom.gamecode,
        'DSNGameTitle': rom.title,
    }
    if save_type:
        plist['DSNSaveType'] = save_type
    return plist


def build_macos(args, rom, name, bundle_id, out, libdir):
    scratch = os.path.join(out, 'swiftpm')
    build = swift_build_args(libdir, scratch, args.debug)
    # SwiftPM doesn't see libdsn.a change, so drop the old executable to relink
    bin_dir = subprocess.run(build + ['--show-bin-path'], check=True, capture_output=True, text=True).stdout.strip()
    exe = os.path.join(bin_dir, EXECUTABLE)
    if os.path.exists(exe):
        os.remove(exe)
    run(build + ['--jobs', str(args.jobs)])
    bundle = os.path.join(out, name + '.app')
    if os.path.exists(bundle):
        shutil.rmtree(bundle)
    contents = os.path.join(bundle, 'Contents')
    os.makedirs(os.path.join(contents, 'MacOS'))
    res = os.path.join(contents, 'Resources')
    os.makedirs(res)
    shutil.copy2(exe, os.path.join(contents, 'MacOS', EXECUTABLE))
    shutil.copyfile(args.rom, os.path.join(res, 'game.nds'))
    shutil.copyfile(os.path.join(libdir, 'arm9.bin'), os.path.join(res, 'arm9.bin'))
    lib = metallib(out, PLATFORMS['macos'])
    if lib:
        shutil.copyfile(lib, os.path.join(res, 'default.metallib'))
    with open(os.path.join(contents, 'Info.plist'), 'wb') as f:
        plistlib.dump(mac_info_plist(name, bundle_id, rom, args.save_type), f)
    with open(os.path.join(contents, 'PkgInfo'), 'w') as f:
        f.write('APPL????')
    # an ad hoc signature (no identity, no keychain): Apple silicon runs only signed code, and
    # it seals the resources
    if shutil.which('codesign'):
        run(['codesign', '--force', '--sign', '-', bundle])
    return bundle


def xcconfig_text(value):
    """A value safe in an xcconfig: it has no comment starter, no variable reference, no quote."""
    return re.sub(r'[^A-Za-z0-9 ._+-]', '_', value)


def write_xcconfig(path, out, game, rom, name, bundle_id, save_type):
    """The settings app/project.yml reads (the game, its names, where each SDK's libdsn.a is).
    The project builds against copies of the game's files, so the ROM's path (it may have
    spaces) never reaches Xcode."""
    libdirs = [p for p in PLATFORMS if os.path.exists(os.path.join(f'{game}-{p}', 'libdsn.a'))]
    lines = [
        f'DSN_GAME_DIR = {game}',
        f'DSN_ROM = {os.path.join(out, "game.nds")}',
        f'DSN_ARM9 = {os.path.join(game, "arm9.bin")}',
        f'DSN_APP_NAME = {xcconfig_text(name)}',
        f'DSN_PRODUCT_NAME = {xcconfig_text(name)}',
        f'DSN_BUNDLE_ID = {bundle_id}',
        f'DSN_GAME_CODE = {xcconfig_text(rom.gamecode)}',
        f'DSN_GAME_TITLE = {xcconfig_text(rom.title)}',
        f'DSN_SAVE_TYPE = {xcconfig_text(save_type or "")}',
    ]
    # each SDK finds its own libdsn.a (the ones built so far)
    for p in libdirs:
        lines.append(f'LIBRARY_SEARCH_PATHS[sdk={PLATFORMS[p].sdk}*] = "$(DSN_GAME_DIR)-{p}"')
    with open(path, 'w') as f:
        f.write('\n'.join(lines) + '\n')


def generate_project(project_dir, xcconfig):
    """XcodeGen's project from app/project.yml, in project_dir (DSNative.xcodeproj)."""
    if not shutil.which('xcodegen'):
        sys.exit('no xcodegen: brew install xcodegen')
    os.makedirs(project_dir, exist_ok=True)
    env = dict(os.environ, DSN_XCCONFIG=xcconfig, DSN_PACKAGE_PATH=APP)
    run(['xcodegen', 'generate', '--quiet', '--spec', os.path.join(APP, 'project.yml'), '--project', project_dir], env=env)


def build_xcode(args, rom, name, bundle_id, out, game, plat):
    """The iOS, tvOS (and, with --xcode, macOS) app: XcodeGen makes the project from
    app/project.yml and the xcconfig this writes, xcodebuild builds it. Returns the .app, copied
    to OUT/<name>-<platform>.app (OUT/<name>.app for the Mac)."""
    shutil.copyfile(args.rom, os.path.join(out, 'game.nds'))
    xcconfig = os.path.join(out, 'Game.xcconfig')
    write_xcconfig(xcconfig, out, game, rom, name, bundle_id, args.save_type)
    project_dir = os.path.join(out, 'xcode')
    generate_project(project_dir, xcconfig)

    derived = os.path.join(out, f'xcode-build-{plat.name}')
    config = 'Debug' if args.debug else 'Release'
    # Xcode adds the SDK to the products' folder name on every platform but the Mac
    folder = config if plat.sdk == 'macosx' else f'{config}-{plat.sdk}'
    # Xcode finds libdsn.a through -ldsn and doesn't know it is an input: drop the app target's
    # intermediate files so a rebuilt library is linked in (its few Swift files compile in seconds)
    shutil.rmtree(os.path.join(derived, 'Build', 'Intermediates.noindex', 'DSNative.build', folder, f'{plat.target}.build'),
                  ignore_errors=True)
    cmd = ['xcodebuild', '-project', os.path.join(project_dir, 'DSNative.xcodeproj'), '-scheme', plat.target,
           '-configuration', config, '-sdk', plat.sdk, '-destination', plat.destination, '-derivedDataPath', derived,
           '-jobs', str(args.jobs), 'build', 'CODE_SIGNING_ALLOWED=NO', 'CODE_SIGNING_REQUIRED=NO',
           f'ARCHS={plat.arch}', 'ONLY_ACTIVE_ARCH=NO']
    # xcodebuild is chatty: keep the log, show the end of it when it fails
    log = os.path.join(out, f'xcodebuild-{plat.name}.log')
    print('+ ' + ' '.join(cmd) + f'  (log: {log})', flush=True)
    with open(log, 'w') as f:
        r = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT)
    if r.returncode:
        tail = open(log, errors='replace').read().splitlines()
        errors = [l for l in tail if 'error:' in l][:20]
        print('\n'.join(errors or tail[-40:]))
        sys.exit(f'build_app.py: xcodebuild failed (exit status {r.returncode}); see {log}')
    built = os.path.join(derived, 'Build', 'Products', folder, name + '.app')
    if not os.path.isdir(built):
        sys.exit(f'build_app.py: xcodebuild left no {built}')
    bundle = os.path.join(out, f'{name}.app' if plat.name == 'macos' else f'{name}-{plat.name}.app')
    if os.path.exists(bundle):
        shutil.rmtree(bundle)
    shutil.copytree(built, bundle, symlinks=True)
    return bundle


def simulator_device(plat, wanted=None):
    """A device of the platform's family for `simctl`: the one whose name contains `wanted`, or
    one already booted, or the first of a runtime that is installed (iPhone, or Apple TV 4K).
    None when there is none."""
    import json
    out = subprocess.run(['xcrun', 'simctl', 'list', '-j', 'devices', 'available'], capture_output=True, text=True).stdout
    family = 'iOS' if plat.family == 'iOS' else 'tvOS'
    candidates = []
    for runtime, devices in json.loads(out)['devices'].items():
        if f'.{family}-' not in runtime:
            continue
        for d in devices:
            if wanted:
                want = wanted.lower() in d['name'].lower()
            else:
                want = d['name'].startswith('iPhone') if family == 'iOS' else d['name'].startswith('Apple TV')
            if want and 'Duo' not in d['name']:
                candidates.append(d)
    candidates.sort(key=lambda d: (d['state'] != 'Booted', d['name']))
    return candidates[0] if candidates else None


def run_in_simulator(bundle, bundle_id, plat, screenshot, seconds, wanted=None):
    """Boots a simulator device, installs and launches the app, waits, takes a screenshot (when
    asked for) and terminates it again."""
    import time
    dev = simulator_device(plat, wanted)
    if not dev:
        sys.exit(f'no {plat.family} simulator device{f" named {wanted}" if wanted else ""} (xcrun simctl list devices)')
    udid = dev['udid']
    print(f'simulator: {dev["name"]} ({udid})', flush=True)
    if dev['state'] != 'Booted':
        run(['xcrun', 'simctl', 'boot', udid])
    run(['xcrun', 'simctl', 'bootstatus', udid, '-b'], stdout=subprocess.DEVNULL)
    run(['xcrun', 'simctl', 'install', udid, bundle])
    run(['xcrun', 'simctl', 'launch', udid, bundle_id])
    time.sleep(seconds)
    if screenshot:
        run(['xcrun', 'simctl', 'io', udid, 'screenshot', screenshot])
    # an app that has already ended (it crashed) has nothing to terminate: say so
    r = subprocess.run(['xcrun', 'simctl', 'terminate', udid, bundle_id], capture_output=True, text=True)
    if r.returncode:
        sys.exit(f'{bundle_id} was no longer running when the simulator run ended (a crash? see ~/Library/Logs/DiagnosticReports)')


def main():
    ap = argparse.ArgumentParser(prog='python3 tools/build_app.py', description='Package a DS game as an Apple app.')
    ap.add_argument('rom')
    ap.add_argument('-o', '--out', required=True, help='output directory')
    ap.add_argument('--platform', default='macos', choices=sorted(PLATFORMS), help='what to build (default macos)')
    ap.add_argument('--name', help='the app\'s name (default: the ROM\'s title)')
    ap.add_argument('--bundle-id', help='bundle identifier (default: com.dsnative.game.<game code>); saves are kept by it')
    ap.add_argument('--save-type', help='the cartridge\'s save chip (the runtime\'s --save-type: flash512k, eeprom8k, ...), '
                                        'when the runtime can\'t tell from the game code')
    ap.add_argument('--extra', help='entry points to add (the runtime\'s missing.txt)')
    ap.add_argument('--jobs', type=int, default=4, help='parallel C compiles (default 4)')
    ap.add_argument('--opt', default='O2', choices=['O0', 'O1', 'O2', 'Os'],
                    help='optimization of the generated C (default O2; a big game compiles much faster at O1 or O0 '
                         'for a first look, and the library is smaller at Os)')
    ap.add_argument('--debug', action='store_true', help='debug Swift build')
    ap.add_argument('--xcode', action='store_true', help='macOS: build the Xcode project\'s target instead of the SwiftPM executable '
                                                         '(iOS and tvOS always use it)')
    ap.add_argument('--run', action='store_true', help='simulator platforms: boot a simulator, install and launch the app, '
                                                       'then terminate it')
    ap.add_argument('--device', metavar='NAME', help='with --run: the simulator device, by part of its name (default: an iPhone, or the Apple TV)')
    ap.add_argument('--screenshot', metavar='PNG', help='with --run: save the simulator screen to PNG before terminating')
    ap.add_argument('--run-seconds', type=float, default=8, help='with --run: how long the app runs before the screenshot (default 8)')
    args = ap.parse_args()
    if sys.platform != 'darwin':
        sys.exit('build_app.py builds Apple apps: run it on a Mac')
    if not shutil.which('swift'):
        sys.exit('no swift: install the Command Line Tools (xcode-select --install) or Xcode')
    try:
        rom = Rom.load(args.rom)
    except (OSError, RomError) as e:
        sys.exit(f'{args.rom}: {e}')

    name = args.name or rom.title.strip() or rom.gamecode.strip() or 'DS Game'
    name = re.sub(r'[^A-Za-z0-9 ._+-]', '_', name)      # a file name that is safe for xcconfig and the shell too
    code = re.sub(r'[^A-Za-z0-9]', '', rom.gamecode).lower() or 'unknown'
    bundle_id = args.bundle_id or f'com.dsnative.game.{code}'
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    plat = PLATFORMS[args.platform]

    if args.run and not plat.simulator:
        sys.exit('--run needs a simulator platform (ios-sim or tvos-sim)')
    if args.screenshot and not args.run:
        sys.exit('--screenshot goes with --run')
    if plat.sdk != 'macosx' or args.xcode:
        if not shutil.which('xcodebuild'):
            sys.exit('no xcodebuild: the iOS and tvOS apps need Xcode')

    game = recompile(args.rom, out, args.extra)
    libdir = build_library(game, plat, args.jobs, args.opt)
    if plat.name == 'macos' and not args.xcode:
        bundle = build_macos(args, rom, name, bundle_id, out, libdir)
    else:
        bundle = build_xcode(args, rom, name, bundle_id, out, game, plat)
    print(f'built {bundle}')
    if args.run:
        run_in_simulator(bundle, bundle_id, plat, args.screenshot, args.run_seconds, args.device)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except subprocess.CalledProcessError as e:
        sys.exit(f'build_app.py: {e.cmd[0]} failed (exit status {e.returncode})')
