"""Where a computed jump goes, found by running the code before it.

Compilers build switches in many encodings (ARM add pc, pc, rI, lsl #2 over a run of
branches; ldr pc from a table of addresses; GCC's byte and halfword offset tables;
CodeWarrior's Thumb switch with signed halfword offsets; the SDK's add pc, pc, r of a
halfword it loaded). Rather than one pattern each, this module evaluates them:

1. The path: the straight-line code that falls into the jump, read backwards from memory
   (so it doesn't matter which walk reached the jump first). It goes through conditional
   branches not taken, and through a conditional branch taken over a `b default`, and
   stops at a call, a return or an unconditional branch. A second reading, tried only when
   the first finds no table, also goes through a branch taken over a `bl`, taking the call
   for a failed assert that never returns.
2. One run of the path in which every unknown value is a symbol: a register on entry, or
   what a load from unknown memory or an operation discovery doesn't model produced.
   Moves and additions of constants keep a symbol (as symbol + constant), so a guard
   `cmp rI, #n; bhi default` becomes a range of values for rI's symbol. Several guards on
   one symbol (signed checks: `cmp r, #n; bgt; cmp r, #0; blt`) intersect.
3. If the jump's target is known already, it is a constant (ldr r3, =main; blx r3).
   Otherwise, for the symbol whose guard is nearest the jump, the path is run again for
   every value in its range, with everything else still unknown: data processing with
   shifts, pc-relative addresses and loads of any width from the ROM image. Each run
   must end at a known address, or the jump is not a table. Tests for equality don't count
   as bounds, and don't take a value out of the table: they hold only if the branch is the
   one way into the code after it, which the path can't know.
"""

M = 0xFFFFFFFF
MAX_CASES = 1025
PATH_LEN = 32


class Sym:
    """An unknown value plus a constant: the value made at `var` (instruction index and
    slot, or (-1, register) on entry to the path), + c."""
    __slots__ = ('var', 'c')

    def __init__(self, var, c=0):
        self.var, self.c = var, c & M

    def __eq__(self, o):
        return isinstance(o, Sym) and o.var == self.var and o.c == self.c

    def __hash__(self):
        return hash((self.var, self.c))


def _add(a, b):
    if isinstance(a, int):
        if isinstance(b, int):
            return (a + b) & M
        return Sym(b.var, b.c + a) if isinstance(b, Sym) else None
    if isinstance(a, Sym) and isinstance(b, int):
        return Sym(a.var, a.c + b)
    return None


def _sub(a, b):
    if isinstance(b, int):
        if isinstance(a, int):
            return (a - b) & M
        return Sym(a.var, a.c - b) if isinstance(a, Sym) else None
    return None


def _ints(*v):
    return all(isinstance(x, int) for x in v)


def _shift_imm(v, st, amt, cin):
    """An immediate shift (as the ARM shifter encodes it): the value and carry out."""
    if st == 0 and amt == 0:
        return v, cin
    if not isinstance(v, int):
        return None, None
    if st == 0:
        return (v << amt) & M, (v >> (32 - amt)) & 1
    if st == 3 and amt == 0:            # rrx
        return (None, None) if cin is None else ((cin << 31) | (v >> 1), v & 1)
    if st == 3:
        return ((v >> amt) | (v << (32 - amt))) & M, (v >> (amt - 1)) & 1
    amt = amt or 32
    if st == 1:
        return (v >> amt if amt < 32 else 0), (v >> (amt - 1)) & 1
    neg = v >> 31
    if amt >= 32:
        return (M if neg else 0), neg
    return ((v - (neg << 32)) >> amt) & M, (v >> (amt - 1)) & 1


def _shift_reg(v, st, amt, cin):
    """A shift by a register: the bottom byte of amt."""
    if isinstance(amt, int) and amt & 0xFF == 0:
        return v, cin
    if not _ints(v, amt):
        return None, None
    amt &= 0xFF
    if st == 3:
        amt &= 31
        return (v, v >> 31) if amt == 0 else (((v >> amt) | (v << (32 - amt))) & M, (v >> (amt - 1)) & 1)
    if amt < 32:
        return _shift_imm(v, st, amt, cin)
    if st == 0:
        return 0, (v & 1 if amt == 32 else 0)
    if st == 1:
        return 0, (v >> 31 if amt == 32 else 0)
    return (M if v >> 31 else 0), v >> 31


# -- conditions as sets of values -----------------------------------------------------
def _iv(start, count):
    """`count` values from `start`, wrapping at 2^32, as sorted (lo, hi) intervals."""
    if count <= 0:
        return []
    if count >= 1 << 32:
        return [(0, M)]
    start &= M
    end = start + count - 1
    return [(start, end)] if end <= M else [(0, end - (1 << 32)), (start, M)]


def _t_set(cc, imm, op):
    """The values t for which condition cc holds on the flags of t - imm (op 'sub': cmp,
    subs), t + imm ('add': cmn, adds) or t ('mov': movs, N and Z only). None: no
    constraint known. MI and PL are taken as the signed order, as compilers use them
    (subs r, #k; bmi for r < k): the values for which the subtraction overflows, which
    the source's arithmetic doesn't have, are left out."""
    if op == 'mov':
        if cc not in (0, 1, 4, 5):
            return None
        op, imm = 'sub', 0
    if op == 'add':
        if cc in (2, 3, 8, 9):          # the carry of t + imm: t >= 2^32 - imm
            if cc == 2:
                return _iv(-imm, imm)
            if cc == 3:
                return _iv(0, (1 << 32) - imm)
            if cc == 8:
                return _iv(1 - imm, imm - 1)
            return _iv(0, (1 << 32) - imm + 1) if imm else [(0, M)]
        if imm == 0x80000000 and cc in (6, 7, 10, 11, 12, 13):
            return None
        op, imm = 'sub', -imm & M       # N, Z and the signed order are those of t - (-imm)
    S = 1 << 31
    sim = imm ^ S                       # t ^ 2^31 puts the signed order in unsigned order
    return {0: lambda: [(imm, imm)], 1: lambda: _iv(imm + 1, M),
            2: lambda: _iv(imm, (1 << 32) - imm), 3: lambda: _iv(0, imm),
            4: lambda: _iv(S, sim), 5: lambda: _iv(imm, (1 << 32) - sim),     # as lt, ge
            8: lambda: _iv(imm + 1, M - imm), 9: lambda: _iv(0, imm + 1),
            10: lambda: _iv(imm, (1 << 32) - sim), 11: lambda: _iv(S, sim),
            12: lambda: _iv(imm + 1, M - sim), 13: lambda: _iv(S, sim + 1)}.get(cc, lambda: None)()


def _norm(ivs):
    out = []
    for lo, hi in sorted(ivs):
        if out and lo <= out[-1][1] + 1:
            out[-1] = (out[-1][0], max(out[-1][1], hi))
        else:
            out.append((lo, hi))
    return out


def _complement(ivs):
    out, at = [], 0
    for lo, hi in _norm(ivs):
        if lo > at:
            out.append((at, lo - 1))
        at = hi + 1
    if at <= M:
        out.append((at, M))
    return out


def _intersect(a, b):
    out = []
    for lo, hi in a:
        for lo2, hi2 in b:
            if max(lo, lo2) <= min(hi, hi2):
                out.append((max(lo, lo2), min(hi, hi2)))
    return _norm(out)


def cond_set(cc, c, imm, op, want=True):
    """The values of a symbol (holding symbol + c when the flags were set) for which cc
    is `want`."""
    ts = _t_set(cc, imm & M, op)
    if ts is None:
        return None
    if not want:
        ts = _complement(ts)
    return _norm([iv for lo, hi in ts for iv in _iv(lo - c, hi - lo + 1)])


def _holds(cc, f):
    n, z, c, v = f
    need = {0: (z,), 1: (z,), 2: (c,), 3: (c,), 4: (n,), 5: (n,), 6: (v,), 7: (v,), 8: (c, z), 9: (c, z),
            10: (n, v), 11: (n, v), 12: (z, n, v), 13: (z, n, v)}[cc]
    if any(x is None for x in need):
        return None
    return bool({0: z, 1: not z, 2: c, 3: not c, 4: n, 5: not n, 6: v, 7: not v, 8: c and not z, 9: not c or z,
                 10: n == v, 11: n != v, 12: not z and n == v, 13: z or n != v}[cc])


def cond_code(ins):
    """The condition (0-13), or 14 for always (Thumb swi and undefined, ARM's 0xF space)."""
    cc = (ins.word >> 8) & 0xF if ins.thumb and (ins.word & 0xF000) == 0xD000 and not ins.pair else \
        14 if ins.thumb else ins.word >> 28
    return 14 if cc >= 14 else cc


# -- the jumps ----------------------------------------------------------------------
def jump_kind(ins):
    """'dp' (a data-processing write to the PC), 'ldr' (a load to it), 'bx' or 'blx' for
    a jump to a computed address that may be a table or a constant; None for returns."""
    w = ins.word
    if ins.thumb:
        if ins.pair or (w >> 10) != 0x11:
            return None
        o, rs, rd = (w >> 8) & 3, (w >> 3) & 0xF, (w & 7) | ((w >> 4) & 8)
        if o == 3:
            return None if rs in (14, 15) else ('blx' if w & 0x80 else 'bx')
        if o in (0, 2) and rd == 15 and not (o == 2 and rs == 14):
            return 'dp'
        return None
    if w >> 28 == 0xF:
        return None
    if (w & 0x0FFFFFD0) == 0x012FFF10:
        return None if (w & 0xF) == 14 else ('blx' if w & 0x20 else 'bx')
    op = (w >> 25) & 7
    if op in (0, 1) and ((w >> 12) & 0xF) == 15 and not (op == 0 and (w & 0x90) == 0x90):
        opc = (w >> 21) & 0xF
        if w & (1 << 20) or 8 <= opc <= 11:
            return None     # an exception return (subs pc, lr, #4)
        reads = set() if opc in (13, 15) else {(w >> 16) & 0xF}
        if not op:
            reads.add(w & 0xF)
            if w & 0x10:
                reads.add((w >> 8) & 0xF)
        return None if reads <= {14} else 'dp'
    if op in (2, 3) and (w & (1 << 20)) and not (w & (1 << 22)) and ((w >> 12) & 0xF) == 15 \
            and not (op == 3 and w & 0x10):
        return None if ((w >> 16) & 0xF) == 13 else 'ldr'
    return None


def table_form(ins):
    """The pc-relative jumps that only a table explains: 'addpc' for ARM add pc, pc, rI,
    lsl #2 (which may also enter an unrolled loop), 'table' for the other forms."""
    w = ins.word
    if ins.thumb:
        return 'table' if jump_kind(ins) == 'dp' and (w >> 8) & 3 == 0 and (w >> 3) & 0xF != 15 else None
    if (w & 0x0FEFF010) == 0x008FF000:          # add pc, pc, rI, shift #n
        return 'addpc' if (w & 0xFF0) in (0x100, 0x180, 0x200) else 'table'     # lsl #2, #3, #4
    if (w & 0x0F7FF010) == 0x071FF000 and jump_kind(ins) == 'ldr':    # ldr pc, [pc, rI, shift #n]
        return 'table'
    return None


def _prev(prog, seg, cur, thumb):
    """The instruction just before cur (a Thumb BL pair as one)."""
    if thumb:
        if not seg.contains(cur - 2, 2):
            return None
        if (seg.u16(cur - 2) >> 11) in (0x1D, 0x1F):
            return prog.decode(seg, cur - 4, True) if seg.contains(cur - 4, 2) else None
        return prog.decode(seg, cur - 2, True)
    return prog.decode(seg, cur - 4, False) if cur - 4 >= seg.addr else None


def build_path(prog, seg, ins, limit=PATH_LEN, assertion=False):
    """[(insn, required outcome)] ending at ins: True for a branch that must be taken or a
    conditional jump that must execute, False for a branch that must not be taken, None
    for no requirement. The path goes back while the code before falls into it, and
    starts at a call or a return, at a branch to an instruction of the path (a join: what
    came before is one way here of several), or at the head of a loop the path leaves
    through its back edge (the path is the loop's last run). When the instruction before
    doesn't fall through (b default, an early return), the path goes on from the branch
    to here just before it, taken (cmp r, #n; bls 1f; b default; 1:).

    A call skipped by the branch before it (cmp r, #n; bls 1f; bl f; 1:) is a join too,
    for the call may return. With `assertion`, f is taken as one that doesn't (a failed
    assert), and the path goes on from the branch, taken."""
    thumb = ins.thumb
    path = [(ins, True if cond_code(ins) != 14 else None)]
    cur = ins.addr
    head = None

    def branch_here(p, reach):
        q = p
        for _ in range(reach):
            q = _prev(prog, seg, q.addr, thumb)
            if q is None or q.flow == 'jump' and q.target == cur and q.target_thumb == thumb:
                return q
        return None

    while len(path) < limit and (head is None or cur > head):
        p = _prev(prog, seg, cur, thumb)
        if p is None or p.next != cur or p.flow == 'stop':
            break
        if p.flow == 'jump' and p.target_thumb == thumb and any(e[0].addr == p.target for e in path[:-1]):
            path = [e for e in path if e[0].addr >= p.target]
            break
        if p.flow == 'jump' and p.cond is not None and p.target < cur:
            head = p.target if head is None else max(head, p.target)
        falls = p.flow in ('seq', 'split') or p.cond is not None and p.flow in ('jump', 'ret') \
            or p.flow == 'jump' and p.target == cur
        if not falls:
            call = p.flow in ('call', 'icall')
            q = branch_here(p, 1 if call else 8) if assertion or not call else None
            if q is None:
                break
            path.append((q, True))
            cur = q.addr
            continue
        if p.cond is not None and p.flow in ('jump', 'ret'):
            path.append((p, None if p.flow == 'jump' and p.target == cur else False))
        else:
            path.append((p, None))
        cur = p.addr
    path.reverse()
    return path


class _Run:
    """One run of a path. With `chosen` set, the symbol made there is `value` instead.

    Memory is the ROM image, but a word at a fixed address is only a constant when it is a
    literal (read pc-relative): anything else at an address known without the index is a
    variable, whose value in the image is only its first. The first run (nothing chosen)
    finds those loads; the runs for each index value leave them unknown too."""

    def __init__(self, prog, seg, chosen=None, value=0, fixed=None, ignore=()):
        self.prog, self.seg, self.arch = prog, seg, prog.arch
        self.chosen, self.value = chosen, value
        self.ignore = ignore        # path indices of tests whose outcome is not required
        self.fixed = set() if fixed is None else fixed     # path indices of variable loads
        self.i = -1
        self.r = [self.fresh(n) for n in range(16)]
        self.f = None           # None, (n, z, c, v) with None for unknown, or ('s', var, c, imm, op)
        self.pcval = None
        self.constraints = []   # (var, set of values, path index)
        self.infeasible = False
        self.bad = None         # the path index where it turned out infeasible
        self.mem = {}           # (address, size, signed) -> value, since the last store

    def fresh(self, slot):
        k = (self.i, slot)
        return self.value if k == self.chosen else Sym(k)

    def set(self, n, v):
        if n == 15:
            self.pcval = v
        else:
            self.r[n] = self.fresh(n) if v is None else v

    def carry(self):
        return self.f[2] if self.f is not None and len(self.f) == 4 else None

    # -- flags --
    def flags_sub(self, a, b):
        if _ints(a, b):
            r = (a - b) & M
            self.f = (r >> 31, int(r == 0), int(a >= b), ((a ^ b) & (a ^ r)) >> 31)
        else:
            self.f = ('s', a.var, a.c, b, 'sub') if isinstance(a, Sym) and isinstance(b, int) else None

    def flags_add(self, a, b):
        if _ints(a, b):
            r = (a + b) & M
            self.f = (r >> 31, int(r == 0), int(a + b > M), (~(a ^ b) & (a ^ r) & M) >> 31)
        elif isinstance(a, Sym) and isinstance(b, int):
            self.f = ('s', a.var, a.c, b, 'add')
        elif isinstance(b, Sym) and isinstance(a, int):
            self.f = ('s', b.var, b.c, a, 'add')
        else:
            self.f = None

    def flags_logic(self, r, c):
        old = self.f if self.f is not None and len(self.f) == 4 else (None,) * 4
        if isinstance(r, int):
            self.f = (r >> 31, int(r == 0), c, old[3])
        else:
            self.f = ('s', r.var, r.c, 0, 'mov') if isinstance(r, Sym) else None

    def flags_carry_op(self, r, a, b, sub):
        """adc, sbc, rsc: known only when everything is."""
        self.f = None
        if _ints(r, a, b):
            self.f = (r >> 31, int(r == 0), None, None)

    def cond(self, cc):
        if cc == 14:
            return True
        f = self.f
        if f is None:
            return None
        return f if len(f) == 5 else _holds(cc, f)

    def require(self, cc, want):
        h = self.cond(cc)
        if h is None:
            return
        if h is True or h is False:
            if h != want and not self.infeasible and self.i not in self.ignore:
                self.infeasible, self.bad = True, self.i
            return
        _, var, c, imm, op = h
        s = cond_set(cc, c, imm, op, want)
        if s is not None:
            self.constraints.append((var, s, self.i))

    # -- memory: the ROM image --
    def read(self, a, n):
        seg = self.seg
        if not seg.contains(a, n):
            cands = [s for s in self.prog.segs_for(a, seg) if s.contains(a, n)]
            vals = {bytes(s.data[a - s.addr:a - s.addr + n]) for s in cands}
            if len(vals) != 1:
                return None
            return int.from_bytes(vals.pop(), 'little')
        return int.from_bytes(seg.data[a - seg.addr:a - seg.addr + n], 'little')

    def load(self, a, size, signed=False, rotate=False, pcrel=False):
        if not isinstance(a, int):
            return None
        if not pcrel:
            if self.chosen is None:
                self.fixed.add(self.i)
            if self.i in self.fixed:
                return None
        if size == 4:
            v = self.read(a & ~3, 4)
            if v is not None and rotate and a & 3:
                s = (a & 3) * 8
                v = ((v >> s) | (v << (32 - s))) & M
            return v
        if size == 2 and a & 1 and self.arch < 5:   # the ARM7: ldrh rotates, ldrsh loads a byte
            if signed:
                size = 1
            else:
                v = self.read(a & ~1, 2)
                return None if v is None else ((v >> 8) | (v << 24)) & M
        v = self.read(a & ~1 if size == 2 else a, size)
        if v is not None and signed and v >> (8 * size - 1):
            v = (v - (1 << (8 * size))) & M
        return v

    def ld(self, rd, a, size, signed=False, rotate=False, pcrel=False):
        """rd = a load. Loads of one address with no store between read one value, and a
        word load reads the word stored there (an index spilled to the stack, reloaded)."""
        k = None if a is None else (a, size, signed)
        if k in self.mem:
            return self.set(rd, self.mem[k])
        self.set(rd, self.load(a, size, signed, rotate, pcrel))
        if k is not None:
            self.mem[k] = self.pcval if rd == 15 else self.r[rd]

    def st(self, a=None, v=None):
        """A store: it may change any memory; a word stored at a known address is known."""
        self.mem.clear()
        if a is not None and v is not None:
            self.mem[(a, 4, False)] = v

    def clobber(self, regs=range(16)):
        for n in regs:
            self.set(n, None)
        self.f = None
        self.mem.clear()

    # -- running --
    def run(self, path):
        last = len(path) - 1
        for i, (ins, req) in enumerate(path):
            self.i = i
            cc = cond_code(ins)
            if i < last and ins.flow in ('jump', 'ret'):
                if req is not None:
                    self.require(cc, req)
            elif i == last:
                if cc != 14:
                    self.require(cc, True)
                self.exec(ins)
            elif cc != 14 and (h := self.cond(cc)) is not True:
                if h is False:
                    continue
                r, f = list(self.r), self.f     # may or may not run: what it changes is unknown
                self.exec(ins)
                for n in range(16):
                    if self.r[n] != r[n]:
                        self.r[n] = self.fresh(('m', n))
                if self.f != f:
                    self.f = None
            else:
                self.exec(ins)
            if self.infeasible:
                return self

        return self

    def exec(self, ins):
        if ins.thumb:
            self.thumb(ins)
        else:
            self.arm(ins.word, ins.addr)

    def arm(self, w, a):
        r = self.r
        op = (w >> 25) & 7
        if w >> 28 == 0xF:
            if (w & 0x0D70F000) != 0x0550F000:  # anything but pld
                self.clobber()
            return
        if op in (0, 1) and not (op == 0 and (w & 0x90) == 0x90):
            return self.arm_dp(w, a)
        rn, rd = (w >> 16) & 0xF, (w >> 12) & 0xF
        pc = a + 8
        if op == 0:
            if (w & 0x60) == 0:
                if (w & 0x0FC000F0) == 0x00000090:      # mul, mla
                    vm, vs = r[w & 0xF], r[(w >> 8) & 0xF]
                    res = (vm * vs) & M if _ints(vm, vs) else None
                    if w & (1 << 21):
                        res = _add(res, r[rd]) if res is not None else None
                    if w & (1 << 20):
                        self.f = None
                    return self.set(rn, res)
                from .program import arm_writes
                self.clobber(arm_writes(w) or range(16))
                return
            sh = (w >> 5) & 3                           # ldrh, ldrsb, ldrsh, ldrd, strh, strd
            off = ((w >> 4) & 0xF0) | (w & 0xF) if w & (1 << 22) else r[w & 0xF]
            base = pc if rn == 15 else r[rn]
            ea = _add(base, off) if w & (1 << 23) else _sub(base, off)
            at = ea if w & (1 << 24) else base
            wb = (not (w & (1 << 24)) or w & (1 << 21)) and rn != 15
            if wb:
                self.set(rn, ea)
            if w & (1 << 20):
                return self.ld(rd, at, 1 if sh == 2 else 2, signed=sh != 1, pcrel=rn == 15)
            if sh == 2:                                 # ldrd
                self.ld(rd, at, 4, pcrel=rn == 15)
                self.ld(rd + 1, _add(at, 4), 4, pcrel=rn == 15)
            else:
                self.st()
            return
        if op in (2, 3):
            if op == 3 and w & 0x10:
                return self.clobber()
            if op == 3:
                rm = w & 0xF
                off, _ = _shift_imm(pc if rm == 15 else r[rm], (w >> 5) & 3, (w >> 7) & 0x1F, self.carry())
            else:
                off = w & 0xFFF
            base = pc if rn == 15 else r[rn]
            ea = _add(base, off) if w & (1 << 23) else _sub(base, off)
            at = ea if w & (1 << 24) else base
            v = r[rd] if rd != 15 else None
            if (not (w & (1 << 24)) or w & (1 << 21)) and rn != 15:
                self.set(rn, ea)
            byte = w & (1 << 22)
            if w & (1 << 20):
                self.ld(rd, at, 1 if byte else 4, rotate=not byte, pcrel=rn == 15)
            else:
                self.st(None if byte else at, v)
            return
        if op == 4:
            if w & (1 << 21):
                n = bin(w & 0xFFFF).count('1')
                self.set(rn, _add(r[rn], 4 * n) if w & (1 << 23) else _sub(r[rn], 4 * n))
            if w & (1 << 20):
                for n in range(16):
                    if w & (1 << n):
                        self.set(n, None)
            else:
                self.st()
            return
        if op == 5:
            return
        if (w & 0x0F000000) == 0x0F000000:              # swi: the BIOS may change r0-r3
            return self.clobber(range(4))
        if (w & 0x0F100010) == 0x0E100010:              # mrc
            if rd == 15:
                self.f = None
            else:
                self.set(rd, None)

    def arm_dp(self, w, a):
        r = self.r
        opc, s = (w >> 21) & 0xF, (w >> 20) & 1
        rn, rd = (w >> 16) & 0xF, (w >> 12) & 0xF
        if not s and 8 <= opc <= 11:                    # the miscellaneous space
            if (w & 0x0FFFFFD0) == 0x012FFF10:          # bx, blx
                v = r[w & 0xF]
                if w & 0x20:
                    self.set(14, a + 4)
                self.pcval = v
            elif (w & 0x0FFF0FF0) == 0x016F0F10:        # clz
                v = r[w & 0xF]
                self.set(rd, 32 - v.bit_length() if isinstance(v, int) else None)
            elif (w & 0x0FBF0FFF) == 0x010F0000:        # mrs
                self.set(rd, None)
            elif (w & 0x0DB0F000) == 0x0120F000:        # msr
                if w & (1 << 19) and not w & (1 << 22):
                    self.f = None
            else:
                from .program import arm_writes
                self.clobber(arm_writes(w) or range(16))
            return
        pc = a + 8
        cin = self.carry()
        if w & (1 << 25):
            imm, rot = w & 0xFF, ((w >> 8) & 0xF) * 2
            op2 = ((imm >> rot) | (imm << (32 - rot))) & M if rot else imm
            sc = op2 >> 31 if rot else cin
        else:
            rm, st = w & 0xF, (w >> 5) & 3
            if w & 0x10:
                pc = a + 12
                op2, sc = _shift_reg(pc if rm == 15 else r[rm], st, r[(w >> 8) & 0xF], cin)
            else:
                op2, sc = _shift_imm(pc if rm == 15 else r[rm], st, (w >> 7) & 0x1F, cin)
        vn = pc if rn == 15 else r[rn]
        both = _ints(vn, op2)
        res = None
        if opc == 13:
            res = op2
        elif opc == 15:
            res = ~op2 & M if isinstance(op2, int) else None
        elif opc in (2, 10):
            res = _sub(vn, op2)
        elif opc == 3:
            res = _sub(op2, vn)
        elif opc in (4, 11):
            res = _add(vn, op2)
        elif both and opc in (0, 8):
            res = vn & op2
        elif both and opc in (1, 9):
            res = vn ^ op2
        elif both and opc == 12:
            res = vn | op2
        elif both and opc == 14:
            res = vn & ~op2 & M
        elif both and cin is not None and opc in (5, 6, 7):
            res = {5: vn + op2 + cin, 6: vn - op2 - (1 - cin), 7: op2 - vn - (1 - cin)}[opc] & M
        if s or 8 <= opc <= 11:
            if opc in (0, 1, 8, 9, 12, 13, 14, 15):
                self.flags_logic(res, sc)
            elif opc in (2, 10):
                self.flags_sub(vn, op2)
            elif opc == 3:
                self.flags_sub(op2, vn)
            elif opc in (4, 11):
                self.flags_add(vn, op2)
            else:
                self.flags_carry_op(res, vn, op2, opc != 5)
        if not 8 <= opc <= 11:
            self.set(rd, res)

    def thumb(self, ins):
        hw, a = ins.word, ins.addr
        r = self.r
        if ins.pair:
            return self.clobber()
        cin = self.carry()
        lo_d, lo_s = hw & 7, (hw >> 3) & 7
        if hw >> 13 == 0:
            if (hw >> 11) == 3:                         # add/sub rd, rs, rn or #imm3
                vn = r[lo_s]
                op2 = (hw >> 6) & 7 if hw & 0x400 else r[(hw >> 6) & 7]
                if hw & 0x200:
                    self.flags_sub(vn, op2)
                    return self.set(lo_d, _sub(vn, op2))
                self.flags_add(vn, op2)
                return self.set(lo_d, _add(vn, op2))
            res, c = _shift_imm(r[lo_s], hw >> 11, (hw >> 6) & 0x1F, cin)   # lsl/lsr/asr #imm
            self.flags_logic(res, c)
            return self.set(lo_d, res)
        if hw >> 13 == 1:                               # mov/cmp/add/sub rd, #imm8
            o, rd, imm = (hw >> 11) & 3, (hw >> 8) & 7, hw & 0xFF
            v = r[rd]
            if o == 0:
                self.flags_logic(imm, cin)
                return self.set(rd, imm)
            if o == 2:
                self.flags_add(v, imm)
                return self.set(rd, _add(v, imm))
            self.flags_sub(v, imm)
            return self.set(rd, _sub(v, imm)) if o == 3 else None
        if (hw >> 10) == 0x10:                          # ALU operations
            o = (hw >> 6) & 0xF
            vd, vs = r[lo_d], r[lo_s]
            both = _ints(vd, vs)
            res = None
            if o in (2, 3, 4, 7):
                res, c = _shift_reg(vd, {2: 0, 3: 1, 4: 2, 7: 3}[o], vs, cin)
                self.flags_logic(res, c)
            elif o == 10:
                return self.flags_sub(vd, vs)
            elif o == 11:
                return self.flags_add(vd, vs)
            elif o == 9:
                res = _sub(0, vs)
                self.flags_sub(0, vs)
            elif o in (5, 6):
                if both and cin is not None:
                    res = (vd + vs + cin if o == 5 else vd - vs - (1 - cin)) & M
                self.flags_carry_op(res, vd, vs, o == 6)
            elif o == 13:
                res = (vd * vs) & M if both else None
                self.flags_logic(res, None)
            else:
                if both:
                    res = {0: vd & vs, 1: vd ^ vs, 8: vd & vs, 12: vd | vs, 14: vd & ~vs & M, 15: ~vs & M}[o]
                elif o == 15 and isinstance(vs, int):
                    res = ~vs & M
                self.flags_logic(res, cin)
                if o == 8:
                    return
            return self.set(lo_d, res)
        if (hw >> 10) == 0x11:                          # hi registers: add, cmp, mov, bx
            o, rs, rd = (hw >> 8) & 3, (hw >> 3) & 0xF, lo_d | ((hw >> 4) & 8)
            vs = a + 4 if rs == 15 else r[rs]
            vd = a + 4 if rd == 15 else r[rd]
            if o == 0:
                return self.set(rd, _add(vd, vs))
            if o == 1:
                return self.flags_sub(vd, vs)
            if o == 2:
                return self.set(rd, vs)
            if hw & 0x80:
                self.set(14, (a + 2) | 1)
            self.pcval = vs
            return
        if (hw >> 11) == 9:                             # ldr rd, [pc, #imm]
            return self.ld((hw >> 8) & 7, ((a + 4) & ~3) + (hw & 0xFF) * 4, 4, pcrel=True)
        if (hw >> 12) == 5:                             # loads and stores with a register offset
            at = _add(r[lo_s], r[(hw >> 6) & 7])
            o = (hw >> 10) & 3
            if hw & 0x200:                              # strh, ldrsb, ldrh, ldrsh
                if o:
                    return self.ld(lo_d, at, 1 if o == 1 else 2, signed=o != 2)
            elif o >= 2:                                # ldr, ldrb
                return self.ld(lo_d, at, 4 if o == 2 else 1, rotate=True)
            return self.st(at if o == 0 and not hw & 0x200 else None, r[lo_d])
        if (hw >> 13) == 3:                             # ldr/str/ldrb/strb rd, [rb, #imm]
            byte = hw & 0x1000
            imm = (hw >> 6) & 0x1F
            at = _add(r[lo_s], imm if byte else imm * 4)
            if hw & 0x800:
                return self.ld(lo_d, at, 1 if byte else 4, rotate=True)
            return self.st(None if byte else at, r[lo_d])
        if (hw >> 12) == 8:                             # ldrh/strh rd, [rb, #imm]
            if hw & 0x800:
                return self.ld(lo_d, _add(r[lo_s], ((hw >> 6) & 0x1F) * 2), 2)
            return self.st()
        if (hw >> 12) == 9:                             # sp-relative loads and stores
            rd, at = (hw >> 8) & 7, _add(r[13], (hw & 0xFF) * 4)
            if hw & 0x800:
                return self.ld(rd, at, 4, rotate=True)
            return self.st(at, r[rd])
        if (hw >> 12) == 0xA:                           # add rd, pc/sp, #imm
            imm = (hw & 0xFF) * 4
            return self.set((hw >> 8) & 7, _add(r[13], imm) if hw & 0x800 else ((a + 4) & ~3) + imm)
        if (hw >> 8) == 0xB0:                           # add sp, #imm
            imm = (hw & 0x7F) * 4
            return self.set(13, _sub(r[13], imm) if hw & 0x80 else _add(r[13], imm))
        if (hw >> 12) == 0xB and (hw & 0x600) == 0x400:     # push, pop
            if hw & 0x800:
                for n in [n for n in range(8) if hw & (1 << n)] + ([15] if hw & 0x100 else []):
                    self.set(n, None)
            else:
                self.st()
            return self.set(13, None)
        if (hw >> 12) == 0xC:                           # ldmia, stmia
            rb, rl = (hw >> 8) & 7, hw & 0xFF
            if hw & 0x800:
                for n in range(8):
                    if rl & (1 << n):
                        self.set(n, None)
            else:
                self.st()
            if not (hw & 0x800 and rl & (1 << rb)):
                self.set(rb, _add(r[rb], 4 * bin(rl).count('1')))
            return
        if (hw >> 8) == 0xDF:                           # swi
            self.clobber(range(4))


def _target(run, ins, kind, prog):
    """Where the run's jump lands, as (addr, thumb), or None when that is unknown or in no
    segment."""
    v = run.pcval
    if not isinstance(v, int):
        return None
    if kind == 'dp':
        t = (v & ~1, True) if ins.thumb else (v & ~3, False)
    elif kind == 'ldr' and prog.arch < 5:
        t = (v & ~3, False)
    else:
        t = (v & ~1, True) if v & 1 else (v & ~3, False)
    return t if prog.segs_for(t[0], run.seg) else None


def evaluate(prog, seg, ins):
    """Where the computed jump `ins` goes: ('const', [(addr, thumb)]), ('table', [(addr,
    thumb), ...]) with one entry per value of the index symbol in increasing order, or None
    when the code before it doesn't say. The path is read without assumptions first, then
    with the assertion reading (a call the branch before it skips never returns), which can
    only bound a table the first reading leaves without one."""
    kind = jump_kind(ins)
    if kind is None:
        return None
    paths = [build_path(prog, seg, ins)]
    again = build_path(prog, seg, ins, assertion=True)
    if [e[0].addr for e in again] != [e[0].addr for e in paths[0]]:
        paths.append(again)
    form = table_form(ins)
    for path in paths:
        res = _evaluate_path(prog, seg, ins, kind, form, path)
        if res:
            return res
    return None


def _evaluate_path(prog, seg, ins, kind, form, path):
    """A loop's head is a join the path can't see when its back edge comes after the jump:
    the loop's first values (movs r4, #0) make the path infeasible, or a table's index a
    constant (whose table read then looks like a variable's). So a run that is infeasible,
    or finds no table where only a table explains the jump, starts the path later."""
    for k in range(len(path)):
        first = _Run(prog, seg).run(path[k:])
        if first.infeasible:
            continue
        t = _target(first, ins, kind, prog)
        if t is not None:
            if not form:
                return 'const', [t]
            continue
        res = _table(prog, seg, ins, kind, path[k:], first)
        if res or not form:
            return res
    return None


def _table(prog, seg, ins, kind, path, first):
    """The table bounded by the guards of the first run, for the symbol whose guard is
    nearest the jump: one run per value. Each must reach a known address.

    Tests for equality (a zero check before the switch) don't bound a table, and they
    hold only if the branch is the one way into the code after it, which the path can't
    know: a value they leave out costs nothing to keep."""
    tests = {i for _, s, i in first.constraints if sum(hi - lo + 1 for lo, hi in s) in (1, M)}
    guards = [c for c in first.constraints if c[2] not in tests]
    order = []
    for var, _, _ in reversed(guards):
        if var not in order:
            order.append(var)
    for var in order:
        vals = [(0, M)]
        for v2, s, _ in guards:
            if v2 == var:
                vals = _intersect(vals, s)
        if not vals or sum(hi - lo + 1 for lo, hi in vals) > MAX_CASES:
            continue
        out = []
        for value in (v for lo, hi in vals for v in range(lo, hi + 1)):
            run = _Run(prog, seg, var, value, first.fixed, tests).run(path)
            if run.infeasible:
                continue        # this value never reaches the jump
            t = _target(run, ins, kind, prog)
            if t is None:
                out = None
                break
            out.append(t)
        if out:
            return 'table', out
    return None
