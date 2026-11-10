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
import nds                                                      # noqa: E402
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


def arm7_image_unsigned(chance=False):
    """SDK 4's ARM7: no signature or SDK version; the six parameter words sit after the crt0's
    literals and its start-up code reaches them through a literal at 0x20."""
    segs = [(0x027E0000, bytes(range(0x30)), 0), (0x037F8000, bytes(range(0x40, 0x90)), 0x20),
            (0x06000000, bytes(range(0x90, 0xB8)), 0x10)]
    params, start = 0x40, 0x58
    lst = start + sum(len(d) for _, d, _ in segs)
    entries = b''.join(struct.pack('<3I', a, len(d), bss) for a, d, bss in segs)
    img = bytearray(lst + len(entries))
    struct.pack_into('<I', img, 0, 0xEAFFFFFE)                          # b .
    struct.pack_into('<I', img, 0x20, ARM7 + params)                    # the literal start-up code loads
    # a word that points into the image but not at parameters comes first
    struct.pack_into('<I', img, 0x10, ARM7 + 0x30)
    struct.pack_into('<6I', img, params, ARM7 + lst, ARM7 + lst + len(entries), ARM7 + start, ARM7 + start, ARM7 + start, 0)
    o = start
    for _, d, _ in segs:
        img[o:o + len(d)] = d
        o += len(d)
    img[lst:] = entries
    return bytes(img), segs


def check_unsigned():
    arm7, segs = arm7_image_unsigned()
    rom = Rom.load(build_rom(struct.pack('<I', 0xEAFFFFFE), 0x02000000, 0x02000000, arm7, ARM7, ARM7))
    got = [(s.addr, s.data, s.bss) for s in rom.autoloads7]
    assert got == segs, f'{[(hex(a), len(d), b) for a, d, b in got]}'
    assert len(rom.segments(7, overlays=False)[0].data) == 0x58, 'the static image ends where the autoload data starts'


def check_no_unsigned():
    """Words that merely point into the image are not parameters: the sizes must add up."""
    arm7, _ = arm7_image_unsigned()
    img = bytearray(arm7)
    struct.pack_into('<I', img, 0x40, struct.unpack_from('<I', img, 0x40)[0] + 12)   # the list drops its first entry
    rom = Rom.load(build_rom(struct.pack('<I', 0xEAFFFFFE), 0x02000000, 0x02000000, bytes(img), ARM7, ARM7))
    assert rom.params7 is None and not rom.autoloads7, 'a table that does not add up was taken for the parameters'
    plain = Rom.load(build_rom(struct.pack('<I', 0xEAFFFFFE), 0x02000000, 0x02000000, b'\x00' * 0x40, ARM7, ARM7))
    assert plain.params7 is None, 'homebrew has no parameters'


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


def pic_rom(copy_words, stub):
    """An ARM7 whose start-up (at 0x50) ends in `copy_words`, then the jump at 0x70, with the stub
    at 0xA0, the way SDK 5's crt0 copies its last two words to the stack and jumps there."""
    img = bytearray(arm7_image(3)[0])
    code = [0xE59F4038,         # ldr r4, [pc, #0x38]  (the stub's address, at 0x90)
            0xE4945004,         # ldr r5, [r4], #4
            0xE50D5004,         # str r5, [sp, #-4]
            0xE5945000,         # ldr r5, [r4]
            0xE58D5000,         # str r5, [sp]
            0xE3A02004,         # mov r2, #4
            0xE59F0024,         # ldr r0, [pc, #0x24]  (the code to run next, at 0x94)
            *copy_words,
            0xE12FFF13,         # 0x70: bx r3
            0xEAFFFFFE]         # b .
    struct.pack_into(f'<{len(code)}I', img, 0x50, *code)
    struct.pack_into('<II', img, 0x90, ARM7 + 0xA0, 0x037F8000)
    struct.pack_into(f'<{len(stub)}I', img, 0xA0, *stub)
    return Rom.load(build_rom(struct.pack('<I', 0xEAFFFFFE), 0x02000000, 0x02000000, bytes(img), ARM7, ARM7))


STRH_R2_R1, BX_R0 = 0xE1C120B0, 0xE12FFF10


def check_pic_stub():
    """Code copied to the stack and jumped to has no address: the reader returns it as a
    position-independent stub, not as a segment."""
    rom = pic_rom([0xE24D3004], [STRH_R2_R1, BX_R0])       # sub r3, sp, #4
    got = [(s.addr, s.data, s.kind) for s in rom.pic_stubs[7]]
    assert got == [(0, struct.pack('<II', STRH_R2_R1, BX_R0), 'pic')], got
    assert not rom.pic_stubs[9] and not rom.stubs[7], 'a stack stub is not a start-up stub of a fixed address'
    assert all(s.kind != 'pic' for s in rom.segments(7)), 'a stub without an address is not a segment'


def check_no_pic_stub():
    """Only a copy the reader follows to its end is a stub."""
    cases = {
        'a jump to a register that is not on the stack': ([0xE1A03000], [STRH_R2_R1, BX_R0]),       # mov r3, r0
        'a store after the copy through an unknown base': ([0xE5862000, 0xE24D3004], [STRH_R2_R1, BX_R0]),    # str r2, [r6]
        'a stack address that isn\'t the first word copied': ([0xE24D3008], [STRH_R2_R1, BX_R0]),   # sub r3, sp, #8
        'a copy that is not code that leaves': ([0xE24D3004], [STRH_R2_R1, STRH_R2_R1]),
        'a stack pointer written in between': ([0xE24DD010, 0xE24D3004], [STRH_R2_R1, BX_R0]),      # sub sp, sp, #16
    }
    for what, (copy, stub) in cases.items():
        got = pic_rom(copy, stub).pic_stubs[7]
        assert not got, f'{what}: found {[s.data.hex() for s in got]}'


def check_blz_in_place():
    """A crt0 decompresses in place, writing backwards from the end of the output while it reads
    backwards from the footer. A literal costs a flag bit, so when the first stretch of the
    compressed part doesn't shrink, the output pointer can overtake input not read yet: the
    compressor then has to leave more of the start uncompressed."""
    import random
    rng = random.Random(7)
    data = bytes(rng.randrange(256) for _ in range(0x40)) + bytes(rng.randrange(256) for _ in range(300)) + \
        b'the quick brown fox jumps over the lazy dog. ' * 120
    keep = 0x40
    naive = nds._blz_encode(data, keep)
    assert nds.blz_decompress(naive) == data, 'the encoder round trip'
    assert nds._blz_in_place(naive) != data, 'the data should be one where the plain encoding is unsafe in place'
    packed = nds.blz_compress(data, keep)
    assert packed is not None and len(packed) < len(data)
    assert packed[:keep] == data[:keep], 'the head is kept'
    assert nds.blz_decompress(packed) == data, 'the round trip'
    assert nds._blz_in_place(packed) == data, 'decompression in place'
    assert nds.blz_compress(bytes(rng.randrange(256) for _ in range(2000)), keep) is None, 'nothing to gain'


def main():
    failed = 0
    for name, fn in [('ARM7 autoloads, 3-word entries', lambda: check(3)), ('ARM7 autoloads, 4-word entries', lambda: check(4)),
                     ('ARM7 parameters without a signature', check_unsigned), ('no parameters without a signature', check_no_unsigned),
                     ('start-up stub', check_stub), ('stack stub', check_pic_stub),
                     ('no stack stub', check_no_pic_stub), ('BLZ compression decompresses in place', check_blz_in_place)]:
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
