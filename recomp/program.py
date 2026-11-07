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
        self._visited = set()
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
            work.extend(self.add_entry(addr, thumb))
        while True:
            self._walk(work)
            if not scan_data:
                break
            work.extend(self._scan_data())
            if not work:
                break
        return self

    def _scan_data(self):
        covered = set()
        for (si, a, t), ins in self.insns.items():
            for k in range(0, ins.size, 2):
                covered.add((si, a + k))
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
                    found.append(k)
        return found

    def _walk(self, work):
        seen = set()
        visited = self._visited
        while work:
            k = work.popleft()
            if k in seen:
                continue
            seen.add(k)
            si, addr, thumb = k
            seg = self.segs[si]
            recent = []
            while True:
                if (si, addr, thumb) in visited:
                    break
                visited.add((si, addr, thumb))
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
                    self._literal_candidate(seg, lit, work)
                if ins.target is not None:
                    for ts in self.segs_for(ins.target, seg):
                        tk = (ts.index, ins.target, ins.target_thumb)
                        if ins.flow == 'call' or ins.target_thumb != thumb:
                            if tk not in self.entries:
                                self.entries.add(tk)
                            work.append(tk)
                        elif ts is not seg:
                            self.entries.add(tk)    # a branch into another segment: a function of its own
                            work.append(tk)
                        else:
                            work.append(tk)
                    if ins.flow == 'call':
                        self.resume.add((si, ins.next, thumb))
                for kind, rm in [e for e in ins.extra if isinstance(e, tuple) and isinstance(e[0], str)]:
                    targets = self._jump_table(seg, ins, kind, rm, recent)
                    self.extra_targets[(si, ins.addr, thumb)] = targets
                    for ta, tt in targets:
                        if seg.contains(ta, 2):
                            tk = (si, ta, tt)
                            self.resume.add(tk)
                            work.append(tk)
                if ins.flow in ('split', 'icall'):
                    self.resume.add((si, ins.next, thumb))
                ends = ins.flow == 'stop' or (ins.cond is None and ins.flow in ('jump', 'ret'))
                if ends:
                    break
                addr = ins.next

    def _literal_candidate(self, seg, lit, work):
        for s in self.segs_for(lit, seg):
            if not s.contains(lit, 4):
                continue
            v = s.u32(lit)
            thumb = bool(v & 1)
            a = v & ~1
            if not thumb and a & 3:
                continue
            for k in self.add_entry(a, thumb, speculative=True):
                work.append(k)
            break

    def _jump_table(self, seg, ins, kind, rm, recent):
        """The entries of a jump table, bounded by the cmp rm, #n just before it."""
        n = None
        for prev in reversed(recent[-6:-1]):
            w = prev.word
            if not prev.thumb and (w & 0x0FF0F000) == 0x03500000 and ((w >> 16) & 0xF) == rm:
                imm = w & 0xFF
                rot = ((w >> 8) & 0xF) * 2
                n = ((imm >> rot) | (imm << (32 - rot))) & 0xFFFFFFFF if rot else imm
                break
        if n is None or n > 1024:
            self.warnings.append(f'{seg.name}: jump table at {ins.addr:#x} without a bound')
            return []
        out = []
        if kind == 'addpc':
            out = [(ins.addr + 8 + 4 * i, False) for i in range(n + 1)]
        else:
            base = ins.addr + 8
            for i in range(n + 1):
                if not seg.contains(base + 4 * i, 4):
                    break
                v = seg.u32(base + 4 * i)
                self.literals.add(base + 4 * i)
                out.append((v & ~1, bool(v & 1) and self.arch >= 5))
        return out

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
        funcs = len(self.entries)
        n = len(self.insns)
        return {'functions': funcs, 'instructions': n, 'labels': len(self.resume), 'rejected_candidates': len(self.rejected)}
