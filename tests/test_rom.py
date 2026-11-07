"""The ROM reader on synthetic images: Nitro module parameters and autoload lists in the
layouts the SDK versions use, on both CPUs.

    python3 tests/test_rom.py
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, 'tools'))
from nds import build_rom, Rom, NITROCODE_BE, NITROCODE_LE     # noqa: E402

ARM7 = 0x02380000


def arm7_image(entry_words):
    """An ARM7 binary whose autoload list has the given entry size (3 or 4 words): two
    segments, for ARM7 WRAM and for main RAM."""
    segs = [(0x037F8000, bytes(range(0x40)), 0x10), (0x02FE0000, bytes(range(0x80, 0xA0)), 0x8)]
    start = 0x100
    lst = start + sum(len(d) for _, d, _ in segs)
    entries = b''.join(struct.pack('<4I', a, len(d), a, bss) if entry_words == 4 else struct.pack('<3I', a, len(d), bss)
                       for a, d, bss in segs)
    img = bytearray(lst + len(entries))
    struct.pack_into('<I', img, 0, 0xEAFFFFFE)             # b .
    struct.pack_into('<7I', img, 0x20, ARM7 + lst, ARM7 + lst + len(entries), ARM7 + start, ARM7 + start, ARM7 + start,
                     0, 0x0503757C)
    struct.pack_into('<II', img, 0x3C, NITROCODE_BE, NITROCODE_LE)
    o = start
    for _, d, _ in segs:
        img[o:o + len(d)] = d
        o += len(d)
    img[lst:] = entries
    return bytes(img), segs


def check(entry_words):
    arm7, segs = arm7_image(entry_words)
    arm9 = struct.pack('<I', 0xEAFFFFFE)
    rom = Rom.load(build_rom(arm9, 0x02000000, 0x02000000, arm7, ARM7, ARM7))
    got = [(s.addr, s.data, s.bss) for s in rom.autoloads7]
    assert got == segs, f'{entry_words}-word entries: {[(hex(a), len(d), b) for a, d, b in got]}'
    static = rom.segments(7, overlays=False)[0]
    assert static.addr == ARM7 and len(static.data) == 0x100, 'the ARM7 static image ends where the autoload data starts'


def check_stub():
    """SDK 5's crt0 copies a start-up stub to ITCM and calls it before the autoloads."""
    stub = struct.pack('<4I', 0xE1D020B0, 0xE1510002, 0x012FFF1E, 0xEAFFFFFB)   # ldrh; cmp; bxeq lr; b
    img = bytearray(0x80)
    struct.pack_into('<10I', img, 0, 0xE59F1020,   # ldr r1, [pc, #0x20]  (src, at 0x28)
                     0xE59F2020,                   # ldr r2, [pc, #0x20]  (dst, at 0x2c)
                     0xE2823010,                   # add r3, r2, #0x10
                     0xE4910004, 0xE4820004, 0xE1520003, 0xBAFFFFFB,   # the copy loop
                     0xE12FFF32,                   # blx r2
                     0xEAFFFFFE, 0)                # b .
    struct.pack_into('<II', img, 0x28, 0x02000040, 0x01FF8000)
    img[0x40:0x50] = stub
    rom = Rom.load(build_rom(bytes(img), 0x02000000, 0x02000000, arm7_image(3)[0], ARM7, ARM7))
    got = [(s.addr, s.data, s.kind) for s in rom.stubs[9]]
    assert got == [(0x01FF8000, stub, 'stub')], got
    assert any(s.kind == 'stub' for s in rom.segments(9)), 'the stub is not among the ARM9 segments'


def main():
    failed = 0
    for name, fn in [('ARM7 autoloads, 3-word entries', lambda: check(3)), ('ARM7 autoloads, 4-word entries', lambda: check(4)),
                     ('start-up stub', check_stub)]:
        try:
            fn()
            print(f'ok    {name}')
        except AssertionError as e:
            failed += 1
            print(f'FAIL  {name}: {e}')
    print('ROM reader tests passed' if not failed else f'{failed} ROM reader test(s) failed')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
