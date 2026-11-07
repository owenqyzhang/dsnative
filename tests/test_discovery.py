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


def test_thumb_switch():
    """CodeWarrior's Thumb switch: a table of signed halfword offsets right after add pc."""
    p = Program([('arm9', BASE, image({0x00: [
        0x2802,             # cmp r0, #2
        0xD80E,             # bhi 0x22
        0x1800,             # adds r0, r0, r0
        0x4478,             # add r0, pc
        0x88C0,             # ldrh r0, [r0, #6]
        0x0400,             # lsls r0, r0, #16
        0x1400,             # asrs r0, r0, #16
        0x4487,             # add pc, r0
        0x0008, 0x000C, 0x0004,     # 0x10: the table, from 0x12
        0x2003, 0x4770,     # 0x16: movs r0, #3; bx lr
        0x2001, 0x4770,     # 0x1a: movs r0, #1; bx lr
        0x2002, 0x4770,     # 0x1e: movs r0, #2; bx lr
        0x2000, 0x4770,     # 0x22: movs r0, #0; bx lr
    ]}), 'static')], arch=5).discover([(BASE, True)])
    assert p.extra_targets.get((0, BASE + 0x0E, True)) == [(BASE + o, True) for o in (0x1A, 0x1E, 0x16)], \
        p.extra_targets.get((0, BASE + 0x0E, True))
    assert not p.warnings, p.warnings


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


def test_overlay_call_stays_in_its_overlay():
    """Two overlays share 0x02100000. The static code's call there could land in either;
    overlay A's call into its own range can only land in A, which is loaded while it runs."""
    ov = 0x02100000
    p = Program([('arm9', BASE, arm(0xEB03FFFE, 0xE12FFF1E), 'static'),     # bl 0x02100000; bx lr
                 ('ovA', ov, arm(0xE92D4000,        # push {lr}
                                 0xEB000001,        # bl 0x02100010
                                 0xE8BD8000,        # pop {pc}
                                 0,
                                 0xE3A00001, 0xE12FFF1E), 'overlay'),      # mov r0, #1; bx lr
                 ('ovB', ov, arm(0xE3A00002, 0xE12FFF1E, 0, 0,
                                 0xE3A00003, 0xE12FFF1E), 'overlay')], arch=5).discover([(BASE, False)])
    assert (1, ov, False) in p.entries and (2, ov, False) in p.entries, 'static call into shared overlay space'
    assert (1, ov + 0x10, False) in p.entries
    assert (2, ov + 0x10, False) not in p.entries, "overlay A's own call taken into overlay B"


SHARED = 0x02100000


def shared_address(static):
    """The static code at BASE (a call to a function of its own, then `static`) enters
    SHARED, which two overlays cover. Overlay A holds code there. Overlay B holds a table
    of two pointers to functions in the static code and a bx lr: data, that reads as
    code as well (andeq r0, r0, #0x20), and at most one of the two is what runs."""
    parts = {0x00: arm(0xEB00000A),                 # bl 0x30
             0x20: arm(0xE3A00001, 0xE12FFF1E),     # f: mov r0, #1; bx lr
             0x28: arm(0xE3A00002, 0xE12FFF1E),     # g: mov r0, #2; bx lr
             0x30: arm(0xE3A00004, 0xE12FFF1E)}     # h: mov r0, #4; bx lr
    parts.update(static)
    return Program([('arm9', BASE, image(parts), 'static'),
                    ('ovA', SHARED, arm(0xE3A00003, 0xE12FFF1E), 'overlay'),        # mov r0, #3; bx lr
                    ('ovB', SHARED, arm(BASE + 0x20, BASE + 0x28, 0xE12FFF1E), 'overlay')],
                   arch=5).discover([(BASE, False)])


def check_shared_address(p, how):
    for si in (1, 2):
        k = (si, SHARED, False)
        assert k in p._spec_visited and k not in p._visited, \
            f'{how}: the code at the shared address in segment {si} is not a candidate'
    assert (0, BASE + 0x30, False) in p._visited, f'{how}: a call to the one segment there is not certain'
    assert (1, SHARED, False) in p.build_functions(), f'{how}: overlay A\'s code is not compiled'
    for off in (0x20, 0x28):
        assert (0, BASE + off, False) in p.entries, \
            f'{how}: overlay B\'s pointer to {BASE + off:#x} is hidden by walking its table as code'


def test_shared_address_branch_is_a_candidate():
    """A call or branch to an address several segments cover walks each as a candidate. Walked
    as certain code, overlay B's table would hide its pointers from the data scan, which
    skips only words that certain code covers."""
    for how, word in (('bl', 0xEB03FFFD), ('b', 0xEA03FFFD)):      # to 0x02100000
        check_shared_address(shared_address({0x04: arm(word, 0xE12FFF1E)}), how)


def test_shared_address_computed_call_is_a_candidate():
    """The same for ldr r3, =addr; blx r3, which jumps.evaluate reads as a constant."""
    check_shared_address(shared_address({0x04: arm(0xE59F3008,      # ldr r3, [pc, #8]  (the literal at 0x14)
                                                   0xE12FFF33,      # blx r3
                                                   0xE12FFF1E),     # bx lr
                                         0x14: SHARED}), 'blx')


def test_saved_context_resumes():
    """OS_SaveContext saves adr + 8 as the context's pc, and OS_LoadContext resumes it with
    subs pc, lr, #4: on the bx lr, which must be a label."""
    p = discover(image({
        0x00: arm(0xE3A00001,           # mov r0, #1
                  0xE8817FFF,           # stm r1, {r0-lr}
                  0xE28F0008,           # add r0, pc, #8  (0x18)
                  0xE581003C,           # str r0, [r1, #0x3c]
                  0xE3A00000,           # mov r0, #0
                  0xE12FFF1E),          # 0x14: bx lr
    }))
    assert BASE + 0x14 in p.build_functions()[(0, BASE, False)][1], 'the resume point is not a label'


def test_stub_and_autoload_share_an_address():
    """crt0 runs a stub at 0x01ff8000 before the ITCM autoload is copied there: a call there
    can land in either, and both are checked against memory when dispatched."""
    import tempfile
    itcm = 0x01FF8000
    p = Program([('arm9', BASE, arm(0xE59F200C, 0xE12FFF32,      # ldr r2, =itcm; blx r2
                                    0xE59F2008, 0xE12FFF32,      # ldr r2, =itcm + 8; blx r2
                                    0xE12FFF1E, itcm, itcm + 8), 'static'),
                 ('itcm', itcm, arm(0xE3A00001, 0xE12FFF1E, 0xE3A00002, 0xE12FFF1E), 'autoload'),
                 ('stub', itcm, arm(0xE3A00003, 0xE12FFF1E), 'stub')], arch=5).discover([(BASE, False)])
    assert (1, itcm, False) in p.entries and (2, itcm, False) in p.entries
    with tempfile.TemporaryDirectory() as d:
        p.emit(d)
        table = open(os.path.join(d, 'g9_table.c')).read()
    assert table.count('{0x1ff8000u, 16, ') == 1 and table.count('{0x1ff8000u, 8, ') == 1, table
    assert '{0x1ff8008u, 0, ' in table, 'code only the autoload has needs no check'


def test_overwritten_index_is_no_bound():
    """A cmp of a register written again before the jump bounds nothing: the table is then
    the run of branches after it, not n + 1 entries."""
    p = discover(image({
        0x00: arm(0xE3500005,           # cmp r0, #5
                  0xE1A00001,           # mov r0, r1
                  0x908FF100,           # addls pc, pc, r0, lsl #2
                  0xEA000001,           # b 0x18
                  0xEA000001,           # 0x10: b 0x1c
                  0xEA000001,           # b 0x20
                  0xE12FFF1E,           # 0x18: bx lr
                  0xE3A00001, 0xE12FFF1E,   # 0x1c: mov r0, #1; bx lr
                  0xE3A00002, 0xE12FFF1E),  # 0x24: mov r0, #2; bx lr
    }))
    assert p.extra_targets.get((0, BASE + 0x08, False)) == [(BASE + 0x10, False), (BASE + 0x14, False)], \
        p.extra_targets.get((0, BASE + 0x08, False))


def test_unrolled_loop_entry():
    """The SDK's division enters an unrolled loop part way: add pc, pc, r2, lsl #2 with
    nothing to bound r2. Every instruction of the run after it is a place it can land."""
    p = discover(image({
        0x00: arm(0xE0822082,           # add r2, r2, r2, lsl #1
                  0xE08FF102,           # add pc, pc, r2, lsl #2
                  0xE1A00000,           # nop
                  0xE0B33081, 0xE0433001, 0xE0B00000,   # 0x0c: adcs r3, r1, r3, lsl #1; sublo; adcs
                  0xE0B33081, 0xE0433001, 0xE0B00000,   # 0x18
                  0xE12FFF1E),          # 0x24: bx lr
    }))
    assert p.extra_targets.get((0, BASE + 0x04, False)) == [(BASE + o, False) for o in range(0x0C, 0x28, 4)], \
        p.extra_targets.get((0, BASE + 0x04, False))
    assert not p.warnings, p.warnings


def thumb_discover(parts, arch=5):
    return Program([('arm9', BASE, image(parts), 'static')], arch=arch).discover([(BASE, True)])


def cases(p, jump, thumb, offsets):
    got = p.extra_targets.get((0, BASE + jump, thumb))
    want = [(BASE + o, thumb) for o in offsets]
    assert got == want, f'jump table at {BASE + jump:#x}: {[(hex(a), t) for a, t in got or []]}'


def test_thumb_switch_signed_range():
    """CodeWarrior's Thumb switch bounded by a signed range: cmp r0, #2; bgt default;
    cmp r0, #0; blt default. Neither check alone bounds the table."""
    p = thumb_discover({0x00: [
        0x2802,             # cmp r0, #2
        0xDC10,             # bgt 0x26
        0x2800,             # cmp r0, #0
        0xDB0E,             # blt 0x26
        0x1800,             # 0x08: adds r0, r0, r0
        0x4478,             # add r0, pc
        0x88C0,             # ldrh r0, [r0, #6]
        0x0400,             # lsls r0, r0, #16
        0x1400,             # asrs r0, r0, #16
        0x4487,             # 0x12: add pc, r0
        0x0008, 0x000C, 0x0004,     # 0x14: the table
        0x2003, 0x4770,     # 0x1a: movs r0, #3; bx lr
        0x2001, 0x4770,     # 0x1e
        0x2002, 0x4770,     # 0x22
        0x2000, 0x4770,     # 0x26: default
    ]})
    cases(p, 0x12, True, (0x1E, 0x22, 0x1A))
    assert not p.warnings, p.warnings


def test_thumb_switch_ble_over_default():
    """cmp r0, #2; ble 1f; b default; 1: cmp r0, #0; blt default: the upper bound is a
    branch taken over the b, the lower bound a check after it."""
    p = thumb_discover({0x00: [
        0x2802,             # cmp r0, #2
        0xDD00,             # ble 0x06
        0xE010,             # b 0x28
        0x2800,             # 0x06: cmp r0, #0
        0xDB0E,             # blt 0x28
        0x1800,             # 0x0a: adds r0, r0, r0
        0x4478,             # add r0, pc
        0x88C0,             # ldrh r0, [r0, #6]
        0x0400,             # lsls r0, r0, #16
        0x1400,             # asrs r0, r0, #16
        0x4487,             # 0x14: add pc, r0
        0x0008, 0x000C, 0x0004,     # 0x16: the table
        0x2003, 0x4770,     # 0x1c: movs r0, #3; bx lr
        0x2001, 0x4770,     # 0x20
        0x2002, 0x4770,     # 0x24
        0x2000, 0x4770,     # 0x28: default
    ]})
    cases(p, 0x14, True, (0x20, 0x24, 0x1C))
    assert not p.warnings, p.warnings


def test_thumb_switch_after_call_that_returns():
    """A bound before a call is no bound of the switch after it: cmp r0, #1; bhi 1f; bl f;
    1: cmp r0, #2; bhi default. f returns, so r0 = 0 and 1 reach the switch too."""
    p = thumb_discover({0x00: [
        0x2801,             # cmp r0, #1
        0xD801,             # bhi 0x08
        0xF000, 0xF814,     # bl 0x30
        0x2802,             # 0x08: cmp r0, #2
        0xD80E,             # bhi 0x2a
        0x1800,             # adds r0, r0, r0
        0x4478,             # add r0, pc
        0x88C0,             # ldrh r0, [r0, #6]
        0x0400,             # lsls r0, r0, #16
        0x1400,             # asrs r0, r0, #16
        0x4487,             # 0x16: add pc, r0
        0x0008, 0x000C, 0x0004,     # 0x18: the table
        0x2003, 0x4770,     # 0x1e: movs r0, #3; bx lr
        0x2001, 0x4770,     # 0x22
        0x2002, 0x4770,     # 0x26
        0x2000, 0x4770,     # 0x2a: default
        0, 0,
        0x4770,             # 0x30: f: bx lr
    ]})
    cases(p, 0x16, True, (0x22, 0x26, 0x1E))


def test_thumb_switch_after_assertion():
    """cmp r0, #2; bls 1f; bl panic; 1: switch: the only bound is the one a failed assert
    skips, and panic never returns."""
    p = thumb_discover({0x00: [
        0x2802,             # cmp r0, #2
        0xD901,             # bls 0x08
        0xF000, 0xF814,     # bl 0x30
        0x1800,             # 0x08: adds r0, r0, r0
        0x4478,             # add r0, pc
        0x88C0,             # ldrh r0, [r0, #6]
        0x0400,             # lsls r0, r0, #16
        0x1400,             # asrs r0, r0, #16
        0x4487,             # 0x12: add pc, r0
        0x0008, 0x000C, 0x0004,     # 0x14: the table
        0x2003, 0x4770,     # 0x1a: movs r0, #3; bx lr
        0x2001, 0x4770,     # 0x1e
        0x2002, 0x4770,     # 0x22
        0, 0, 0, 0, 0,
        0xE7FE,             # 0x30: panic: b panic
    ]})
    cases(p, 0x12, True, (0x1E, 0x22, 0x1A))


def test_thumb_switch_after_zero_check():
    """adds r4, r0, #0; bne 1f; movs r0, #0; pop {r4, pc}; 1: cmp r4, #2; bhi default; the
    switch. The zero check leaves out index 0 only if the bne is the one way to 1:, which
    the code doesn't say: the table keeps it."""
    p = thumb_discover({0x00: [
        0x1C04,             # adds r4, r0, #0
        0xD101,             # bne 0x08
        0x2000, 0xBD10,     # movs r0, #0; pop {r4, pc}
        0x2C02,             # 0x08: cmp r4, #2
        0xD80E,             # bhi 0x2a
        0x1922,             # adds r2, r4, r4
        0x447A,             # add r2, pc
        0x88D2,             # ldrh r2, [r2, #6]
        0x0412,             # lsls r2, r2, #16
        0x1412,             # asrs r2, r2, #16
        0x4497,             # 0x16: add pc, r2
        0x0008, 0x000C, 0x0004,     # 0x18: the table
        0x2003, 0x4770,     # 0x1e: movs r0, #3; bx lr
        0x2001, 0x4770,     # 0x22
        0x2002, 0x4770,     # 0x26
        0x2000, 0x4770,     # 0x2a: default
    ]})
    cases(p, 0x16, True, (0x22, 0x26, 0x1E))


def test_thumb_address_table():
    """GCC 15's switch for the ARM7 (ARMv4T has no tbb): the cases are plain even addresses
    in a table in rodata, loaded with ldr r3, [r3, r0] (the index shifted) and jumped to
    with mov pc, r3. Nothing about the table is encoded in the instructions but the bound."""
    p = thumb_discover({
        0x00: [0xB510,      # push {r4, lr}
               0x2802,      # cmp r0, #2
               0xD809,      # bhi 0x1a
               0x4B06,      # ldr r3, [pc, #24]  (the table's address, at 0x20)
               0x0080,      # lsls r0, r0, #2
               0x581B,      # ldr r3, [r3, r0]
               0x469F,      # 0x0c: mov pc, r3
               0x2001, 0xBD10,      # 0x0e: movs r0, #1; pop {r4, pc}
               0x2002, 0xBD10,      # 0x12
               0x2003, 0xBD10,      # 0x16
               0x2000, 0xBD10,      # 0x1a: default
               0x46C0],
        0x20: BASE + 0x24,
        0x24: BASE + 0x16, 0x28: BASE + 0x0E, 0x2C: BASE + 0x12,
    }, arch=4)
    cases(p, 0x0C, True, (0x16, 0x0E, 0x12))
    assert not p.warnings, p.warnings


def test_arm7_halfword_table():
    """The ARM7's SDK code in Black 2: the index is a bit field, the table of halfwords is
    addressed from add r1, pc, r3, lsl #1 and added to the pc."""
    p = Program([('arm7', BASE, image({
        0x00: arm(0xE5902000,           # ldr r2, [r0]
                  0xE1A01902,           # lsl r1, r2, #18
                  0xE1A03D21,           # lsr r3, r1, #26
                  0xE3530002,           # cmp r3, #2
                  0x8A00000A,           # bhi 0x40
                  0xE08F1083,           # 0x14: add r1, pc, r3, lsl #1
                  0xE1D110B4,           # ldrh r1, [r1, #4]
                  0xE08FF001),          # 0x1c: add pc, pc, r1
        0x20: struct.pack('<3H', 0x04, 0x0C, 0x14),     # the table: cases at 0x24 + each
        0x28: arm(0xE3A0000A, 0xE12FFF1E,   # mov r0, #10; bx lr
                  0xE3A00014, 0xE12FFF1E,   # 0x30
                  0xE3A0001E, 0xE12FFF1E),  # 0x38
        0x40: arm(0xE3A00000, 0xE12FFF1E),  # default
    }), 'static')], arch=4).discover([(BASE, False)])
    cases(p, 0x1C, False, (0x28, 0x30, 0x38))
    assert not p.warnings, p.warnings


def test_ldr_pc_address_table():
    """ldr pc, [pc, r0, lsl #2] after cmp r0, #2: a table of addresses follows."""
    p = discover(image({0x00: arm(
        0xE3500002,         # cmp r0, #2
        0x979FF100,         # ldrls pc, [pc, r0, lsl #2]
        0xEA000008,         # b 0x30
        BASE + 0x18, BASE + 0x20, BASE + 0x28,      # 0x0c: the table
        0xE3A0000A, 0xE12FFF1E,     # 0x18: mov r0, #10; bx lr
        0xE3A00014, 0xE12FFF1E,     # 0x20
        0xE3A0001E, 0xE12FFF1E,     # 0x28
        0xE3A00000, 0xE12FFF1E)}))  # 0x30
    cases(p, 0x04, False, (0x18, 0x20, 0x28))


def pic_tables(stubs):
    """Emit a program with the given position-independent stubs [(name, words)]: the
    program, and the text of its g9_pic.c."""
    import tempfile
    p = Program([('arm9', BASE, arm(0xE12FFF1E), 'static')], arch=5,
                pic=[(n, arm(*w)) for n, w in stubs]).discover([(BASE, False)])
    with tempfile.TemporaryDirectory() as d:
        p.emit(d)
        return p, open(os.path.join(d, 'g9_pic.c')).read()


def test_position_independent_stub():
    """A stub copied to the stack is compiled as a function of its own and listed with its
    bytes, longest first, for the dispatcher to find by content."""
    p, src = pic_tables([('short', [0xE1C120B0, 0xE12FFF10]),          # strh r2, [r1]; bx r0
                         ('long', [0xE1C120B0, 0xE1A00000, 0xE12FFF10])])    # strh; mov r0, r0; bx r0
    assert not p.warnings, p.warnings
    assert src.index('{12, ') < src.index('{8, '), 'the longer stub comes first'
    assert 'const u32 g9_pic_len = 2;' in src
    assert 'wr16(c, a, R[2]);' in src and 'return dsr_bx(c, R[0]);' in src, 'the stub is compiled'
    assert '{8, g9_pic0_bytes, g9_pic0}' in src and '{12, g9_pic1_bytes, g9_pic1}' in src


def test_stub_that_reads_its_address_is_refused():
    """Nothing in a stub that runs wherever it is may depend on where: a pc read or a branch
    makes the code differ with the address, and straight-line code ends at the one jump."""
    for what, words in {'pc read': [0xE28F0004, 0xE12FFF10],            # add r0, pc, #4; bx r0
                        'literal load': [0xE59F0000, 0xE12FFF10],       # ldr r0, [pc]; bx r0
                        'branch': [0xEA000000, 0xE12FFF10],             # b .+8; bx r0
                        'call': [0xEB000000, 0xE12FFF10],               # bl .+8; bx r0
                        'no jump': [0xE1C120B0],                        # strh r2, [r1]
                        'conditional jump': [0xE1C120B0, 0x012FFF10]}.items():     # bxeq r0
        p, src = pic_tables([('stub', words)])
        assert any('not position-independent' in w for w in p.warnings), f'{what}: {p.warnings}'
        assert 'const u32 g9_pic_len = 0;' in src, f'{what}: the stub was emitted'


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
