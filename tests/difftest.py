"""Differential test of the recompiler and the interpreter against Unicorn's ARM946 (QEMU's
ARMv5TE model) and against each other.

Builds thousands of random snippets of ARM and Thumb code (data processing, multiplies,
the ARMv5TE DSP instructions, loads and stores of every kind, block transfers, conditional
branches), recompiles them to C with the real recompiler, and runs each from the same random
state in Unicorn, as recompiled code, in the runtime's interpreter (no compiled code at all),
and with half of the snippet's compiled entries dropped (control passes between the two, in
both directions). It compares the registers, the flags and a 4 KB data area.

Unicorn can't judge the DS-specific classes (CP15, SWI, mode switches, user-bank transfers,
undefined encodings), and its snippets are structured. So a second part, for both CPUs
(ARMv5TE and ARMv4T), runs single random instructions of every encoding class, in every
mode, with random banked registers and wild memory accesses, as recompiled code and in the
interpreter, and compares everything: all registers, the CPSR and SPSR, the banked
registers, the memory and the sequence of accesses outside it, and where it stopped.

    python3 tests/difftest.py [--cases N] [--seed S] [--fuzz N]
"""
import argparse
import os
import random
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)
from recomp.program import Program      # noqa: E402

import unicorn                         # noqa: E402
from unicorn import arm_const as A     # noqa: E402

BASE = 0x02000000
DATA = 0x02200000
DATA_LEN = 0x1000
SLOT = 0x100


def cond(r):
    return 0xE if r.random() < 0.7 else r.randrange(0, 14)


# ---------------------------------------------------------------- ARM generators
def a_dp(r, dest, allow_pc_src=True):
    c = cond(r)
    op = r.randrange(16)
    s = 1 if op in (8, 9, 10, 11) else r.randrange(2)
    rd = 0 if op in (8, 9, 10, 11) else r.choice(dest)
    kind = r.randrange(3)
    rn = r.randrange(16 if allow_pc_src else 15)
    if op in (13, 15):
        rn = 0      # MOV/MVN: Rn should be zero
    if kind == 0:
        return (c << 28) | (1 << 25) | (op << 21) | (s << 20) | (rn << 16) | (rd << 12) | (r.randrange(16) << 8) | r.randrange(256)
    if kind == 1:
        rm = r.randrange(16 if allow_pc_src else 15)
        return (c << 28) | (op << 21) | (s << 20) | (rn << 16) | (rd << 12) | (r.randrange(32) << 7) | (r.randrange(4) << 5) | rm
    rn = 0 if op in (13, 15) else r.randrange(15)
    rs = r.randrange(15)
    rm = r.randrange(15)
    return (c << 28) | (op << 21) | (s << 20) | (rn << 16) | (rd << 12) | (rs << 8) | (r.randrange(4) << 5) | 0x10 | rm


def a_mul(r, dest):
    c = cond(r)
    k = r.randrange(9)
    rm, rs, rn = r.randrange(15), r.randrange(15), r.randrange(15)
    if k == 0:      # MUL/MLA
        rd = r.choice([x for x in dest if x != rm] or [0])
        acc = r.randrange(2)
        if not acc:
            rn = 0
        return (c << 28) | (acc << 21) | (r.randrange(2) << 20) | (rd << 16) | (rn << 12) | (rs << 8) | 0x90 | rm
    if k == 1:      # long multiplies
        hi, lo = r.sample([x for x in dest if x != rm], 2)
        return (c << 28) | (1 << 23) | (r.randrange(4) << 21) | (r.randrange(2) << 20) | (hi << 16) | (lo << 12) | (rs << 8) | 0x90 | rm
    if k == 2:      # CLZ
        return (c << 28) | 0x016F0F10 | (r.choice(dest) << 12) | rm
    if k == 3:      # QADD family
        return (c << 28) | 0x01000050 | (r.randrange(4) << 21) | (rn << 16) | (r.choice(dest) << 12) | rm
    # DSP multiplies
    op = r.randrange(4)
    x, y = r.randrange(2), r.randrange(2)
    if op == 2:
        hi, lo = r.sample(dest, 2)
        return (c << 28) | 0x01000080 | (2 << 21) | (hi << 16) | (lo << 12) | (rs << 8) | (y << 6) | (x << 5) | rm
    rd = r.choice(dest)
    if op == 3 or (op == 1 and x):
        rn = 0
    return (c << 28) | 0x01000080 | (op << 21) | (rd << 16) | (rn << 12) | (rs << 8) | (y << 6) | (x << 5) | rm


PTRS = [8, 9, 10, 11]


def a_mem(r, dest):
    c = cond(r)
    k = r.randrange(5)
    base = r.choice(PTRS)
    if k == 0:      # LDR/STR/LDRB/STRB immediate
        b = r.randrange(2)
        l = r.randrange(2)
        p, u = r.randrange(2), r.randrange(2)
        w = r.randrange(2) if p else 0
        off = r.randrange(0, 8) * 8 if (w or not p) else (r.randrange(0, 0x40) if b else r.randrange(0, 0x10) * 4)
        rd = r.choice(dest) if l else r.randrange(15)
        if l and (not p or w) and rd == base:
            rd = dest[0]
        return (c << 28) | (1 << 26) | (p << 24) | (u << 23) | (b << 22) | (w << 21) | (l << 20) | (base << 16) | (rd << 12) | off
    if k == 1:      # register offset (r12 holds a small multiple of 4)
        b = r.randrange(2)
        l = r.randrange(2)
        p, u = r.randrange(2), r.randrange(2)
        w = r.randrange(2) if p else 0
        rd = r.choice(dest) if l else r.randrange(15)
        sh = r.randrange(1, 3)
        return (c << 28) | (3 << 25) | (p << 24) | (u << 23) | (b << 22) | (w << 21) | (l << 20) | (base << 16) | (rd << 12) | (sh << 7) | 12
    if k == 2:      # halfword, signed and doubleword
        sh = r.randrange(1, 4)
        l = r.randrange(2)
        p, u = r.randrange(2), r.randrange(2)
        w = r.randrange(2) if p else 0
        if not l and sh in (2, 3):  # LDRD / STRD
            rd = r.choice([0, 2, 4, 6])
            off = r.randrange(0, 8) * 8
        else:
            rd = r.choice(dest) if l else r.randrange(15)
            off = r.randrange(0, 0x40) if sh == 2 else r.randrange(0, 0x20) * 2
            if w or not p:
                off = r.randrange(0, 8) * 8
        if (not p or w) and rd in (base, base + 1 if (not l and sh in (2, 3)) else -1):
            w, p = 0, 1
        return (c << 28) | (p << 24) | (u << 23) | (1 << 22) | (w << 21) | (l << 20) | (base << 16) | (rd << 12) | ((off >> 4) << 8) | 0x90 | (sh << 5) | (off & 0xF)
    if k == 3:      # LDM/STM
        l = r.randrange(2)
        pool = list(range(8)) if l else list(range(8)) + [12, 14]
        rl = 0
        for x in r.sample(pool, r.choice([2, 4])):
            rl |= 1 << x
        return (c << 28) | (4 << 25) | (r.randrange(2) << 24) | (r.randrange(2) << 23) | (r.randrange(2) << 21) | (l << 20) | (base << 16) | rl
    # SWP / SWPB
    return (c << 28) | 0x01000090 | (r.randrange(2) << 22) | (base << 16) | (r.choice(dest) << 12) | r.randrange(8)


def arm_snippet(r, mem):
    dest = list(range(8)) if mem else list(range(15))
    n = r.randrange(3, 13)
    words = []
    for i in range(n):
        k = r.random()
        if k < 0.08 and i < n - 1:
            words.append(('br', r.randrange(1, min(4, n - i) + 1), cond(r)))
            continue
        if mem and k < 0.55:
            words.append(a_mem(r, dest))
        elif k < 0.75:
            words.append(a_dp(r, dest))
        else:
            words.append(a_mul(r, dest))
    out = []
    for i, w in enumerate(words):
        if isinstance(w, tuple):
            _, ahead, c = w
            off = ahead - 1   # target = this + 4*ahead, encoded relative to pc = this + 8
            out.append((c << 28) | (0xA << 24) | ((off - 1) & 0xFFFFFF) if ahead >= 1 else 0)
        else:
            out.append(w)
    return b''.join(struct.pack('<I', w & 0xFFFFFFFF) for w in out) + struct.pack('<I', 0xEAFFFFFE)


# ---------------------------------------------------------------- Thumb generators
def t_alu(r, dest):
    k = r.randrange(5)
    rd, rs = r.choice(dest), r.randrange(8)
    if k == 0:
        return (r.randrange(3) << 11) | (r.randrange(32) << 6) | (rs << 3) | rd
    if k == 1:
        return 0x1800 | (r.randrange(4) << 9) | (r.randrange(8) << 6) | (rs << 3) | rd
    if k == 2:
        o = r.randrange(4)
        return 0x2000 | (o << 11) | (rd << 8) | r.randrange(256)
    if k == 3:
        return 0x4000 | (r.randrange(16) << 6) | (rs << 3) | rd
    # format 5: add / cmp / mov with high registers, never writing the PC
    o = r.randrange(3)
    rs = r.randrange(16)
    rd = r.choice(dest)
    if o == 1:
        rs = r.randrange(15)
    return 0x4400 | (o << 8) | ((rs >> 3) << 6) | ((rs & 7) << 3) | rd


def t_mem(r, dest):
    k = r.randrange(9)
    rb = r.choice([6, 7])
    rd = r.choice(dest)
    if k == 0:      # register offset (r5 holds a small multiple of 4)
        o = r.randrange(4)
        return 0x5000 | (o << 10) | (5 << 6) | (rb << 3) | (rd if o >= 2 else r.randrange(8))
    if k == 1:
        o = r.randrange(4)
        return 0x5200 | (o << 10) | (5 << 6) | (rb << 3) | (rd if o else r.randrange(8))
    if k == 2:
        b, l = r.randrange(2), r.randrange(2)
        return 0x6000 | (b << 12) | (l << 11) | (r.randrange(32) << 6) | (rb << 3) | (rd if l else r.randrange(8))
    if k == 3:
        l = r.randrange(2)
        return 0x8000 | (l << 11) | (r.randrange(32) << 6) | (rb << 3) | (rd if l else r.randrange(8))
    if k == 4:      # SP-relative
        l = r.randrange(2)
        return 0x9000 | (l << 11) | ((rd if l else r.randrange(8)) << 8) | r.randrange(32)
    if k == 5:      # load address
        return 0xA000 | (r.randrange(2) << 11) | (rd << 8) | r.randrange(256)
    if k == 6:      # adjust SP
        return 0xB000 | (r.randrange(2) << 7) | r.randrange(8)
    if k == 7:      # PUSH / POP (no PC)
        l = r.randrange(2)
        rl = 0
        for x in r.sample(dest, r.randrange(1, len(dest) + 1)):
            rl |= 1 << x
        if not l and r.randrange(2):
            rl |= 0x100     # push lr
        return 0xB400 | (l << 11) | rl
    l = r.randrange(2)       # LDMIA / STMIA
    rl = 0
    for x in r.sample(dest, r.randrange(1, len(dest) + 1)):
        rl |= 1 << x
    return 0xC000 | (l << 11) | (rb << 8) | rl


def thumb_snippet(r, mem):
    dest = [0, 1, 2, 3, 4] if mem else list(range(8))
    n = r.randrange(3, 16)
    hws = []
    for i in range(n):
        k = r.random()
        if k < 0.1 and i < n - 1:
            ahead = r.randrange(1, min(4, n - i) + 1)
            off = ahead - 2      # target = this + 2*ahead, relative to pc = this + 4
            if r.randrange(3):
                hws.append(0xD000 | (r.randrange(14) << 8) | (off & 0xFF))
            else:
                hws.append(0xE000 | (off & 0x7FF))
            continue
        if mem and k < 0.6:
            hws.append(t_mem(r, dest))
        else:
            hws.append(t_alu(r, dest))
    return b''.join(struct.pack('<H', x) for x in hws) + struct.pack('<H', 0xE7FE)


# ---------------------------------------------------------------- running
def make_cases(r, count):
    image = bytearray(DATA - BASE + DATA_LEN)
    data = bytes(r.getrandbits(8) for _ in range(DATA_LEN))
    image[DATA - BASE:] = data
    cases = []
    for i in range(count):
        start = BASE + i * SLOT
        thumb = r.random() < 0.45
        mem = r.random() < 0.5
        code = thumb_snippet(r, mem) if thumb else arm_snippet(r, mem)
        assert len(code) <= SLOT
        image[start - BASE:start - BASE + len(code)] = code
        exit_ = start + len(code) - (2 if thumb else 4)
        regs = [r.getrandbits(32) if r.random() < 0.8 else r.choice([0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, r.randrange(64)]) for _ in range(16)]
        if mem:
            if thumb:
                regs[6] = DATA + 0x400 + r.randrange(0, 0x40) * 4
                regs[7] = DATA + 0x900 + r.randrange(0, 0x40) * 4
                regs[5] = r.randrange(0, 16) * 4
                regs[13] = DATA + 0x600 + r.randrange(0, 0x40) * 4
            else:
                for k, p in enumerate(PTRS):
                    regs[p] = DATA + 0x400 + 0x200 * k
                regs[12] = r.randrange(0, 16) * 4
        regs[15] = 0
        cpsr = (r.getrandbits(5) << 27) | 0x1F | 0xC0
        cases.append((start, int(thumb), exit_, regs, cpsr, 0))
    return bytes(image), cases


def run_unicorn(image, cases):
    uc = unicorn.Uc(unicorn.UC_ARCH_ARM, unicorn.UC_MODE_ARM)
    uc.ctl_set_cpu_model(A.UC_CPU_ARM_946)
    uc.mem_map(BASE, 0x400000)
    uc.mem_write(BASE, image)
    data = image[DATA - BASE:]
    regs_ids = [getattr(A, f'UC_ARM_REG_R{i}') for i in range(13)] + [A.UC_ARM_REG_SP, A.UC_ARM_REG_LR]
    out = []
    for start, thumb, ex, regs, cpsr, _ in cases:
        uc.mem_write(DATA, data)
        uc.reg_write(A.UC_ARM_REG_CPSR, cpsr)
        for i, rid in enumerate(regs_ids):
            uc.reg_write(rid, regs[i])
        try:
            uc.emu_start(start | thumb, ex, count=2000)
            err = None
        except unicorn.UcError as e:
            err = str(e)
        vals = [uc.reg_read(rid) for rid in regs_ids]
        fl = uc.reg_read(A.UC_ARM_REG_CPSR) & 0xF8000000
        mem = bytes(uc.mem_read(DATA, DATA_LEN))
        hsh = 2166136261
        for b in mem:
            hsh = ((hsh ^ b) * 16777619) & 0xFFFFFFFF
        out.append((vals, fl, hsh, err))
    return out


FIELDS = [f'r{i}' for i in range(15)] + ['flags', 'data', 'pc', 'cpsr', 'spsr', 'banked registers', 'outside accesses', 'stopped']


def build_native(image, cases, work, arch=5):
    """Recompile the cases and run them as compiled code (A), interpreted (B) and mixed (C).
    Returns, per case, the final state of each run: {'A': [...], 'B': [...], 'C': [...]}, each
    list the 15 registers, the flags, the data hash and the fields in FIELDS."""
    prefix = 'g9' if arch == 5 else 'g7'
    prog = Program([('test', BASE, image[:len(cases) * SLOT], 'static')], arch=arch, prefix=prefix)
    prog.discover([(c[0], bool(c[1])) for c in cases])
    gen = os.path.join(work, 'gen')
    prog.emit(gen)
    srcs = [os.path.join(gen, f) for f in sorted(os.listdir(gen)) if f.endswith('.c')]
    exe = os.path.join(work, 'difftest')
    cmd = ['cc', '-O1', '-w', '-I', os.path.join(ROOT, 'runtime'), f'-DTABLE={prefix}_table', f'-DTABLE_LEN={prefix}_table_len',
           f'-DARCH={arch}', '-o', exe, os.path.join(HERE, 'difftest_main.c'), os.path.join(ROOT, 'runtime', 'cpu.c'),
           os.path.join(ROOT, 'runtime', 'interp.c')] + srcs
    subprocess.run(cmd, check=True)
    inp = os.path.join(work, 'cases.bin')
    with open(inp, 'wb') as f:
        f.write(struct.pack('<II', len(cases), len(image)))
        f.write(image)
        for start, thumb, ex, regs, cpsr, kind in cases:
            f.write(struct.pack('<4I', start, thumb, ex, kind))
            f.write(struct.pack('<16I', *regs))
            f.write(struct.pack('<I', cpsr))
    errs = []
    runs = {}
    ends = os.path.join(work, 'ends.bin')
    for run in 'ABC':
        res = subprocess.run([exe, inp, run] + ([ends] if run != 'A' else []), check=True, capture_output=True, text=True)
        errs += [l for l in res.stderr.splitlines() if l.strip() and not l.startswith('[ARM')]    # the runtime's own notes
        lines = {}
        for line in res.stdout.splitlines():
            t = line.split()
            lines[int(t[1])] = None if t[0] == 'S' else [int(x, 16) if k > 1 else x for k, x in enumerate(t)][2:]
        runs[run] = lines
        if run == 'A':
            with open(ends, 'wb') as f:
                for i, case in enumerate(cases):
                    # where it stopped; the others run on instead (0xFFFFFFFF) where A ran code at that address:
                    # a loop (it used up its budget), or its own start in the state it started in (an undefined
                    # instruction there). Its own start in the other state is a different place, which A had no
                    # code for, but the stop address can't say so and the others can't be run: 0xFFFFFFFE skips them.
                    pc, cpsr, stopped = lines[i][17], lines[i][18], lines[i][22]
                    at_start = pc == case[0]
                    same_state = (cpsr >> 5) & 1 == case[1]
                    f.write(struct.pack('<I', 0xFFFFFFFF if not stopped or (at_start and same_state) else
                                        0xFFFFFFFE if at_start else pc))
    out = []
    for i in range(len(cases)):
        a = runs['A'][i]
        out.append({r: (runs[r][i] if runs[r].get(i) is not None else a) for r in 'ABC'})
    return out, '\n'.join(errs)


def disasm(image, start, thumb, ex):
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB if thumb else capstone.CS_MODE_ARM)
    code = image[start - BASE:ex - BASE + 4]
    return '\n'.join(f'    {i.address:08x}: {i.bytes.hex():10s} {i.mnemonic} {i.op_str}' for i in md.disasm(code, start))


# ---------------------------------------------------------------- single random instructions
# (value, mask) of each ARM encoding class, in the order the decoders test them: the bits in the
# mask are fixed, the others random. Together they cover every class the runtime decodes,
# including the ones Unicorn can't judge. Bits 31-28 (the condition) are random where the
# mask leaves them free.
ARM_CLASSES = [
    (0x02000000, 0x0E000000),   # data processing, immediate
    (0x00000000, 0x0E000010),   # data processing, shift by immediate
    (0x00000010, 0x0E000090),   # data processing, shift by register
    (0x00000090, 0x0FC000F0),   # MUL, MLA
    (0x00800090, 0x0F8000F0),   # UMULL, UMLAL, SMULL, SMLAL
    (0x01000080, 0x0F900090),   # SMLAxy, SMLAWy, SMULWy, SMLALxy, SMULxy
    (0x01000050, 0x0F900FF0),   # QADD, QSUB, QDADD, QDSUB
    (0x016F0F10, 0x0FFF0FF0),   # CLZ
    (0x012FFF10, 0x0FFFFFF0),   # BX
    (0x012FFF30, 0x0FFFFFF0),   # BLX register
    (0x01200070, 0x0FF000F0),   # BKPT
    (0x010F0000, 0x0FBF0FFF),   # MRS
    (0x0120F000, 0x0FB0FFF0),   # MSR, register
    (0x0320F000, 0x0FB0F000),   # MSR, immediate
    (0x01000090, 0x0FB00FF0),   # SWP, SWPB
    (0x00000090, 0x0E000090),   # halfword, signed and doubleword transfers (SH 0 is undefined)
    (0x04000000, 0x0E000000),   # LDR, STR, LDRB, STRB, immediate offset
    (0x06000000, 0x0E000010),   # LDR, STR, LDRB, STRB, shifted register offset
    (0x06000010, 0x0E000010),   # (undefined: the media space)
    (0x08000000, 0x0E000000),   # LDM, STM
    (0x0A000000, 0x0E000000),   # B, BL
    (0xFA000000, 0xFE000000),   # BLX immediate
    (0x0F000000, 0x0F000000),   # SWI
    (0x0E000F10, 0x0F000F10),   # MRC, MCR to CP15
    (0x0E000000, 0x0F000000),   # coprocessor data processing and transfers (other coprocessors)
    (0x0C000000, 0x0E000000),   # LDC, STC
    (0xF0000000, 0xF0000000),   # the unconditional space: PLD, BLX, the rest undefined
    (0x00000000, 0x00000000),   # anything
]
MODES = [0x10, 0x11, 0x12, 0x13, 0x17, 0x1B, 0x1F]
SAFE_SWI = [0x03, 0x04, 0x05, 0x06, 0x07, 0x09, 0x0D, 0x0F, 0x1F, 0xEE]     # none reaches a long loop


def fuzz_arm(r):
    v, m = r.choice(ARM_CLASSES)
    w = v | (r.getrandbits(32) & ~m & 0xFFFFFFFF)
    if not m >> 28 and r.random() < 0.6:
        w = (w & 0x0FFFFFFF) | 0xE0000000
    if (w & 0x0F000000) == 0x0F000000 and w >> 28 != 0xF:
        w = (w & 0xFF00FFFF) | (r.choice(SAFE_SWI) << 16)
    if (w & 0x0E000000) == 0x0A000000:      # a branch: to its own sentinel or itself (the rest of the slot is
        if r.random() < 0.5:                # padding), or well outside the image
            w = (w & 0xFF000000) | r.choice([0xFFFFFE, 0xFFFFFF])
        else:
            w = (w & 0xFF00FFFF) | (r.randrange(1, 15) << 20) | (w & 0xFFFFF)
    return [w]


def fuzz_thumb(r):
    k = r.random()
    if k < 0.12:        # BL or BLX pair
        return [0xF000 | r.getrandbits(11), (0xF800 if r.random() < 0.7 else 0xE800) | r.getrandbits(11)]
    if k < 0.16:        # a prefix or suffix on its own
        return [r.choice([0xF000, 0xF800, 0xE800]) | r.getrandbits(11)]
    hw = r.getrandbits(16)
    if hw >> 8 == 0xDF:
        hw = 0xDF00 | r.choice(SAFE_SWI)
    return [hw]


def make_fuzz_cases(r, count, arch):
    """One random instruction per slot, then a branch to itself; the rest of the slot is that
    branch too (ARM and Thumb), so a branch that lands anywhere near stops there."""
    image = bytearray(DATA - BASE + DATA_LEN)
    image[DATA - BASE:] = bytes(r.getrandbits(8) for _ in range(DATA_LEN))
    cases = []
    for i in range(count):
        start = BASE + i * SLOT
        thumb = r.random() < 0.5
        if thumb:
            hws = fuzz_thumb(r)
            code = b''.join(struct.pack('<H', x) for x in hws)
            pad = struct.pack('<H', 0xE7FE)
        else:
            code = b''.join(struct.pack('<I', x) for x in fuzz_arm(r))
            pad = struct.pack('<I', 0xEAFFFFFE)
        slot = code + pad * ((SLOT - len(code)) // len(pad))
        image[start - BASE:start - BASE + SLOT] = slot
        ptr = r.random() < 0.6
        regs = []
        for _ in range(16):
            if ptr and r.random() < 0.5:
                regs.append(DATA + 0x400 + r.randrange(0, 0x180) * 4 + (r.randrange(4) if r.random() < 0.2 else 0))
            elif r.random() < 0.8:
                regs.append(r.getrandbits(32))
            else:
                regs.append(r.choice([0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, r.randrange(64)]))
        regs[15] = 0
        cpsr = (r.getrandbits(5) << 27) | r.choice(MODES) | (r.getrandbits(2) << 6)
        cases.append((start, int(thumb), start + len(code), regs, cpsr, 1))
    return bytes(image), cases


def compare(case, image, a, b, label_a, label_b, fields, show, counter):
    """Compare two runs' states on `fields` (indices into FIELDS); print the first few differences."""
    diff = [k for k in fields if a[k] != b[k]]
    if not diff:
        return False
    counter[0] += 1
    if counter[0] <= show:
        start, thumb, ex, regs, cpsr, kind = case
        print(f'case at {start:08x} ({"Thumb" if thumb else "ARM"}): {label_a} and {label_b} differ; initial cpsr {cpsr:08x}')
        print(disasm(image, start, thumb, ex + (4 if kind and not thumb else 2)))
        for k in diff:
            ini = f' (in {regs[k]:08x})' if k < 15 else ''
            print(f'    {FIELDS[k]}{ini}: {label_a} {a[k]:08x} {label_b} {b[k]:08x}')
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cases', type=int, default=3000)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--show', type=int, default=5)
    ap.add_argument('--fuzz', type=int, help='single random instructions per CPU (default: as many as --cases)')
    args = ap.parse_args()
    r = random.Random(args.seed)
    count = min(args.cases, (DATA - BASE) // SLOT)
    image, cases = make_cases(r, count)
    ref = run_unicorn(image, cases)
    with tempfile.TemporaryDirectory() as work:
        nat, err = build_native(image, cases, work)
    if err.strip():
        print(err[:2000])
    bad = [0]
    skipped = 0
    UNI = list(range(15)) + [15, 16]        # what Unicorn gives: registers, flags, data
    ALL = list(range(len(FIELDS)))
    for i, (case, (rv, rf, rh, rerr), n) in enumerate(zip(cases, ref, nat)):
        # the three runs of the same code agree with each other, whatever Unicorn did
        bad_before = bad[0]
        compare(case, image, n['A'], n['B'], 'compiled', 'interpreted', ALL, args.show, bad)
        compare(case, image, n['A'], n['C'], 'compiled', 'mixed', ALL, args.show, bad)
        if rerr:
            skipped += 1
            if os.environ.get('SHOW_SKIPPED') and skipped <= 3:
                start, thumb, ex, regs, cpsr, _ = case
                print('skipped', i, rerr); print(disasm(image, start, thumb, ex))
            continue
        for tag, name in (('A', 'compiled'), ('B', 'interpreted'), ('C', 'mixed')):
            v = n[tag]
            uni = rv + [rf, rh]
            if bad[0] == bad_before and compare(case, image, uni, v[:17], 'unicorn', name, range(17), args.show, bad):
                break
    print(f'{count} cases: {count - bad[0] - skipped} match, {bad[0]} differ, {skipped} skipped (Unicorn faulted); '
          f'each run compiled, interpreted and mixed')
    total_bad = bad[0]
    fuzz = args.cases if args.fuzz is None else args.fuzz
    fuzz = min(fuzz, (DATA - BASE) // SLOT)
    for arch, name in ((5, 'ARMv5TE'), (4, 'ARMv4T')):
        if not fuzz:
            break
        fr = random.Random(args.seed * 1000 + arch)
        fimage, fcases = make_fuzz_cases(fr, fuzz, arch)
        with tempfile.TemporaryDirectory() as work:
            nat, err = build_native(fimage, fcases, work, arch)
        if err.strip():
            print(err[:2000])
        fbad = [0]
        for case, n in zip(fcases, nat):
            compare(case, fimage, n['A'], n['B'], 'compiled', 'interpreted', ALL, args.show, fbad)
        print(f'{name}: {fuzz} random instructions: {fuzz - fbad[0]} match, {fbad[0]} differ (compiled and interpreted)')
        total_bad += fbad[0]
    return 1 if total_bad else 0


if __name__ == '__main__':
    sys.exit(main())
