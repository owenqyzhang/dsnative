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
    def __init__(self, segments, arch=5, prefix='g9'):
        """segments: list of (name, addr, data, kind); kind 'overlay' marks code whose
        address other code can share (checked at run time)."""
        self.segs = [Seg(i, *s) for i, s in enumerate(segments)]
        self.arch = arch
        self.prefix = prefix
        self.insns = {}             # (seg, addr, thumb) -> Insn
        self.entries = set()        # function entries (seg, addr, thumb)
        self.resume = set()         # labels that must exist: return sites, jump-table targets
        self.extra_targets = {}     # (seg, addr, thumb) -> [(addr, thumb)] (jump tables)
        self.rejected = set()
        self.literals = set()
        self.warnings = []
        self._visited = set()       # instructions on walks from the entry points
        self._spec_visited = set()  # instructions reached only from pointer candidates
        self._unbounded = set()     # jump tables no walk found the bound of
        self._cs = None

    # -- segments ------------------------------------------------------------
    def segs_for(self, addr, from_seg=None):
        """The segments a branch to addr can land in: the caller's own segment first, then
        the non-overlay segments, then every overlay that covers it."""
        out = []
        if from_seg is not None and from_seg.contains(addr, 2):
            out.append(from_seg)
            if from_seg.kind != 'overlay':
                return out
        for s in self.segs:
            if s.kind != 'overlay' and s.contains(addr, 2) and s not in out:
                return out + [s]
        for s in self.segs:
            if s.kind == 'overlay' and s.contains(addr, 2) and s not in out:
                out.append(s)
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
    def add_entry(self, addr, thumb, seg=None, speculative=False):
        segs = [seg] if seg is not None else self.segs_for(addr)
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
        while True:
            self._walk(work)
            if not scan_data:
                break
            work.extend(self._scan_data())
            if not work:
                break
        for si, a, t in sorted(self._unbounded - self.extra_targets.keys()):
            self.warnings.append(f'{self.segs[si].name}: jump table at {a:#x} without a bound')
        self._unbounded.clear()
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
                for k in self.add_entry(t, thumb, speculative=True):
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
                if not thumb and ins.flow in ('ret', 'icall') and (ins.word & 0x0FFFFFD0) == 0x012FFF10:
                    # bx/blx rN after ldr rN, =f: f is code, as surely as this walk is
                    v = self._const(recent, len(recent) - 1, ins.word & 0xF)
                    if v is not None:
                        for ts in self.segs_for(v & ~1, seg):
                            tk = (ts.index, v & ~1, bool(v & 1))
                            self.entries.add(tk)
                            push(tk, spec)
                if ins.target is not None:
                    for ts in self.segs_for(ins.target, seg):
                        tk = (ts.index, ins.target, ins.target_thumb)
                        if ins.flow == 'call' or ins.target_thumb != thumb:
                            if tk not in self.entries:
                                self.entries.add(tk)
                            push(tk, spec)
                        elif ts is not seg:
                            self.entries.add(tk)    # a branch into another segment: a function of its own
                            push(tk, spec)
                        else:
                            push(tk, spec)
                    if ins.flow == 'call':
                        self.resume.add((si, ins.next, thumb))
                for kind, rm in [e for e in ins.extra if isinstance(e, tuple) and isinstance(e[0], str)]:
                    jk = (si, ins.addr, thumb)
                    targets = self._jump_table(seg, ins, kind, rm, recent)
                    if targets is None:
                        self._unbounded.add(jk)
                        continue
                    self.extra_targets[jk] = targets
                    for ta, tt in targets:
                        if seg.contains(ta, 2):
                            tk = (si, ta, tt)
                            self.resume.add(tk)
                            push(tk, spec)
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
            for k in self.add_entry(a, thumb, speculative=True):
                push(k, True)
            break

    def _jump_table(self, seg, ins, kind, rm, recent):
        """The entries of a jump table (None when this walk didn't come through its bound):
        - add pc, pc, rm, lsl #2 after cmp rm, #n: a table of n + 1 branches follows;
        - the same add pc after ldrb rm, [rt, ri] (or add ri, ri, ri; ldrh rm, [rt, ri]),
          with cmp ri, #n before it and rt loaded from a literal: a table of n + 1 byte
          (or halfword) offsets at rt, each case at pc + 4 * offset (GCC 15);
        - ldr pc, [pc, rm, lsl #2] after cmp rm, #n: a table of n + 1 addresses follows."""
        i = len(recent) - 1        # recent[i] is the jump
        out = []
        if kind == 'addpc':
            load, size, ri = recent[i - 1] if i >= 1 else None, 0, None
            if load is not None and not load.thumb and ((load.word >> 12) & 0xF) == rm:
                w = load.word
                if (w & 0x0FF00FF0) == 0x07D00000:             # ldrb rm, [rt, ri]
                    size, ri, at = 1, w & 0xF, i - 1
                elif (w & 0x0FF00FF0) == 0x019000B0 and i >= 2:  # ldrh rm, [rt, ri]
                    dbl = recent[i - 2].word if not recent[i - 2].thumb else 0
                    r = w & 0xF
                    if dbl in (0xE0800000 | (r << 16) | (r << 12) | r, 0xE1A00080 | (r << 12) | r):
                        size, ri, at = 2, r, i - 2               # after add ri, ri, ri or lsl ri, ri, #1
            if size and ri == (load.word >> 16) & 0xF:
                return None     # base and index in one register: not a table
            if size:
                n = self._bound(recent, at, ri)
                base = self._const(recent, at, (load.word >> 16) & 0xF)
                if n is None or base is None:
                    return None
                ts = next((s for s in self.segs_for(base, seg) if s.contains(base, size * (n + 1))), None)
                if ts is None:
                    return None
                for k in range(n + 1):
                    off = ts.data[base - ts.addr + k] if size == 1 else ts.u16(base + 2 * k)
                    out.append((ins.addr + 8 + 4 * off, False))
                return out
        n = self._bound(recent, i, rm)
        if n is None:
            return None
        if kind == 'addpc':
            out = [(ins.addr + 8 + 4 * k, False) for k in range(n + 1)]
        else:
            base = ins.addr + 8
            for k in range(n + 1):
                if not seg.contains(base + 4 * k, 4):
                    break
                v = seg.u32(base + 4 * k)
                self.literals.add(base + 4 * k)
                out.append((v & ~1, bool(v & 1) and self.arch >= 5))
        return out

    @staticmethod
    def _bound(recent, i, r):
        """n from the cmp r, #n shortly before recent[i], if nothing in between writes r."""
        for prev in reversed(recent[max(0, i - 6):i]):
            if prev.thumb:
                return None
            w = prev.word
            if (w & 0x0FF0F000) == 0x03500000 and ((w >> 16) & 0xF) == r:
                imm = w & 0xFF
                rot = ((w >> 8) & 0xF) * 2
                n = ((imm >> rot) | (imm << (32 - rot))) & 0xFFFFFFFF if rot else imm
                return n if n <= 1024 else None
            wr = arm_writes(w)
            if wr is None or r in wr:
                return None
        return None

    def _const(self, recent, i, r):
        """The value r holds at recent[i], when the last write to it was a literal load
        (ldr r, [pc, #imm]) or an adr (add/sub r, pc, #imm)."""
        for prev in reversed(recent[max(0, i - 32):i]):
            if prev.thumb:
                return None
            w = prev.word
            wr = arm_writes(w)
            if wr is None:
                return None
            if r not in wr:
                continue
            if (w >> 28) != 0xE or ((w >> 12) & 0xF) != r or wr != {r}:
                return None
            if (w & 0x0F7F0000) == 0x051F0000:              # ldr r, [pc, #+-imm]
                lit = (prev.addr + 8 + ((w & 0xFFF) if w & (1 << 23) else -(w & 0xFFF))) & 0xFFFFFFFF
                s = next((s for s in self.segs_for(lit) if s.contains(lit, 4)), None)
                return s.u32(lit) if s is not None else None
            if (w & 0x0FEF0000) in (0x028F0000, 0x024F0000):  # add/sub r, pc, #imm
                imm = w & 0xFF
                rot = ((w >> 8) & 0xF) * 2
                imm = ((imm >> rot) | (imm << (32 - rot))) & 0xFFFFFFFF if rot else imm
                return (prev.addr + 8 + (imm if (w & 0x01E00000) == 0x00800000 else -imm)) & 0xFFFFFFFF
            return None
        return None

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
            if seg.kind == 'overlay':
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
                'rejected_candidates': len(self.rejected)}
