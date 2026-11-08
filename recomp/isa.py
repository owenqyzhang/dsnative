"""Decode one ARM (ARMv4T/ARMv5TE) or Thumb instruction and emit the C that performs it.

decode_arm / decode_thumb return an Insn: the C statements, how control flow continues
(the flow kind and its static targets) and the literal-pool addresses it reads. The C
uses the helpers of runtime/dsr.h. Semantics follow the ARM Architecture Reference Manual
and GBATEK where the DS CPUs are specific (ARM946E-S is ARMv5TE, ARM7TDMI is ARMv4T).

Flow kinds:
  seq     falls through to the next instruction
  split   falls through, but the next instruction must start a new block (the runtime may
          have to stop here: a BIOS call that waits, a CPSR write that unmasks interrupts)
  jump    static branch to `target`
  call    BL/BLX: LR set in `code`, then leaves the function to `target`
  ret     leaves the function to a computed address (`code` holds the return statement)
  icall   an indirect call (BLX reg, or mov lr, pc before a jump): leaves like ret, and the
          next instruction is its return site
  stop    undefined: `code` raises it
A conditional instruction (cond set) falls through when its condition fails.
"""
from dataclasses import dataclass, field

COND = ['c->fz', '!c->fz', 'c->fc', '!c->fc', 'c->fn', '!c->fn', 'c->fv', '!c->fv',
        '(c->fc && !c->fz)', '(!c->fc || c->fz)', '(c->fn == c->fv)', '(c->fn != c->fv)',
        '(!c->fz && c->fn == c->fv)', '(c->fz || c->fn != c->fv)', None, None]
COND_NAMES = ['eq', 'ne', 'cs', 'cc', 'mi', 'pl', 'vs', 'vc', 'hi', 'ls', 'ge', 'lt', 'gt', 'le', '', 'nv']


@dataclass
class Insn:
    addr: int
    size: int
    thumb: bool
    code: list = field(default_factory=list)
    flow: str = 'seq'
    cond: str = None            # C condition expression, None for always
    target: int = None
    target_thumb: bool = None
    literals: list = field(default_factory=list)    # addresses of literal pool words read
    word: int = 0
    pair: bool = False          # Thumb BL/BLX emitted as one 4-byte instruction

    @property
    def next(self):
        return self.addr + self.size


def sx(v, bits):
    m = 1 << (bits - 1)
    return (v & (m - 1)) - (v & m)


def h(v):
    return f'0x{v & 0xFFFFFFFF:x}u'


def reg(n, pc):
    """C expression reading register n, with the PC as the constant it reads as."""
    return h(pc) if n == 15 else f'R[{n}]'


def popcount(v):
    return bin(v).count('1')


# ---------------------------------------------------------------------------
# shifter operands
# ---------------------------------------------------------------------------

def imm_shift(kind, amount, v):
    """Shift by an immediate: returns (value expr, carry expr or None for unchanged)."""
    if kind == 0:  # LSL
        if amount == 0:
            return v, None
        return f'({v} << {amount})', f'(({v} >> {32 - amount}) & 1)'
    if kind == 1:  # LSR
        if amount == 0:
            return '0u', f'({v} >> 31)'
        return f'({v} >> {amount})', f'(({v} >> {amount - 1}) & 1)'
    if kind == 2:  # ASR
        if amount == 0:
            return f'(u32)((s32){v} >> 31)', f'({v} >> 31)'
        return f'(u32)((s32){v} >> {amount})', f'(({v} >> {amount - 1}) & 1)'
    # ROR / RRX
    if amount == 0:
        return f'((c->fc << 31) | ({v} >> 1))', f'({v} & 1)'
    return f'ror32({v}, {amount})', f'(({v} >> {amount - 1}) & 1)'


SHIFT_FN = ['LSL_R', 'LSR_R', 'ASR_R', 'ROR_R']


# ---------------------------------------------------------------------------
# ARM
# ---------------------------------------------------------------------------

DP_NAMES = ['and', 'eor', 'sub', 'rsb', 'add', 'adc', 'sbc', 'rsc', 'tst', 'teq', 'cmp', 'cmn', 'orr', 'mov', 'bic', 'mvn']


def arm_operand2(w, addr, code):
    """Emit the shifter operand of a data-processing instruction into `code` as the locals
    op2 (and sc, the shifter carry, when it changes). Returns 'sc' or None."""
    pc = addr + 8
    if w & (1 << 25):
        imm = w & 0xFF
        rot = ((w >> 8) & 0xF) * 2
        v = ((imm >> rot) | (imm << (32 - rot))) & 0xFFFFFFFF if rot else imm
        code.append(f'u32 op2 = {h(v)};')
        if rot:
            code.append(f'u32 sc = {(v >> 31) & 1};')
            return 'sc'
        return None
    rm = w & 0xF
    kind = (w >> 5) & 3
    if w & 0x10:
        rs = (w >> 8) & 0xF
        pc12 = addr + 12
        code.append(f'u32 sc; u32 op2 = {SHIFT_FN[kind]}({reg(rm, pc12)}, {reg(rs, pc12)} & 0xFF, c->fc, &sc);')
        return 'sc'
    amount = (w >> 7) & 0x1F
    val, carry = imm_shift(kind, amount, 'rmv')
    code.append(f'u32 rmv = {reg(rm, pc)};')
    code.append(f'u32 op2 = {val};')
    if carry is not None:
        code.append(f'u32 sc = {carry};')
        return 'sc'
    return None


def arm_dataproc(w, ins, arch):
    addr = ins.addr
    op = (w >> 21) & 0xF
    s = (w >> 20) & 1
    rn = (w >> 16) & 0xF
    rd = (w >> 12) & 0xF
    code = []
    sc = arm_operand2(w, addr, code)
    pc = addr + 12 if (not (w & (1 << 25)) and (w & 0x10)) else addr + 8
    a = reg(rn, pc)
    res = None
    if op in (0, 1, 8, 9, 12, 13, 14, 15):  # logical
        expr = {0: f'{a} & op2', 1: f'{a} ^ op2', 8: f'{a} & op2', 9: f'{a} ^ op2',
                12: f'{a} | op2', 13: 'op2', 14: f'{a} & ~op2', 15: '~op2'}[op]
        code.append(f'u32 res = {expr};')
        if s and rd != 15:
            code.append('NZ(c, res);')
            if sc:
                code.append(f'c->fc = {sc};')
        res = 'res'
    else:
        fn_s = {2: ('SUBS', a, 'op2'), 3: ('SUBS', 'op2', a), 4: ('ADDS', a, 'op2'), 5: ('ADCS', a, 'op2'),
                6: ('SBCS', a, 'op2'), 7: ('SBCS', 'op2', a), 10: ('SUBS', a, 'op2'), 11: ('ADDS', a, 'op2')}[op]
        plain = {2: f'{a} - op2', 3: f'op2 - {a}', 4: f'{a} + op2', 5: f'{a} + op2 + c->fc',
                 6: f'{a} - op2 - !c->fc', 7: f'op2 - {a} - !c->fc', 10: None, 11: None}[op]
        if s and rd != 15:
            code.append(f'u32 res = {fn_s[0]}(c, {fn_s[1]}, {fn_s[2]});')
        else:
            code.append(f'u32 res = {plain or "0"};')
        res = 'res'
    if op in (8, 9, 10, 11):    # compare: no result written
        if not s:
            raise ValueError('compare without S is not a data-processing instruction')
        if rd == 15:            # TSTP etc. (ARMv2): treat as the compare alone
            pass
        ins.code = ['{ ' + ' '.join(code) + ' }']
        return
    if rd == 15:
        if s:
            code.append(f'return dsr_exception_return(c, {res});')
        else:
            # ARMv4/v5 data-processing writes to the PC do not change the state
            code.append(f'return {res} & ~3u;')
        ins.flow = 'ret'
        ins.code = ['{ ' + ' '.join(code) + ' }']
        return
    code.append(f'R[{rd}] = {res};')
    ins.code = ['{ ' + ' '.join(code) + ' }']


def arm_mul(w, ins):
    acc = (w >> 21) & 1
    s = (w >> 20) & 1
    rd = (w >> 16) & 0xF
    rn = (w >> 12) & 0xF
    rs = (w >> 8) & 0xF
    rm = w & 0xF
    e = f'R[{rm}] * R[{rs}]' + (f' + R[{rn}]' if acc else '')
    c = [f'u32 res = {e};', f'R[{rd}] = res;']
    if s:
        c.append('NZ(c, res);')   # ARMv5 leaves C alone (ARMv4: C is meaningless)
    ins.code = ['{ ' + ' '.join(c) + ' }']


def arm_mull(w, ins):
    signed = (w >> 22) & 1
    acc = (w >> 21) & 1
    s = (w >> 20) & 1
    hi = (w >> 16) & 0xF
    lo = (w >> 12) & 0xF
    rs = (w >> 8) & 0xF
    rm = w & 0xF
    if signed:
        e = f'(u64)((s64)(s32)R[{rm}] * (s64)(s32)R[{rs}])'
    else:
        e = f'(u64)R[{rm}] * (u64)R[{rs}]'
    c = [f'u64 res = {e};']
    if acc:
        c.append(f'res += ((u64)R[{hi}] << 32) | R[{lo}];')
    c.append(f'R[{lo}] = (u32)res; R[{hi}] = (u32)(res >> 32);')
    if s:
        c.append('c->fn = (u32)(res >> 63); c->fz = res == 0;')
    ins.code = ['{ ' + ' '.join(c) + ' }']


def arm_dsp_mul(w, ins):
    op = (w >> 21) & 3
    rd = (w >> 16) & 0xF
    rn = (w >> 12) & 0xF
    rs = (w >> 8) & 0xF
    rm = w & 0xF
    x = (w >> 5) & 1
    y = (w >> 6) & 1
    hx = f'(s32)(s16)(R[{rm}] >> 16)' if x else f'(s32)(s16)R[{rm}]'
    hy = f'(s32)(s16)(R[{rs}] >> 16)' if y else f'(s32)(s16)R[{rs}]'
    if op == 0:     # SMLAxy
        c = f's32 p = {hx} * {hy}; s64 t = (s64)p + (s32)R[{rn}]; if (t != (s32)t) c->fq = 1; R[{rd}] = (u32)t;'
    elif op == 1:
        if x:       # SMULWy
            c = f'R[{rd}] = (u32)(s32)(((s64)(s32)R[{rm}] * {hy}) >> 16);'
        else:       # SMLAWy
            c = f's32 p = (s32)(((s64)(s32)R[{rm}] * {hy}) >> 16); s64 t = (s64)p + (s32)R[{rn}]; if (t != (s32)t) c->fq = 1; R[{rd}] = (u32)t;'
    elif op == 2:   # SMLALxy: RdHi = rd, RdLo = rn
        c = f's64 t = (s64)(((u64)R[{rd}] << 32) | R[{rn}]) + (s64)({hx} * {hy}); R[{rn}] = (u32)t; R[{rd}] = (u32)((u64)t >> 32);'
    else:           # SMULxy
        c = f'R[{rd}] = (u32)({hx} * {hy});'
    ins.code = ['{ ' + c + ' }']


def arm_qadd(w, ins):
    op = (w >> 21) & 3
    rn = (w >> 16) & 0xF
    rd = (w >> 12) & 0xF
    rm = w & 0xF
    b = f'(s64)(s32)R[{rn}]'
    if op >= 2:
        b = f'(s64)(s32)QSAT(c, 2 * (s64)(s32)R[{rn}])'
    e = f'(s64)(s32)R[{rm}] + {b}' if op in (0, 2) else f'(s64)(s32)R[{rm}] - {b}'
    ins.code = [f'{{ R[{rd}] = QSAT(c, {e}); }}']


def arm_halfword(w, ins, arch):
    addr = ins.addr
    p = (w >> 24) & 1
    u = (w >> 23) & 1
    imm = (w >> 22) & 1
    wb = (w >> 21) & 1
    l = (w >> 20) & 1
    rn = (w >> 16) & 0xF
    rd = (w >> 12) & 0xF
    sh = (w >> 5) & 3
    pc = addr + 8
    off = h(((w >> 4) & 0xF0) | (w & 0xF)) if imm else f'R[{w & 0xF}]'
    c = [f'u32 base = {reg(rn, pc)};', f'u32 off = {off};', f'u32 ea = base {"+" if u else "-"} off;']
    c.append(f'u32 a = {"ea" if p else "base"};')
    write_back = (not p) or wb
    tail = []
    if l:
        kind = {1: 'ldrh', 2: 'ldrsb', 3: 'ldrsh'}[sh]
        if kind == 'ldrh':
            c.append('u32 v = ldrh(c, a);')
        elif kind == 'ldrsb':
            c.append('u32 v = (u32)(s32)(s8)rd8(c, a);')
        else:
            c.append('u32 v = ldrsh(c, a);')
        if write_back and rn != 15:
            c.append(f'R[{rn}] = ea;')
        if rd == 15:
            c.append('return dsr_ldpc(c, v);')
            ins.flow = 'ret'
        else:
            c.append(f'R[{rd}] = v;')
    else:
        if sh == 1:     # STRH
            c.append(f'wr16(c, a, {reg(rd, addr + 12)});')
            if write_back and rn != 15:
                c.append(f'R[{rn}] = ea;')
        elif arch < 5:
            undefined(ins, w)       # (with its condition dropped: a conditional stop would fall off the function)
            return
        elif sh == 2:   # LDRD
            if rd & 1 or rd == 14:
                undefined(ins, w)
                return
            c.append(f'u32 v0 = rd32(c, a), v1 = rd32(c, a + 4);')
            if write_back and rn != 15:
                c.append(f'R[{rn}] = ea;')
            c.append(f'R[{rd}] = v0; R[{rd + 1}] = v1;')
        else:           # STRD
            if rd & 1 or rd == 14:
                undefined(ins, w)
                return
            c.append(f'wr32(c, a, R[{rd}]); wr32(c, a + 4, R[{rd + 1}]);')
            if write_back and rn != 15:
                c.append(f'R[{rn}] = ea;')
    ins.code = ['{ ' + ' '.join(c + tail) + ' }']


def arm_single(w, ins, arch):
    addr = ins.addr
    pc = addr + 8
    regoff = (w >> 25) & 1
    p = (w >> 24) & 1
    u = (w >> 23) & 1
    b = (w >> 22) & 1
    wb = (w >> 21) & 1
    l = (w >> 20) & 1
    rn = (w >> 16) & 0xF
    rd = (w >> 12) & 0xF
    c = []
    if regoff:
        rm = w & 0xF
        val, _ = imm_shift((w >> 5) & 3, (w >> 7) & 0x1F, reg(rm, pc))
        off = val
    else:
        off = h(w & 0xFFF)
        if rn == 15 and p and not wb:
            lit = (pc + (w & 0xFFF) if u else pc - (w & 0xFFF)) & 0xFFFFFFFF
            if not b and l:
                ins.literals.append(lit)
    c += [f'u32 base = {reg(rn, pc)};', f'u32 off = {off};', f'u32 ea = base {"+" if u else "-"} off;',
          f'u32 a = {"ea" if p else "base"};']
    write_back = ((not p) or wb) and rn != 15
    if l:
        c.append('u32 v = rd8(c, a);' if b else 'u32 v = ldr32(c, a);')
        if write_back:
            c.append(f'R[{rn}] = ea;')
        if rd == 15:
            c.append('return dsr_ldpc(c, v);')
            ins.flow = 'ret'
        else:
            c.append(f'R[{rd}] = v;')
    else:
        v = reg(rd, addr + 12)
        c.append(f'u32 sv = {v};')
        c.append('wr8(c, a, sv);' if b else 'wr32(c, a, sv);')
        if write_back:
            c.append(f'R[{rn}] = ea;')
    ins.code = ['{ ' + ' '.join(c) + ' }']


def block_transfer(ins, arch, rn, rlist, p, u, s, wb, l, pc_store, user_regs):
    """LDM/STM (also Thumb PUSH/POP/LDMIA/STMIA). Emits into ins.code."""
    empty = rlist == 0
    if empty:       # ARMv4/v5: transfers the PC, the base moves by 0x40
        rlist = 1 << 15
        n = 16
    else:
        n = popcount(rlist)
    regs = [i for i in range(16) if rlist >> i & 1]
    c = [f'u32 base = R[{rn}];']
    if u:
        start = 'base + 4' if p else 'base'
        wbv = f'base + {4 * n}'
    else:
        start = f'base - {4 * n}' if p else f'base - {4 * n - 4}'
        wbv = f'base - {4 * n}'
    c.append(f'u32 a = {start}; u32 wbv = {wbv};')
    in_list = bool(rlist >> rn & 1)
    if l:
        loads_pc = 15 in regs
        for r in regs:
            if r == 15:
                c.append('u32 npc = rd32(c, a); a += 4;')
            elif s and not loads_pc and user_regs:
                c.append(f'dsr_set_user_reg(c, {r}, rd32(c, a)); a += 4;')
            else:
                c.append(f'R[{r}] = rd32(c, a); a += 4;')
        do_wb = wb
        if wb and in_list:
            if arch < 5:
                do_wb = False
            else:   # ARMv5: written back if Rn is the only register or not the last one
                do_wb = len(regs) == 1 or regs[-1] != rn
        if do_wb:
            c.append(f'R[{rn}] = wbv;')
        if loads_pc:
            if s:
                c.append('return dsr_exception_return(c, npc);')
            else:
                c.append('return dsr_ldpc(c, npc);')
            ins.flow = 'ret'
    else:
        first = regs[0] if regs else -1
        for r in regs:
            if r == 15:
                v = h(pc_store)
            elif r == rn and wb and arch < 5 and r != first:
                v = 'wbv'       # ARMv4 stores the new base unless it is first
            elif s and user_regs:
                v = f'dsr_get_user_reg(c, {r})'
            else:
                v = f'R[{r}]'
            c.append(f'wr32(c, a, {v}); a += 4;')
        if wb:
            c.append(f'R[{rn}] = wbv;')
    ins.code = ['{ ' + ' '.join(c) + ' }']


def arm_block(w, ins, arch):
    block_transfer(ins, arch, (w >> 16) & 0xF, w & 0xFFFF, (w >> 24) & 1, (w >> 23) & 1,
                   (w >> 22) & 1, (w >> 21) & 1, (w >> 20) & 1, ins.addr + 12, True)


def undefined(ins, w):
    ins.code = [f'return dsr_undefined(c, {h(ins.addr)}, {h(w)});']
    ins.flow = 'stop'
    ins.cond = None
    return ins


def decode_arm(addr, w, arch=5):
    ins = Insn(addr, 4, False, word=w)
    cond = w >> 28
    if cond == 0xF:
        if arch >= 5 and (w & 0x0E000000) == 0x0A000000:      # BLX imm
            off = sx(w & 0xFFFFFF, 24) * 4 + ((w >> 24) & 1) * 2
            ins.target = (addr + 8 + off) & 0xFFFFFFFF
            ins.target_thumb = True
            ins.code = [f'R[14] = {h(addr + 4)};', 'c->t = 1;']
            ins.flow = 'call'
            return ins
        if arch >= 5 and (w & 0x0D70F000) == 0x0550F000:      # PLD
            return ins
        return undefined(ins, w)
    ins.cond = COND[cond]
    top = (w >> 25) & 7
    if top == 0:
        if (w & 0x0FFFFFF0) == 0x012FFF10:      # BX
            ins.code = [f'return dsr_bx(c, {reg(w & 0xF, addr + 8)});']
            ins.flow = 'ret'
            return ins
        if (w & 0x0FFFFFF0) == 0x012FFF30:      # BLX reg
            if arch < 5:
                return undefined(ins, w)
            ins.code = [f'{{ u32 t = {reg(w & 0xF, addr + 8)}; R[14] = {h(addr + 4)}; return dsr_bx(c, t); }}']
            ins.flow = 'icall'
            return ins
        if (w & 0x0FFF0FF0) == 0x016F0F10:      # CLZ
            if arch < 5:
                return undefined(ins, w)
            rm, rd = w & 0xF, (w >> 12) & 0xF
            ins.code = [f'{{ u32 v = R[{rm}]; R[{rd}] = v ? (u32)__builtin_clz(v) : 32u; }}']
            return ins
        if (w & 0x0F900FF0) == 0x01000050:      # QADD/QSUB/QDADD/QDSUB
            if arch < 5:
                return undefined(ins, w)
            arm_qadd(w, ins)
            return ins
        if (w & 0x0FF000F0) == 0x01200070:      # BKPT
            return undefined(ins, w)
        if (w & 0x0FBF0FFF) == 0x010F0000:      # MRS
            rd = (w >> 12) & 0xF
            src = 'dsr_get_spsr(c)' if w & (1 << 22) else 'dsr_cpsr(c)'
            ins.code = [f'R[{rd}] = {src};']
            return ins
        if (w & 0x0FB0FFF0) == 0x0120F000:      # MSR reg
            spsr = (w >> 22) & 1
            fm = (w >> 16) & 0xF
            ins.code = [f'dsr_msr(c, {spsr}, {fm}, R[{w & 0xF}]);']
            if not spsr and fm & 1:
                ins.flow = 'split'
            return ins
        if (w & 0x0F900090) == 0x01000080:      # SMLAxy etc.
            if arch < 5:
                return undefined(ins, w)
            arm_dsp_mul(w, ins)
            return ins
        if (w & 0x0FC000F0) == 0x00000090:
            arm_mul(w, ins)
            return ins
        if (w & 0x0F8000F0) == 0x00800090:
            arm_mull(w, ins)
            return ins
        if (w & 0x0FB00FF0) == 0x01000090:      # SWP/SWPB
            b = (w >> 22) & 1
            rn, rd, rm = (w >> 16) & 0xF, (w >> 12) & 0xF, w & 0xF
            if b:
                ins.code = [f'{{ u32 a = R[{rn}]; u32 v = rd8(c, a); wr8(c, a, R[{rm}]); R[{rd}] = v; }}']
            else:
                ins.code = [f'{{ u32 a = R[{rn}]; u32 v = ldr32(c, a); wr32(c, a, R[{rm}]); R[{rd}] = v; }}']
            return ins
        if (w & 0x0E000090) == 0x00000090:
            if (w >> 5) & 3:
                arm_halfword(w, ins, arch)
                return ins
            return undefined(ins, w)
        if ((w >> 23) & 3) == 2 and not (w >> 20) & 1:
            return undefined(ins, w)
        arm_dataproc(w, ins, arch)
        return ins
    if top == 1:
        if (w & 0x0FB0F000) == 0x0320F000:      # MSR imm
            spsr = (w >> 22) & 1
            fm = (w >> 16) & 0xF
            imm = w & 0xFF
            rot = ((w >> 8) & 0xF) * 2
            v = ((imm >> rot) | (imm << (32 - rot))) & 0xFFFFFFFF if rot else imm
            ins.code = [f'dsr_msr(c, {spsr}, {fm}, {h(v)});']
            if not spsr and fm & 1:
                ins.flow = 'split'
            return ins
        if ((w >> 23) & 3) == 2 and not (w >> 20) & 1:
            return undefined(ins, w)
        arm_dataproc(w, ins, arch)
        return ins
    if top == 2 or (top == 3 and not (w & 0x10)):
        arm_single(w, ins, arch)
        return ins
    if top == 3:
        return undefined(ins, w)
    if top == 4:
        arm_block(w, ins, arch)
        return ins
    if top == 5:
        off = sx(w & 0xFFFFFF, 24) * 4
        ins.target = (addr + 8 + off) & 0xFFFFFFFF
        ins.target_thumb = False
        if w & (1 << 24):
            ins.code = [f'R[14] = {h(addr + 4)};']
            ins.flow = 'call'
        else:
            ins.flow = 'jump'
        return ins
    if top == 6:
        return undefined(ins, w)
    # top == 7
    if w & (1 << 24):
        num = (w >> 16) & 0xFF
        ins.code = [f'{{ u32 r_ = dsr_swi(c, {num}, {h(addr + 4)}); if (r_) return r_; }}']
        ins.flow = 'split'
        return ins
    if w & 0x10:
        cp = (w >> 8) & 0xF
        if cp != 15 or arch < 5:
            return undefined(ins, w)
        l = (w >> 20) & 1
        crn, rd, crm, op2 = (w >> 16) & 0xF, (w >> 12) & 0xF, w & 0xF, (w >> 5) & 7
        if l:
            if rd == 15:
                ins.code = [f'{{ u32 v = dsr_cp15_read(c, {crn}, {crm}, {op2}); c->fn = v >> 31; c->fz = (v >> 30) & 1; c->fc = (v >> 29) & 1; c->fv = (v >> 28) & 1; }}']
            else:
                ins.code = [f'R[{rd}] = dsr_cp15_read(c, {crn}, {crm}, {op2});']
        else:
            ins.code = [f'if (dsr_cp15_write(c, {crn}, {crm}, {op2}, {reg(rd, addr + 8)})) return {h(addr + 4)};']
            ins.flow = 'split'
        return ins
    return undefined(ins, w)


# ---------------------------------------------------------------------------
# Thumb
# ---------------------------------------------------------------------------

def decode_thumb(addr, hw, next_hw=None, arch=5):
    ins = Insn(addr, 2, True, word=hw)
    pc = addr + 4
    op = hw >> 13
    if op == 0:
        if (hw >> 11) & 3 != 3:     # format 1: shift by immediate
            kind = (hw >> 11) & 3
            amount = (hw >> 6) & 0x1F
            rs, rd = (hw >> 3) & 7, hw & 7
            val, carry = imm_shift(kind, amount, 'v')
            c = [f'u32 v = R[{rs}];', f'u32 res = {val};']
            if carry:
                c.append(f'c->fc = {carry};')
            c.append(f'NZ(c, res); R[{rd}] = res;')
            ins.code = ['{ ' + ' '.join(c) + ' }']
            return ins
        # format 2: add/sub
        imm = (hw >> 10) & 1
        sub = (hw >> 9) & 1
        rn = (hw >> 6) & 7
        rs, rd = (hw >> 3) & 7, hw & 7
        b = h(rn) if imm else f'R[{rn}]'
        ins.code = [f'R[{rd}] = {"SUBS" if sub else "ADDS"}(c, R[{rs}], {b});']
        return ins
    if op == 1:     # format 3
        o = (hw >> 11) & 3
        rd = (hw >> 8) & 7
        imm = hw & 0xFF
        ins.code = [[f'{{ R[{rd}] = {h(imm)}; NZ(c, R[{rd}]); }}',
                     f'SUBS(c, R[{rd}], {h(imm)});',
                     f'R[{rd}] = ADDS(c, R[{rd}], {h(imm)});',
                     f'R[{rd}] = SUBS(c, R[{rd}], {h(imm)});'][o]]
        return ins
    if op == 2:
        if (hw >> 10) == 0x10:      # format 4: ALU
            o = (hw >> 6) & 0xF
            rs, rd = (hw >> 3) & 7, hw & 7
            d, s = f'R[{rd}]', f'R[{rs}]'
            code = {
                0: f'{d} &= {s}; NZ(c, {d});',
                1: f'{d} ^= {s}; NZ(c, {d});',
                2: f'{{ u32 co; {d} = LSL_R({d}, {s}, c->fc, &co); c->fc = co; NZ(c, {d}); }}',
                3: f'{{ u32 co; {d} = LSR_R({d}, {s}, c->fc, &co); c->fc = co; NZ(c, {d}); }}',
                4: f'{{ u32 co; {d} = ASR_R({d}, {s}, c->fc, &co); c->fc = co; NZ(c, {d}); }}',
                5: f'{d} = ADCS(c, {d}, {s});',
                6: f'{d} = SBCS(c, {d}, {s});',
                7: f'{{ u32 co; {d} = ROR_R({d}, {s}, c->fc, &co); c->fc = co; NZ(c, {d}); }}',
                8: f'NZ(c, {d} & {s});',
                9: f'{d} = SUBS(c, 0, {s});',
                10: f'SUBS(c, {d}, {s});',
                11: f'ADDS(c, {d}, {s});',
                12: f'{d} |= {s}; NZ(c, {d});',
                13: f'{d} = {d} * {s}; NZ(c, {d});',
                14: f'{d} &= ~{s}; NZ(c, {d});',
                15: f'{d} = ~{s}; NZ(c, {d});',
            }[o]
            ins.code = ['{ ' + code + ' }']
            return ins
        if (hw >> 10) == 0x11:      # format 5: hi registers, BX
            o = (hw >> 8) & 3
            h1 = (hw >> 7) & 1
            h2 = (hw >> 6) & 1
            rs = ((hw >> 3) & 7) | (h2 << 3)
            rd = (hw & 7) | (h1 << 3)
            src = reg(rs, pc)
            if o == 3:
                if h1:      # BLX reg
                    if arch < 5:
                        return undefined(ins, hw)
                    ins.code = [f'{{ u32 t = {src}; R[14] = {h((addr + 2) | 1)}; return dsr_bx(c, t); }}']
                    ins.flow = 'icall'
                elif rs == 15:      # bx pc: the ARM code 4 bytes on (an interworking veneer)
                    ins.flow = 'jump'
                    ins.target = (addr + 4) & ~3
                    ins.target_thumb = False
                else:
                    ins.code = [f'return dsr_bx(c, {src});']
                    ins.flow = 'ret'
                return ins
            if o == 1:
                ins.code = [f'SUBS(c, {reg(rd, pc)}, {src});']
                return ins
            val = f'{reg(rd, pc)} + {src}' if o == 0 else src
            if rd == 15:
                ins.code = [f'return ({val}) & ~1u;']
                ins.flow = 'ret'
            else:
                ins.code = [f'R[{rd}] = {val};']
            return ins
        if (hw >> 11) == 9:     # format 6: PC-relative load
            rd = (hw >> 8) & 7
            a = ((pc & ~3) + (hw & 0xFF) * 4) & 0xFFFFFFFF
            ins.literals.append(a)
            ins.code = [f'R[{rd}] = rd32(c, {h(a)});']
            return ins
        # formats 7 and 8: register offset
        ro, rb, rd = (hw >> 6) & 7, (hw >> 3) & 7, hw & 7
        a = f'R[{rb}] + R[{ro}]'
        if hw & (1 << 9):
            o = (hw >> 10) & 3
            ins.code = [[f'wr16(c, {a}, R[{rd}]);',
                         f'R[{rd}] = (u32)(s32)(s8)rd8(c, {a});',
                         f'R[{rd}] = ldrh(c, {a});',
                         f'R[{rd}] = ldrsh(c, {a});'][o]]
        else:
            o = (hw >> 10) & 3
            ins.code = [[f'wr32(c, {a}, R[{rd}]);',
                         f'wr8(c, {a}, R[{rd}]);',
                         f'R[{rd}] = ldr32(c, {a});',
                         f'R[{rd}] = rd8(c, {a});'][o]]
        return ins
    if op == 3:     # format 9
        b = (hw >> 12) & 1
        l = (hw >> 11) & 1
        off = (hw >> 6) & 0x1F
        rb, rd = (hw >> 3) & 7, hw & 7
        if b:
            a = f'R[{rb}] + {off}'
            ins.code = [f'R[{rd}] = rd8(c, {a});' if l else f'wr8(c, {a}, R[{rd}]);']
        else:
            a = f'R[{rb}] + {off * 4}'
            ins.code = [f'R[{rd}] = ldr32(c, {a});' if l else f'wr32(c, {a}, R[{rd}]);']
        return ins
    if op == 4:
        l = (hw >> 11) & 1
        if not hw & (1 << 12):      # format 10: halfword
            off = ((hw >> 6) & 0x1F) * 2
            rb, rd = (hw >> 3) & 7, hw & 7
            a = f'R[{rb}] + {off}'
            ins.code = [f'R[{rd}] = ldrh(c, {a});' if l else f'wr16(c, {a}, R[{rd}]);']
        else:                       # format 11: SP-relative
            rd = (hw >> 8) & 7
            a = f'R[13] + {(hw & 0xFF) * 4}'
            ins.code = [f'R[{rd}] = ldr32(c, {a});' if l else f'wr32(c, {a}, R[{rd}]);']
        return ins
    if op == 5:
        if not hw & (1 << 12):      # format 12: load address
            rd = (hw >> 8) & 7
            imm = (hw & 0xFF) * 4
            if hw & (1 << 11):
                ins.code = [f'R[{rd}] = R[13] + {imm};']
            else:
                ins.code = [f'R[{rd}] = {h((pc & ~3) + imm)};']
            return ins
        if (hw >> 8) == 0xB0:       # format 13: add to SP
            imm = (hw & 0x7F) * 4
            ins.code = [f'R[13] {"-" if hw & 0x80 else "+"}= {imm};']
            return ins
        if (hw >> 9) & 3 == 2:      # format 14: push/pop
            l = (hw >> 11) & 1
            rl = hw & 0xFF
            if hw & 0x100:
                rl |= (1 << 15) if l else (1 << 14)
            if l:
                block_transfer(ins, arch, 13, rl, 0, 1, 0, 1, 1, 0, False)
            else:
                block_transfer(ins, arch, 13, rl, 1, 0, 0, 1, 0, 0, False)
            return ins
        if (hw >> 8) == 0xBE:       # BKPT
            return undefined(ins, hw)
        return undefined(ins, hw)
    if op == 6:
        if not hw & (1 << 12):      # format 15: LDMIA/STMIA
            l = (hw >> 11) & 1
            rb = (hw >> 8) & 7
            rl = hw & 0xFF
            block_transfer(ins, arch, rb, rl, 0, 1, 0, 1, l, addr + 6, False)
            return ins
        cond = (hw >> 8) & 0xF
        if cond == 0xF:             # format 17: SWI
            ins.code = [f'{{ u32 r_ = dsr_swi(c, {hw & 0xFF}, {h(addr + 2)}); if (r_) return r_; }}']
            ins.flow = 'split'
            return ins
        if cond == 0xE:
            return undefined(ins, hw)
        ins.cond = COND[cond]       # format 16: conditional branch
        ins.target = (pc + sx(hw & 0xFF, 8) * 2) & 0xFFFFFFFF
        ins.target_thumb = True
        ins.flow = 'jump'
        return ins
    # op == 7
    o = (hw >> 11) & 3
    if o == 0:                      # format 18: B
        ins.target = (pc + sx(hw & 0x7FF, 11) * 2) & 0xFFFFFFFF
        ins.target_thumb = True
        ins.flow = 'jump'
        return ins
    if o == 2:                      # BL/BLX prefix
        hi = sx(hw & 0x7FF, 11) << 12
        if next_hw is not None and (next_hw >> 11) in (0x1F, 0x1D) and not (arch < 5 and (next_hw >> 11) == 0x1D):
            lo = (next_hw & 0x7FF) << 1
            t = (pc + hi + lo) & 0xFFFFFFFF
            ins.size = 4
            ins.pair = True
            ins.flow = 'call'
            if (next_hw >> 11) == 0x1F:
                ins.target, ins.target_thumb = t, True
                ins.code = [f'R[14] = {h((addr + 4) | 1)};']
            else:
                if next_hw & 1:
                    return undefined(ins, hw)
                ins.target, ins.target_thumb = t & ~3, False
                ins.code = [f'R[14] = {h((addr + 4) | 1)};', 'c->t = 0;']
            return ins
        ins.code = [f'R[14] = {h((pc + hi) & 0xFFFFFFFF)};']
        return ins
    # a BL/BLX suffix on its own: jump to LR + offset
    lo = (hw & 0x7FF) << 1
    if o == 3:
        ins.code = [f'{{ u32 t = R[14] + {lo}; R[14] = {h((addr + 2) | 1)}; return t & ~1u; }}']
    else:
        if arch < 5:
            return undefined(ins, hw)
        ins.code = [f'{{ u32 t = (R[14] + {lo}) & ~3u; R[14] = {h((addr + 2) | 1)}; c->t = 0; return t; }}']
    ins.flow = 'ret'
    return ins
