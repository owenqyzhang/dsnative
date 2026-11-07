"""Code discovery on small synthetic images, for cases the homebrew ROM only hits when the
toolchain happens to lay it out that way.

    python3 tests/test_discovery.py
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)
from recomp.program import Program      # noqa: E402

BASE = 0x02000000


def image(parts):
    """parts: {offset: bytes or u32 or [u16 Thumb halfwords]} -> bytes."""
    end = max(o + (len(v) if isinstance(v, bytes) else 4 if isinstance(v, int) else 2 * len(v)) for o, v in parts.items())
    b = bytearray(end)
    for o, v in parts.items():
        if isinstance(v, int):
            struct.pack_into('<I', b, o, v)
        elif isinstance(v, list):
            struct.pack_into(f'<{len(v)}H', b, o, *v)
        else:
            b[o:o + len(v)] = v
    return bytes(b)


def discover(data):
    return Program([('arm9', BASE, data, 'static')], arch=5).discover([(BASE, False)])


def test_string_pointer_does_not_hide_table():
    """A literal pool holds a pointer to a string at an odd address (the tail of a merged
    string). It reads as a Thumb candidate, ASCII decodes as plausible Thumb code, and the
    walk from it runs on into a table of function pointers right after the string. The
    table's entries must still be found."""
    p = discover(image({
        0x00: 0xE59F0008,               # ldr r0, [pc, #8]  (the literal at 0x10)
        0x04: 0xE12FFF1E,               # bx lr
        0x10: BASE + 0x21,              # "nptr ok" read as Thumb at 0x20
        0x20: b'fnptr ok\0\0\0\0\0\0\0\0',
        0x30: BASE + 0x41,              # ops[] = {op_add, op_sub, op_xor}, all Thumb
        0x34: BASE + 0x45,
        0x38: BASE + 0x49,
        0x40: [0x1840, 0x4770],         # adds r0, r0, r1; bx lr
        0x44: [0x1A40, 0x4770],         # subs r0, r0, r1; bx lr
        0x48: [0x4048, 0x4770],         # eors r0, r1; bx lr
    }))
    for off in (0x40, 0x44, 0x48):
        assert (0, BASE + off, True) in p.entries, f'function pointer to {BASE + off:#x} (Thumb) not taken'


def test_candidate_inside_function_keeps_jump_table():
    """A pointer candidate lands between a jump table's cmp and its add pc and is walked
    before the function itself. The walk from the function's entry must still see the cmp,
    bound the table and reach every case."""
    p = discover(image({
        0x00: 0xE59F1010,               # ldr r1, [pc, #16]  (the literal at 0x18)
        0x04: 0xEB000007,               # bl 0x28
        0x08: 0xE12FFF1E,               # bx lr
        0x18: BASE + 0x2C,              # points into the function below, after its cmp
        0x28: 0xE3500002,               # cmp r0, #2
        0x2C: 0xE1A01001,               # mov r1, r1
        0x30: 0x908FF100,               # addls pc, pc, r0, lsl #2
        0x34: 0xEA000002,               # b 0x44
        0x38: 0xEA000003,               # b 0x4c
        0x3C: 0xEA000004,               # b 0x54
        0x40: 0xEA000005,               # b 0x5c
        0x44: 0xE3A00000,               # mov r0, #0; bx lr
        0x48: 0xE12FFF1E,
        0x4C: 0xE3A0000A,               # mov r0, #10; bx lr
        0x50: 0xE12FFF1E,
        0x54: 0xE3A00014,               # mov r0, #20; bx lr
        0x58: 0xE12FFF1E,
        0x5C: 0xE3A0001E,               # mov r0, #30; bx lr
        0x60: 0xE12FFF1E,
    }))
    assert p.extra_targets.get((0, BASE + 0x30, False)) == [(BASE + o, False) for o in (0x38, 0x3C, 0x40)], \
        f'jump table at {BASE + 0x30:#x}: {p.extra_targets.get((0, BASE + 0x30, False))}'
    body = {a for body, _ in p.build_functions().values() for a in body}
    for off in (0x4C, 0x54, 0x5C):
        assert BASE + off in body, f'case at {BASE + off:#x} not reached'
    assert not p.warnings, p.warnings


def arm(*words):
    return struct.pack(f'<{len(words)}I', *words)


def test_byte_offset_jump_table():
    """GCC 15's ARM switch: the index indexes a table of byte offsets in rodata, and the
    cmp bounds the index, not the register add pc reads."""
    p = discover(image({
        0x00: arm(0xE59F2038,           # ldr r2, [pc, #0x38]  (the table's address, at 0x40)
                  0xE3530002,           # cmp r3, #2
                  0x8A000008,           # bhi 0x30
                  0xE7D23003,           # ldrb r3, [r2, r3]
                  0xE08FF103,           # add pc, pc, r3, lsl #2
                  0xE1A00000,           # nop
                  0xE3A0000A, 0xE12FFF1E,   # 0x18: mov r0, #10; bx lr
                  0xE3A00014, 0xE12FFF1E,   # 0x20: mov r0, #20; bx lr
                  0xE3A0001E, 0xE12FFF1E,   # 0x28: mov r0, #30; bx lr
                  0xE3A0002A, 0xE12FFF1E),  # 0x30: mov r0, #42; bx lr
        0x40: BASE + 0x48,
        0x48: bytes([4, 0, 2]),
    }))
    assert p.extra_targets.get((0, BASE + 0x10, False)) == [(BASE + o, False) for o in (0x28, 0x18, 0x20)], \
        p.extra_targets.get((0, BASE + 0x10, False))


def test_halfword_offset_jump_table():
    p = discover(image({
        0x00: arm(0xE59F2038,           # ldr r2, [pc, #0x38]  (the table's address, at 0x40)
                  0xE3530002,           # cmp r3, #2
                  0x8A000009,           # bhi 0x34
                  0xE0833003,           # add r3, r3, r3
                  0xE19230B3,           # ldrh r3, [r2, r3]
                  0xE08FF103,           # add pc, pc, r3, lsl #2
                  0xE1A00000,           # nop
                  0xE3A0000A, 0xE12FFF1E,   # 0x1c: mov r0, #10; bx lr
                  0xE3A00014, 0xE12FFF1E,   # 0x24: mov r0, #20; bx lr
                  0xE3A0001E, 0xE12FFF1E,   # 0x2c: mov r0, #30; bx lr
                  0xE3A0002A, 0xE12FFF1E),  # 0x34: mov r0, #42; bx lr
        0x40: BASE + 0x48,
        0x48: struct.pack('<3H', 4, 0, 2),
    }))
    assert p.extra_targets.get((0, BASE + 0x14, False)) == [(BASE + o, False) for o in (0x2C, 0x1C, 0x24)], \
        p.extra_targets.get((0, BASE + 0x14, False))


def test_constant_branch_target_is_code():
    """ldr r3, =f; blx r3 (crt0 calling main): f is code reached from the entry point, so
    its instruction words are not read as pointers. andeq r0, r0, #0x40 is 0x02000040."""
    p = discover(image({
        0x00: arm(0xE59F3008,           # ldr r3, [pc, #8]  (f, at 0x10)
                  0xE12FFF33,           # blx r3
                  0xE12FFF1E),          # bx lr
        0x10: BASE + 0x20,
        0x20: arm(0x02000040,           # f: andeq r0, r0, #0x40
                  0xE12FFF1E),          # bx lr
        0x40: arm(0xE3A00001, 0xE12FFF1E),  # mov r0, #1; bx lr
    }))
    assert (0, BASE + 0x20, False) in p.entries
    assert (0, BASE + 0x40, False) not in p.entries, 'an instruction word taken as a pointer'


def test_overwritten_index_is_no_bound():
    """A cmp of a register written again before the jump bounds nothing."""
    p = discover(image({
        0x00: arm(0xE3500002,           # cmp r0, #2
                  0xE1A00001,           # mov r0, r1
                  0x908FF100,           # addls pc, pc, r0, lsl #2
                  0xE12FFF1E),          # bx lr
    }))
    assert (0, BASE + 0x08, False) not in p.extra_targets, p.extra_targets
    assert any('without a bound' in w for w in p.warnings), p.warnings


def main():
    failed = 0
    for name, fn in list(globals().items()):
        if name.startswith('test_') and callable(fn):
            try:
                fn()
                print(f'ok    {name}')
            except AssertionError as e:
                failed += 1
                print(f'FAIL  {name}: {e}')
    if failed:
        print(f'{failed} discovery test(s) failed')
        return 1
    print('discovery tests passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
