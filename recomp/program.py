"""Find a CPU's code and write it out as C.

Discovery walks the code from the entry points it is given (the entry address, the BIOS
interrupt handler the game installs is found at run time, so every literal-pool word that
points into code is a candidate entry too), following branches, calls and jump tables.
Grouping then builds C functions: each function entry (a call target or a candidate)
becomes one C function holding every block reachable from it by branches. Return sites,
jump-table targets and the instruction after a waiting BIOS call are labels inside it;
every label is registered in the dispatch table, so execution can resume at any of them.
"""
import os
import struct
from collections import deque

try:
    import capstone
except ImportError:     # disassembly comments are optional
    capstone = None

from .isa import decode_arm, decode_thumb, Insn, h
from .jumps import evaluate, jump_kind, table_form


def arm_writes(w):
    """The registers an ARM instruction can write, or None when it may write more than
    discovery cares to model (calls, SWI, coprocessor, the miscellaneous space)."""
    cond = w >> 28
    if cond == 0xF:
        return None
    rn, rd = (w >> 16) & 0xF, (w >> 12) & 0xF
    op = (w >> 25) & 7
    if op == 0 and (w & 0x90) == 0x90:
        if (w & 0x0FC000F0) == 0x00000090:          # mul, mla
            return {rn}
        if (w & 0x0F8000F0) == 0x00800090:          # umull, umlal, smull, smlal
            return {rn, rd}
        if (w & 0x0FB00FF0) == 0x01000090:          # swp, swpb
            return {rd}
        if (w & 0x60) == 0:
            return None
        out = set()
        if w & (1 << 20):                           # ldrh, ldrsb, ldrsh
            out.add(rd)
        elif (w & 0x60) == 0x40:                    # ldrd
            out |= {rd, rd + 1}
        if not (w & (1 << 24)) or (w & (1 << 21)):
            out.add(rn)
        return out
    if op in (0, 1):
        opc = (w >> 21) & 0xF
        if 8 <= opc <= 11:
            # tst/teq/cmp/cmn with S; without S this is msr, mrs, bx, clz, the DSP ops
            return set() if (w & (1 << 20)) else None
        return {rd}
    if op in (2, 3):
        if op == 3 and (w & 0x10):
            return None
        out = {rd} if w & (1 << 20) else set()
        if not (w & (1 << 24)) or (w & (1 << 21)):
            out.add(rn)
        return out
    if op == 4:
        out = {r for r in range(16) if w & (1 << r)} if w & (1 << 20) else set()
        if w & (1 << 21):
            out.add(rn)
        return out
    if op == 5:
        return set() if not (w & (1 << 24)) else None   # b; bl clobbers the caller-saved registers
    return None


class Seg:
    def __init__(self, index, name, addr, data, kind='static'):
        self.index = index
        self.name = name
        self.addr = addr
        self.data = bytes(data)
        self.kind = kind
        self.overlaps = []      # other segments sharing some of its addresses

    @property
    def end(self):
        return self.addr + len(self.data)

    def contains(self, a, n=1):
        return self.addr <= a and a + n <= self.end

    def u32(self, a):
        return struct.unpack_from('<I', self.data, a - self.addr)[0]

    def u16(self, a):
        return struct.unpack_from('<H', self.data, a - self.addr)[0]


class Program:
    def __init__(self, segments, arch=5, prefix='g9', pic=()):
        """segments: list of (name, addr, data, kind); kind 'overlay' marks code whose
        address other code can share, 'stub' code crt0 copies somewhere and runs. Code at
        an address several segments share is checked against memory at run time.
        pic: (name, data) of code with no address of its own (a stub run from the stack),
        compiled as functions the dispatcher finds by their bytes (see emit_pic)."""
        self.segs = [Seg(i, *s) for i, s in enumerate(segments)]
        for a in self.segs:
            a.overlaps = [b for b in self.segs if b is not a and a.addr < b.end and b.addr < a.end]
        self.arch = arch
        self.prefix = prefix
        self.pic = list(pic)
        self.insns = {}             # (seg, addr, thumb) -> Insn
        self.entries = set()        # function entries (seg, addr, thumb)
        self.resume = set()         # labels that must exist: return sites, jump-table targets, saved contexts
        self.extra_targets = {}     # (seg, addr, thumb) -> [(addr, thumb)] (jump tables)
        self._jumps = {}            # (seg, addr, thumb) -> what jumps.evaluate found there
        self.rejected = set()
        self.literals = set()
        self.warnings = []
        self._visited = set()       # instructions on walks from the entry points
        self._spec_visited = set()  # instructions reached only from pointer candidates
        self._cs = None

    # -- segments ------------------------------------------------------------
    def segs_for(self, addr, from_seg=None):
        """The segments a branch to addr can land in: the caller's own segment when it
        covers addr (an overlay is loaded while its own code runs, and any overlay sharing
        the address would have overwritten it), else the one segment there, else every
        segment that covers it (overlays, or a stub and the autoload copied over it)."""
        if from_seg is not None and from_seg.contains(addr, 2):
            return [from_seg]
        out = [s for s in self.segs if s.contains(addr, 2)]
        for s in out:
            if s.kind != 'overlay' and not s.overlaps:
                return [s]
        return out

    def decode(self, seg, addr, thumb):
        key = (seg.index, addr, thumb)
        ins = self.insns.get(key)
        if ins is not None:
            return ins
        if thumb:
            if not seg.contains(addr, 2):
                return None
            hw = seg.u16(addr)
            nxt = seg.u16(addr + 2) if seg.contains(addr + 2, 2) else None
            ins = decode_thumb(addr, hw, nxt, self.arch)
        else:
            if not seg.contains(addr, 4) or addr & 3:
                return None
            ins = decode_arm(addr, seg.u32(addr), self.arch)
        self.insns[key] = ins
        return ins

    # -- discovery -----------------------------------------------------------
    def add_entry(self, addr, thumb, seg=None, speculative=False, from_seg=None):
        segs = [seg] if seg is not None else self.segs_for(addr, from_seg)
        added = []
        for s in segs:
            k = (s.index, addr, thumb)
            if k in self.entries or k in self.rejected:
                continue
            if speculative and not self._plausible(s, addr, thumb):
                self.rejected.add(k)
                continue
            self.entries.add(k)
            added.append(k)
        return added

    def _plausible(self, seg, addr, thumb):
        """A literal-pool word that points into code is only taken as a function when the
        code there looks like the start of one."""
        if not thumb and addr & 3:
            return False
        a = addr
        for i in range(6):
            ins = self.decode(seg, a, thumb)
            if ins is None or ins.flow == 'stop':
                return False
            if i == 0 and not thumb and (ins.word >> 28) != 0xE:
                return False
            if ins.flow in ('ret', 'jump') and ins.cond is None:
                return True
            a = ins.next
        return True

    def discover(self, entries, scan_data=True):
        """entries: list of (addr, thumb). Walks all code reachable from them, then (with
        scan_data) takes every data word that points at plausible code as an entry too:
        tables of function pointers and vtables live outside literal pools."""
        work = deque()
        for addr, thumb in entries:
            work.extend((k, False) for k in self.add_entry(addr, thumb))
        for s in self.segs:
            if s.kind == 'stub':
                work.extend((k, False) for k in self.add_entry(s.addr, False, seg=s))
        while True:
            self._walk(work)
            if not scan_data:
                break
            work.extend(self._scan_data())
            if not work:
                break
        for si, a, t in sorted(k for k, v in self._jumps.items() if v == 'unbounded'):
            self.warnings.append(f'{self.segs[si].name}: jump table at {a:#x} without a bound')
        return self

    def _scan_data(self):
        """Pointer candidates in data. Only code walked from the entry points hides a word
        from the scan: a candidate that is really data (a string at an odd address reads as
        Thumb code) can walk on into a table of function pointers, and must not hide it."""
        covered = set()
        for k in self._visited:
            ins = self.insns.get(k)
            if ins is not None:
                for o in range(0, ins.size, 2):
                    covered.add((k[0], k[1] + o))
        found = []
        for seg in self.segs:
            for off in range(0, len(seg.data) - 3, 4):
                a = seg.addr + off
                if (seg.index, a) in covered:
                    continue
                v = struct.unpack_from('<I', seg.data, off)[0]
                if not (0x01000000 <= v < 0x04000000):
                    continue
                thumb = bool(v & 1)
                t = v & ~1
                if not thumb and t & 3:
                    continue
                for k in self.add_entry(t, thumb, speculative=True, from_seg=seg):
                    found.append((k, True))
        return found

    def _walk(self, work):
        """work: (key, speculative). A walk from a pointer candidate is speculative, and so
        is all it reaches. Walks from the entry points go first, so they see code with its
        own context (the cmp bounding a jump table, the mov lr, pc of a call) before any
        candidate reaches into it; they stop only at code they covered themselves."""
        queues = (deque(), deque())     # from the entry points, from candidates
        push = lambda k, sp: queues[sp].append(k)     # noqa: E731
        for k, sp in work:
            push(k, sp)
        work.clear()
        seen = set()
        while queues[0] or queues[1]:
            spec = not queues[0]
            k = queues[spec].popleft()
            if (k, spec) in seen:
                continue
            seen.add((k, spec))
            si, addr, thumb = k
            seg = self.segs[si]
            mine = self._spec_visited if spec else self._visited
            recent = []
            while True:
                if (si, addr, thumb) in self._visited or (si, addr, thumb) in mine:
                    break
                mine.add((si, addr, thumb))
                ins = self.decode(seg, addr, thumb)
                if ins is None:
                    self.warnings.append(f'{seg.name}: code runs out of its segment at {addr:#x}')
                    break
                if ins.flow == 'ret' and not thumb and recent and recent[-1].word == 0xE1A0E00F and recent[-1].cond is None \
                        and recent[-1].next == ins.addr:
                    ins.flow = 'icall'      # mov lr, pc; bx rN / ldr pc, [...]: an ARMv4 indirect call
                recent.append(ins)
                for lit in ins.literals:
                    self.literals.add(lit)
                    self._literal_candidate(seg, lit, push)
                if ins.flow in ('ret', 'icall') and jump_kind(ins):
                    self._computed_jump(seg, ins, spec, push)
                if not thumb and (ins.word & 0xFFFF0000) == 0xE28F0000:
                    # adr rN, x: an address saved for an exception-style return (subs pc, lr, #4),
                    # as OS_SaveContext saves its own resume point + 4
                    imm, rot = ins.word & 0xFF, ((ins.word >> 8) & 0xF) * 2
                    v = ins.addr + 8 + (((imm >> rot) | (imm << (32 - rot))) & 0xFFFFFFFF if rot else imm)
                    if seg.contains(v - 4, 4):
                        self.resume.add((si, v - 4, False))
                if ins.target is not None:
                    tsegs = self.segs_for(ins.target, seg)
                    for ts in tsegs:
                        tk = (ts.index, ins.target, ins.target_thumb)
                        # with several segments there, at most one is the code that runs
                        sp = spec or len(tsegs) > 1
                        if ins.flow == 'call' or ins.target_thumb != thumb:
                            if tk not in self.entries:
                                self.entries.add(tk)
                            push(tk, sp)
                        elif ts is not seg:
                            self.entries.add(tk)    # a branch into another segment: a function of its own
                            push(tk, sp)
                        else:
                            push(tk, sp)
                    if ins.flow == 'call':
                        self.resume.add((si, ins.next, thumb))
                if ins.flow in ('split', 'icall'):
                    self.resume.add((si, ins.next, thumb))
                ends = ins.flow == 'stop' or (ins.cond is None and ins.flow in ('jump', 'ret'))
                if ends:
                    break
                addr = ins.next

    def _literal_candidate(self, seg, lit, push):
        for s in self.segs_for(lit, seg):
            if not s.contains(lit, 4):
                continue
            v = s.u32(lit)
            thumb = bool(v & 1)
            a = v & ~1
            if not thumb and a & 3:
                continue
            for k in self.add_entry(a, thumb, speculative=True, from_seg=s):
                push(k, True)
            break

    def _computed_jump(self, seg, ins, spec, push):
        """A jump to a computed address, as jumps.evaluate reads the code before it. A
        constant (ldr r3, =main; blx r3) is a function entry, as surely as this walk is
        code. A table's cases are labels of this function; in another state or segment, or
        called, they are entries. An add pc, pc, rI, lsl #2 nothing bounds is a run of
        branches or an unrolled loop (_unbounded_addpc); other tables are reported."""
        jk = (seg.index, ins.addr, ins.thumb)
        if jk not in self._jumps:
            res = evaluate(self, seg, ins)
            form = table_form(ins)
            if res is None and form == 'addpc':
                out = self._unbounded_addpc(seg, ins)
                res = ('table', out) if out else 'unbounded'
            elif res is None and form:
                res = 'unbounded'
            self._jumps[jk] = res
        res = self._jumps[jk]
        if not isinstance(res, tuple):
            return
        kind, targets = res
        if kind == 'table':
            self.extra_targets[jk] = targets
        for ta, tt in targets:
            tsegs = self.segs_for(ta, seg)
            for ts in tsegs:
                tk = (ts.index, ta, tt)
                if kind == 'table' and ts is seg and tt == ins.thumb and ins.flow != 'icall':
                    self.resume.add(tk)
                else:
                    self.entries.add(tk)
                push(tk, spec or len(tsegs) > 1)

    def _unbounded_addpc(self, seg, ins):
        """add pc, pc, rm, lsl #2 with no cmp to bound it: every place it can land in what
        follows. A run of branches is a table of them (CodeWarrior bounds some switches
        before offsetting the index); straight-line code is an unrolled loop entered part
        way (the SDK's division, a Duff's device copy), so each instruction to the end of
        the run."""
        out = []
        a = ins.addr + 8
        first = self.decode(seg, a, False)
        if first is None:
            return None
        table = first.flow == 'jump' and first.cond is None
        while len(out) < 1024:
            nxt = self.decode(seg, a, False)
            if nxt is None or nxt.flow == 'stop':
                break
            ends = nxt.cond is None and nxt.flow in ('jump', 'ret')
            if table and not (nxt.flow == 'jump' and nxt.cond is None):
                break
            out.append((a, False))
            if ends and not table:
                break
            a += 4
        return out or None

    # -- C output ------------------------------------------------------------
    def _disasm(self, ins):
        if capstone is None:
            return ''
        if self._cs is None:
            self._cs = (capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM),
                        capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB))
        md = self._cs[1 if ins.thumb else 0]
        b = ins.word.to_bytes(4 if not ins.thumb else 2, 'little')
        if ins.pair:
            b += (self.insns_word_after(ins)).to_bytes(2, 'little')
        for i in md.disasm(b, ins.addr):
            return f'{i.mnemonic} {i.op_str}'.strip()
        return '?'

    def insns_word_after(self, ins):
        for s in self.segs:
            if s.contains(ins.addr + 2, 2):
                return s.u16(ins.addr + 2)
        return 0

    def fname(self, k):
        si, addr, thumb = k
        return f'{self.prefix}_s{si}_{addr:08x}_{"t" if thumb else "a"}'

    def build_functions(self):
        """Group the decoded code into functions: entry -> ordered list of instruction keys."""
        funcs = {}
        for e in sorted(self.entries):
            si, start, thumb = e
            if e not in self.insns and self.decode(self.segs[si], start, thumb) is None:
                continue
            body = set()
            labels = {start}
            work = deque([start])
            while work:
                a = work.popleft()
                while True:
                    if a in body:
                        break
                    if a != start and (si, a, thumb) in self.entries:
                        break       # falls into another function: leaves there
                    ins = self.decode(self.segs[si], a, thumb)
                    if ins is None:
                        break
                    body.add(a)
                    if (si, a, thumb) in self.resume:
                        labels.add(a)
                    if ins.target is not None and ins.flow == 'jump' and ins.target_thumb == thumb \
                            and self.segs[si].contains(ins.target, 2) and (si, ins.target, thumb) not in self.entries:
                        labels.add(ins.target)
                        work.append(ins.target)
                    for ta, tt in self.extra_targets.get((si, a, thumb), []):
                        if tt == thumb and self.segs[si].contains(ta, 2) and (si, ta, thumb) not in self.entries:
                            labels.add(ta)
                            work.append(ta)
                    if ins.flow in ('call', 'split', 'icall'):
                        labels.add(ins.next)
                    if ins.flow == 'stop' or (ins.cond is None and ins.flow in ('jump', 'ret')):
                        break
                    a = ins.next
            funcs[e] = (sorted(body), labels)
        return funcs

    def emit(self, outdir, per_file=400):
        os.makedirs(outdir, exist_ok=True)
        funcs = self.build_functions()
        names = []
        files = []
        table = []          # (key, check bytes or None, fname)
        chunk = []
        for e, (body, labels) in funcs.items():
            chunk.append((e, body, labels))
            if len(chunk) >= per_file:
                files.append(chunk)
                chunk = []
        if chunk:
            files.append(chunk)
        for fi, chunk in enumerate(files):
            out = [f'/* generated by dsnative recomp: {self.prefix} part {fi} */', '#include "dsr.h"', '']
            for e, body, labels in chunk:
                fn = self.fname(e)
                names.append(fn)
                out += self._emit_function(e, body, labels, fn, table)
            path = os.path.join(outdir, f'{self.prefix}_code{fi:03d}.c')
            with open(path, 'w') as f:
                f.write('\n'.join(out) + '\n')
        self._emit_table(outdir, names, table)
        self._emit_pic(outdir)
        return funcs

    def _emit_function(self, e, body, labels, fn, table):
        si, start, thumb = e
        seg = self.segs[si]
        bset = set(body)
        labels = sorted(l for l in labels if l in bset)
        lines = [f'u32 {fn}(Cpu *c, u32 pc) {{', '    switch (pc) {']
        for l in labels:
            lines.append(f'    case {h(l)}: goto L_{l:08x};')
            chk = None
            if seg.kind == 'overlay' or any(o.contains(l) for o in seg.overlaps):
                n = min(16, seg.end - l)
                chk = seg.data[l - seg.addr:l - seg.addr + n]
            table.append(((l | (1 if thumb else 0)), chk, fn))
        lines += ['    default: return dsr_bad_entry(c, pc, ' + h(start) + ');', '    }']
        # instruction counts per block
        order = body
        lab = set(labels)
        for idx, a in enumerate(order):
            ins = self.insns[(si, a, thumb)]
            if a in lab:
                n = 0
                for b in order[idx:]:
                    if b != a and b in lab:
                        break
                    n += 1
                    i2 = self.insns[(si, b, thumb)]
                    if i2.flow == 'stop' or (i2.cond is None and i2.flow in ('jump', 'ret')):
                        break
                lines.append(f'L_{a:08x}:')
                lines.append(f'    c->budget -= {n};')
            lines.append(f'    /* {a:08x}: {self._disasm(ins)} */')
            lines += ['    ' + l for l in self._emit_insn(si, ins, thumb, lab)]
            falls = not (ins.flow == 'stop' or (ins.cond is None and ins.flow in ('jump', 'ret')))
            if falls:
                nxt = ins.next
                following = order[idx + 1] if idx + 1 < len(order) else None
                if ins.flow == 'call':
                    pass    # the call itself leaves; the return site is a label
                if nxt != following or (si, nxt, thumb) in self.entries and nxt != start:
                    if nxt in lab:
                        lines.append(f'    goto L_{nxt:08x};')
                    else:
                        lines.append(f'    return {h(nxt)};')
        lines.append('}')
        lines.append('')
        return lines

    def _emit_insn(self, si, ins, thumb, lab):
        code = list(ins.code)
        if ins.flow == 'jump':
            t = ins.target
            if ins.target_thumb == thumb and t in lab:
                code = code + [f'if (c->budget <= 0) return {h(t)};', f'goto L_{t:08x};']
            else:
                if ins.target_thumb != thumb:
                    code = code + [f'c->t = {1 if ins.target_thumb else 0};']
                code = code + [f'return {h(t)};']
        elif ins.flow == 'call':
            code = code + [f'return {h(ins.target)};']
        if ins.cond:
            return [f'if ({ins.cond}) {{'] + ['    ' + l for l in code] + ['}']
        if len(code) > 1:
            return ['{'] + ['    ' + l for l in code] + ['}']
        return code

    def _compile_pic(self, name, data):
        """The C lines of a stub that runs wherever its bytes are, or None (and a warning)
        when it can't: its instructions must give the same code at two different addresses
        (nothing reads the pc, nothing branches relative to it) and the last is the only
        jump, to a register."""
        why = None
        if not data or len(data) % 4:
            why = 'not whole ARM instructions'
        words = struct.unpack(f'<{len(data) // 4}I', data) if why is None else ()
        lines = []
        for k, w in enumerate(words):
            a = (0x02000000 + 4 * k, 0x02444440 + 4 * k)
            ins, other = (decode_arm(x, w, self.arch) for x in a)
            last = k == len(words) - 1
            if (ins.code, ins.cond, ins.flow) != (other.code, other.cond, other.flow):
                why = f'{w:08x} depends on its address'
            elif ins.flow != ('ret' if last else 'seq') or last and ins.cond:
                why = f'{w:08x} is a {ins.flow} where {"the jump" if last else "straight-line code"} belongs'
            if why:
                break
            lines.append(f'    /* {self._disasm(ins)} */')
            lines += ['    ' + l for l in self._emit_insn(-1, ins, False, set())]
        if why:
            self.warnings.append(f'{name}: not position-independent: {why}')
            return None
        return [f'    c->budget -= {len(words)};'] + lines

    def _emit_pic(self, outdir):
        """The position-independent stubs: one function each, and a table of their bytes
        for the dispatcher, which runs the one whose bytes it finds in memory at an address
        nothing was compiled for. Longer stubs first, for one that is a prefix of another."""
        out = [f'/* generated by dsnative recomp: {self.prefix} position-independent stubs */', '#include "dsr.h"', '']
        stubs = []
        for i, (name, data) in enumerate(self.pic):
            body = self._compile_pic(name, data)
            if body is not None:
                fn = f'{self.prefix}_pic{i}'
                out += [f'/* {name} */', f'static const u8 {fn}_bytes[] = {{{", ".join(str(b) for b in data)}}};',
                        f'static u32 {fn}(Cpu *c, u32 pc) {{'] + body + ['}', '']
                stubs.append((len(data), fn))
        out.append(f'const PicEntry {self.prefix}_pic[] = {{')
        for n, fn in sorted(stubs, key=lambda s: -s[0]):
            out.append(f'    {{{n}, {fn}_bytes, {fn}}},')
        out += ['    {0, 0, 0}', '};', f'const u32 {self.prefix}_pic_len = {len(stubs)};']
        with open(os.path.join(outdir, f'{self.prefix}_pic.c'), 'w') as f:
            f.write('\n'.join(out) + '\n')

    def _emit_table(self, outdir, names, table):
        seen = set()
        uniq = []
        for t in table:
            k = (t[0], t[1])
            if k not in seen:
                seen.add(k)
                uniq.append(t)
        table = sorted(uniq, key=lambda t: (t[0], t[2]))
        out = [f'/* generated by dsnative recomp: {self.prefix} dispatch table */', '#include "dsr.h"', '']
        for n in names:
            out.append(f'u32 {n}(Cpu *c, u32 pc);')
        out.append('')
        for i, (key, chk, fn) in enumerate(table):
            if chk:
                out.append(f'static const u8 chk{i}[] = {{{", ".join(str(b) for b in chk)}}};')
        out.append(f'const DispatchEntry {self.prefix}_table[] = {{')
        for i, (key, chk, fn) in enumerate(table):
            if chk:
                out.append(f'    {{{h(key)}, {len(chk)}, {fn}, chk{i}}},')
            else:
                out.append(f'    {{{h(key)}, 0, {fn}, 0}},')
        out.append('    {0, 0, 0, 0}')
        out.append('};')
        out.append(f'const u32 {self.prefix}_table_len = {len(table)};')
        with open(os.path.join(outdir, f'{self.prefix}_table.c'), 'w') as f:
            f.write('\n'.join(out) + '\n')

    def stats(self):
        """Instruction counts are of walked code: rejected candidates' decodes don't count."""
        walked = {k for k in self._visited | self._spec_visited if self.insns.get(k) is not None}
        return {'functions': len(self.entries), 'instructions': len(walked), 'thumb': sum(1 for k in walked if k[2]),
                'from_candidates': len(walked - self._visited), 'labels': len(self.resume),
                'rejected_candidates': len(self.rejected), 'tables': len(self.extra_targets),
                'cases': sum(len(v) for v in self.extra_targets.values()),
                'unbounded': sum(1 for v in self._jumps.values() if v == 'unbounded')}
