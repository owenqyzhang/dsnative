"""Differential test of the recompiler against Unicorn's ARM946 (QEMU's ARMv5TE model).

Builds thousands of random snippets of ARM and Thumb code (data processing, multiplies,
the ARMv5TE DSP instructions, loads and stores of every kind, block transfers, conditional
branches), recompiles them to C with the real recompiler, runs them natively and in Unicorn
from the same random state, and compares the registers, the flags and a 4 KB data area.

    python3 tests/difftest.py [--cases N] [--seed S]
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
        cases.append((start, int(thumb), exit_, regs, cpsr))
    return bytes(image), cases


def run_unicorn(image, cases):
    uc = unicorn.Uc(unicorn.UC_ARCH_ARM, unicorn.UC_MODE_ARM)
    uc.ctl_set_cpu_model(A.UC_CPU_ARM_946)
    uc.mem_map(BASE, 0x400000)
    uc.mem_write(BASE, image)
    data = image[DATA - BASE:]
    regs_ids = [getattr(A, f'UC_ARM_REG_R{i}') for i in range(13)] + [A.UC_ARM_REG_SP, A.UC_ARM_REG_LR]
    out = []
    for start, thumb, ex, regs, cpsr in cases:
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


def build_native(image, cases, work):
    prog = Program([('test', BASE, image[:len(cases) * SLOT], 'static')], arch=5, prefix='g9')
    prog.discover([(s, bool(t)) for s, t, *_ in cases])
    gen = os.path.join(work, 'gen')
    prog.emit(gen)
    srcs = [os.path.join(gen, f) for f in sorted(os.listdir(gen)) if f.endswith('.c')]
    exe = os.path.join(work, 'difftest')
    cmd = ['cc', '-O1', '-w', '-I', os.path.join(ROOT, 'runtime'), '-o', exe,
           os.path.join(HERE, 'difftest_main.c'), os.path.join(ROOT, 'runtime', 'cpu.c')] + srcs
    subprocess.run(cmd, check=True)
    inp = os.path.join(work, 'cases.bin')
    with open(inp, 'wb') as f:
        f.write(struct.pack('<II', len(cases), len(image)))
        f.write(image)
        for start, thumb, ex, regs, cpsr in cases:
            f.write(struct.pack('<3I', start, thumb, ex))
            f.write(struct.pack('<16I', *regs))
            f.write(struct.pack('<I', cpsr))
    res = subprocess.run([exe, inp], check=True, capture_output=True, text=True)
    out = []
    for line in res.stdout.splitlines():
        v = [int(x, 16) for x in line.split()]
        out.append((v[:15], v[15], v[16]))
    return out, res.stderr


def disasm(image, start, thumb, ex):
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB if thumb else capstone.CS_MODE_ARM)
    code = image[start - BASE:ex - BASE + 4]
    return '\n'.join(f'    {i.address:08x}: {i.bytes.hex():10s} {i.mnemonic} {i.op_str}' for i in md.disasm(code, start))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cases', type=int, default=3000)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--show', type=int, default=5)
    args = ap.parse_args()
    r = random.Random(args.seed)
    count = min(args.cases, (DATA - BASE) // SLOT)
    image, cases = make_cases(r, count)
    ref = run_unicorn(image, cases)
    with tempfile.TemporaryDirectory() as work:
        nat, err = build_native(image, cases, work)
    if err.strip():
        print(err[:2000])
    bad = 0
    skipped = 0
    for i, (case, (rv, rf, rh, rerr), (nv, nf, nh)) in enumerate(zip(cases, ref, nat)):
        if rerr:
            skipped += 1
            if os.environ.get('SHOW_SKIPPED') and skipped <= 3:
                start, thumb, ex, regs, cpsr = case
                print('skipped', i, rerr); print(disasm(image, start, thumb, ex))
            continue
        if rv != nv or rf != nf or rh != nh:
            bad += 1
            if bad <= args.show:
                start, thumb, ex, regs, cpsr = case
                print(f'case {i} ({"Thumb" if thumb else "ARM"}) differs; initial cpsr {cpsr:08x}')
                print(disasm(image, start, thumb, ex))
                for k in range(15):
                    if rv[k] != nv[k]:
                        print(f'    r{k}: in {regs[k]:08x} unicorn {rv[k]:08x} native {nv[k]:08x}')
                if rf != nf:
                    print(f'    flags: unicorn {rf:08x} native {nf:08x}')
                if rh != nh:
                    print('    data area differs')
    print(f'{count} cases: {count - bad - skipped} match, {bad} differ, {skipped} skipped (Unicorn faulted)')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
