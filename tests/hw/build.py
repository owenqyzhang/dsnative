"""Build the hardware test ROM (tests/hw/test.nds) and the host reference (host_ref.txt).

Needs arm-none-eabi-gcc. The ARM9 binary is BLZ-compressed and carries Nitro-style module
parameters with an ITCM autoload segment, as a commercial game's does.

    python3 tests/hw/build.py [out_dir]
"""
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
from nds import build_rom, blz_compress, Rom   # noqa: E402

CC = 'arm-none-eabi-gcc'
COMMON = ['-O2', '-ffreestanding', '-fno-strict-aliasing', '-mthumb-interwork', '-Wall', '-Wno-unused-function']


def run(cmd):
    subprocess.run(cmd, check=True)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'build', 'hwtest')
    os.makedirs(out, exist_ok=True)
    src = lambda f: os.path.join(HERE, f)   # noqa: E731
    e9, e7 = os.path.join(out, 'arm9.elf'), os.path.join(out, 'arm7.elf')
    run([CC, '-mcpu=arm946e-s', '-mthumb', *COMMON, '-nostartfiles', '-nostdlib', '-T', src('arm9.ld'),
         '-o', e9, src('crt0_9.s'), src('main9.c'), src('suite.c'), src('libc.c'), '-lgcc'])
    run([CC, '-mcpu=arm7tdmi', '-mthumb', *COMMON, '-nostartfiles', '-nostdlib', '-T', src('arm7.ld'),
         '-o', e7, src('crt0_7.s'), src('main7.c'), src('libc.c'), '-lgcc'])
    b9, b7 = os.path.join(out, 'arm9.bin'), os.path.join(out, 'arm7.bin')
    run(['arm-none-eabi-objcopy', '-O', 'binary', '-R', '.bss', e9, b9])
    run(['arm-none-eabi-objcopy', '-O', 'binary', '-R', '.bss', e7, b7])
    # objcopy writes the ITCM section at its load address after the static image
    arm9 = open(b9, 'rb').read()
    arm7 = open(b7, 'rb').read()
    syms = {}
    for line in subprocess.run(['arm-none-eabi-nm', e9], check=True, capture_output=True, text=True).stdout.splitlines():
        p = line.split()
        if len(p) == 3:
            syms[p[2]] = int(p[0], 16)
    keep = syms['_crt0_end'] - 0x02000000
    packed = blz_compress(arm9, keep)
    params = syms['_start_ModuleParams'] - 0x02000000
    b = bytearray(packed)
    struct.pack_into('<I', b, params + 0x14, 0x02000000 + len(packed))
    rom = build_rom(bytes(b), 0x02000000, 0x02000000, arm7, 0x02380000, 0x02380000,
                    title='DSNATIVETEST', gamecode='DSNT', files={'hello.txt': b'Hello from the cartridge!\n'})
    path = os.path.join(out, 'test.nds')
    open(path, 'wb').write(rom)
    r = Rom.load(path)
    assert r.arm9_compressed and r.arm9[:len(arm9)] == arm9[:params + 0x14] + b'\0\0\0\0' + arm9[params + 0x18:len(arm9)], 'BLZ round trip'
    print(f'{path}: ARM9 {len(arm9)} bytes ({len(packed)} compressed), ARM7 {len(arm7)} bytes, autoloads {[hex(s.addr) for s in r.autoloads]}')
    host = os.path.join(out, 'host_suite')
    run(['cc', '-O2', '-o', host, src('host.c'), src('suite.c')])
    ref = subprocess.run([host], check=True, capture_output=True, text=True).stdout
    open(os.path.join(out, 'host_ref.txt'), 'w').write(ref)
    return path


if __name__ == '__main__':
    main()
