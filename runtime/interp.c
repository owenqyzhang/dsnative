/* dsnative runtime: an interpreter for the code the recompiler could not compile.
 *
 * Two kinds of code have no compiled entry: code the game modifies while it runs (an overlay
 * function patched in place, so its bytes no longer match what was compiled) and code
 * discovery never found (a function copied to RAM, a jump the evaluator could not follow). The
 * dispatcher hands the address to dsr_interp, which decodes and executes ARM and Thumb
 * instructions one at a time until control reaches an address the dispatcher can run, and
 * returns that address as a recompiled function does. It generates no code, so it is allowed
 * where a JIT is not (iOS, tvOS).
 *
 * The semantics are those of recomp/isa.py, and this file keeps its structure and its order
 * of decoding on purpose: the same classes tested in the same order, the same helpers of
 * dsr.h for flags, shifter operands, loads and stores, interworking, CP15 and SWI. The
 * quirks are kept too (an undefined encoding stops even when its condition fails; a
 * data-processing write to the PC never interworks). tests/difftest.py runs the same snippets
 * through the interpreter, the recompiled code and Unicorn, and compares random single
 * instructions of every class between the interpreter and the recompiled code.
 *
 * Register reads of the PC are computed from the instruction's address (R[15] holds the
 * address of the instruction being run, as a dispatch to it would leave it). Budget is one
 * per instruction, charged whether or not its condition passes. Interrupts are taken as the
 * dispatcher takes them between compiled functions, but after every instruction rather than
 * only at a function's end: dsr_interp_exit says when to go back.
 */
#include <stdio.h>
#include <stdlib.h>
#include "dsr.h"

typedef struct {
    u32 next;       /* the address to continue at */
    int call;       /* the instruction was a call: its target is a function entry */
    int leave;      /* back to the dispatcher whatever the address is (a BIOS call that waits, a halt) */
} Step;

static int cond_ok(Cpu *c, u32 cc) {
    switch (cc) {
    case 0: return c->fz;
    case 1: return !c->fz;
    case 2: return c->fc;
    case 3: return !c->fc;
    case 4: return c->fn;
    case 5: return !c->fn;
    case 6: return c->fv;
    case 7: return !c->fv;
    case 8: return c->fc && !c->fz;
    case 9: return !c->fc || c->fz;
    case 10: return c->fn == c->fv;
    case 11: return c->fn != c->fv;
    case 12: return !c->fz && c->fn == c->fv;
    case 13: return c->fz || c->fn != c->fv;
    default: return 1;
    }
}

/* a register as an instruction reads it: the PC is a constant of the instruction's address */
#define RG(n, pcv) ((n) == 15 ? (pcv) : c->r[n])

static s32 sx(u32 v, int bits) { u32 m = 1u << (bits - 1); return (s32)((v & (m - 1)) - (v & m)); }

/* shift by an immediate: a zero amount means 32 for LSR and ASR and RRX for ROR; *chg says
 * whether the shifter carry replaces C (LSL #0 leaves it alone) */
static u32 shift_imm(Cpu *c, u32 kind, u32 n, u32 v, u32 *co, int *chg) {
    *chg = 1;
    switch (kind) {
    case 0:
        if (!n) { *chg = 0; return v; }
        *co = (v >> (32 - n)) & 1; return v << n;
    case 1:
        if (!n) { *co = v >> 31; return 0; }
        *co = (v >> (n - 1)) & 1; return v >> n;
    case 2:
        if (!n) { *co = v >> 31; return (u32)((s32)v >> 31); }
        *co = (v >> (n - 1)) & 1; return (u32)((s32)v >> n);
    default:
        if (!n) { *co = v & 1; return (c->fc << 31) | (v >> 1); }
        *co = (v >> (n - 1)) & 1; return ror32(v, n);
    }
}

static void undef(Cpu *c, u32 pc, u32 w, Step *st) { st->next = dsr_undefined(c, pc, w); }

/* ---- ARM -------------------------------------------------------------------------------- */
static void arm_dataproc(Cpu *c, u32 pc, u32 w, Step *st) {
    u32 op = (w >> 21) & 15, s = (w >> 20) & 1, rn = (w >> 16) & 15, rd = (w >> 12) & 15;
    u32 op2, sc = 0, pcv = pc + 8;
    int has_sc = 0;
    if (w & (1u << 25)) {
        u32 rot = ((w >> 8) & 15) * 2;
        op2 = ror32(w & 0xFF, rot);
        if (rot) { has_sc = 1; sc = op2 >> 31; }
    } else if (w & 0x10) {
        u32 rm = RG(w & 15, pc + 12), rs = RG((w >> 8) & 15, pc + 12) & 0xFF;
        switch ((w >> 5) & 3) {
        case 0: op2 = LSL_R(rm, rs, c->fc, &sc); break;
        case 1: op2 = LSR_R(rm, rs, c->fc, &sc); break;
        case 2: op2 = ASR_R(rm, rs, c->fc, &sc); break;
        default: op2 = ROR_R(rm, rs, c->fc, &sc); break;
        }
        has_sc = 1;
        pcv = pc + 12;
    } else {
        op2 = shift_imm(c, (w >> 5) & 3, (w >> 7) & 31, RG(w & 15, pc + 8), &sc, &has_sc);
    }
    u32 a = RG(rn, pcv), res = 0;
    int flags = s && rd != 15;
    switch (op) {
    case 0: case 8: res = a & op2; break;
    case 1: case 9: res = a ^ op2; break;
    case 12: res = a | op2; break;
    case 13: res = op2; break;
    case 14: res = a & ~op2; break;
    case 15: res = ~op2; break;
    case 2: res = flags ? SUBS(c, a, op2) : a - op2; break;
    case 3: res = flags ? SUBS(c, op2, a) : op2 - a; break;
    case 4: res = flags ? ADDS(c, a, op2) : a + op2; break;
    case 5: res = flags ? ADCS(c, a, op2) : a + op2 + c->fc; break;
    case 6: res = flags ? SBCS(c, a, op2) : a - op2 - !c->fc; break;
    case 7: res = flags ? SBCS(c, op2, a) : op2 - a - !c->fc; break;
    case 10: if (flags) SUBS(c, a, op2); break;
    case 11: if (flags) ADDS(c, a, op2); break;
    }
    if (!((op >= 2 && op <= 7) || op == 10 || op == 11) && flags) {      /* the logical operations */
        NZ(c, res);
        if (has_sc) c->fc = sc;
    }
    if (op >= 8 && op <= 11) return;        /* compares write no result */
    if (rd == 15) { st->next = s ? dsr_exception_return(c, res) : res & ~3u; return; }
    R[rd] = res;
}

static void arm_mul(Cpu *c, u32 w) {
    u32 rd = (w >> 16) & 15, rn = (w >> 12) & 15, rs = (w >> 8) & 15, rm = w & 15;
    u32 res = R[rm] * R[rs] + (((w >> 21) & 1) ? R[rn] : 0);
    R[rd] = res;
    if ((w >> 20) & 1) NZ(c, res);        /* ARMv5 leaves C alone (ARMv4: C is meaningless) */
}

static void arm_mull(Cpu *c, u32 w) {
    u32 hi = (w >> 16) & 15, lo = (w >> 12) & 15, rs = (w >> 8) & 15, rm = w & 15;
    u64 res = ((w >> 22) & 1) ? (u64)((s64)(s32)R[rm] * (s64)(s32)R[rs]) : (u64)R[rm] * (u64)R[rs];
    if ((w >> 21) & 1) res += ((u64)R[hi] << 32) | R[lo];
    R[lo] = (u32)res; R[hi] = (u32)(res >> 32);
    if ((w >> 20) & 1) { c->fn = (u32)(res >> 63); c->fz = res == 0; }
}

static void arm_dsp_mul(Cpu *c, u32 w) {
    u32 op = (w >> 21) & 3, rd = (w >> 16) & 15, rn = (w >> 12) & 15, rs = (w >> 8) & 15, rm = w & 15;
    u32 x = (w >> 5) & 1, y = (w >> 6) & 1;
    s32 hx = x ? (s32)(s16)(R[rm] >> 16) : (s32)(s16)R[rm];
    s32 hy = y ? (s32)(s16)(R[rs] >> 16) : (s32)(s16)R[rs];
    if (op == 0) {          /* SMLAxy */
        s32 p = hx * hy; s64 t = (s64)p + (s32)R[rn];
        if (t != (s32)t) c->fq = 1;
        R[rd] = (u32)t;
    } else if (op == 1) {
        if (x) {            /* SMULWy */
            R[rd] = (u32)(s32)(((s64)(s32)R[rm] * hy) >> 16);
        } else {            /* SMLAWy */
            s32 p = (s32)(((s64)(s32)R[rm] * hy) >> 16); s64 t = (s64)p + (s32)R[rn];
            if (t != (s32)t) c->fq = 1;
            R[rd] = (u32)t;
        }
    } else if (op == 2) {   /* SMLALxy: RdHi = rd, RdLo = rn */
        s64 t = (s64)(((u64)R[rd] << 32) | R[rn]) + (s64)(hx * hy);
        R[rn] = (u32)t; R[rd] = (u32)((u64)t >> 32);
    } else {                /* SMULxy */
        R[rd] = (u32)(hx * hy);
    }
}

static void arm_qadd(Cpu *c, u32 w) {
    u32 op = (w >> 21) & 3, rn = (w >> 16) & 15, rd = (w >> 12) & 15, rm = w & 15;
    s64 b = (s64)(s32)R[rn];
    if (op >= 2) b = (s64)(s32)QSAT(c, 2 * (s64)(s32)R[rn]);
    s64 e = (op == 0 || op == 2) ? (s64)(s32)R[rm] + b : (s64)(s32)R[rm] - b;
    R[rd] = QSAT(c, e);
}

static void arm_halfword(Cpu *c, u32 pc, u32 w, Step *st) {
    u32 p = (w >> 24) & 1, u = (w >> 23) & 1, imm = (w >> 22) & 1, wb = (w >> 21) & 1, l = (w >> 20) & 1;
    u32 rn = (w >> 16) & 15, rd = (w >> 12) & 15, sh = (w >> 5) & 3;
    u32 base = RG(rn, pc + 8);
    u32 off = imm ? (((w >> 4) & 0xF0) | (w & 0xF)) : R[w & 0xF];
    u32 ea = u ? base + off : base - off;
    u32 a = p ? ea : base;
    int write_back = !p || wb;
    if (l) {
        u32 v = sh == 1 ? ldrh(c, a) : sh == 2 ? (u32)(s32)(s8)rd8(c, a) : ldrsh(c, a);
        if (write_back && rn != 15) R[rn] = ea;
        if (rd == 15) { st->next = dsr_ldpc(c, v); return; }
        R[rd] = v;
        return;
    }
    if (sh == 1) {          /* STRH */
        wr16(c, a, RG(rd, pc + 12));
    } else if (sh == 2) {   /* LDRD */
        u32 v0 = rd32(c, a), v1 = rd32(c, a + 4);
        if (write_back && rn != 15) R[rn] = ea;
        R[rd] = v0; R[rd + 1] = v1;
        return;
    } else {                /* STRD */
        wr32(c, a, R[rd]); wr32(c, a + 4, R[rd + 1]);
    }
    if (write_back && rn != 15) R[rn] = ea;
}

static void arm_single(Cpu *c, u32 pc, u32 w, Step *st) {
    u32 regoff = (w >> 25) & 1, p = (w >> 24) & 1, u = (w >> 23) & 1, b = (w >> 22) & 1;
    u32 wb = (w >> 21) & 1, l = (w >> 20) & 1, rn = (w >> 16) & 15, rd = (w >> 12) & 15;
    u32 off, base = RG(rn, pc + 8);
    if (regoff) {
        int chg; u32 co;
        off = shift_imm(c, (w >> 5) & 3, (w >> 7) & 31, RG(w & 15, pc + 8), &co, &chg);
    } else {
        off = w & 0xFFF;
    }
    u32 ea = u ? base + off : base - off;
    u32 a = p ? ea : base;
    int write_back = (!p || wb) && rn != 15;
    if (l) {
        u32 v = b ? rd8(c, a) : ldr32(c, a);
        if (write_back) R[rn] = ea;
        if (rd == 15) { st->next = dsr_ldpc(c, v); return; }
        R[rd] = v;
    } else {
        u32 sv = RG(rd, pc + 12);
        if (b) wr8(c, a, sv); else wr32(c, a, sv);
        if (write_back) R[rn] = ea;
    }
}

/* LDM/STM and Thumb PUSH/POP/LDMIA/STMIA */
static void block_transfer(Cpu *c, Step *st, u32 rn, u32 rlist, int p, int u, int s, int wb, int l,
                           u32 pc_store, int user_regs) {
    u32 n, first, last;
    if (!rlist) rlist = 1u << 15, n = 16;     /* ARMv4/v5: transfers the PC, the base moves by 0x40 */
    else n = (u32)__builtin_popcount(rlist);
    first = (u32)__builtin_ctz(rlist); last = 31u - (u32)__builtin_clz(rlist);
    u32 count = (u32)__builtin_popcount(rlist);
    u32 base = R[rn];
    u32 a = u ? (p ? base + 4 : base) : (p ? base - 4 * n : base - 4 * n + 4);
    u32 wbv = u ? base + 4 * n : base - 4 * n;
    if (l) {
        int loads_pc = (rlist >> 15) & 1;
        u32 npc = 0;
        for (u32 r = 0; r < 16; r++) {
            if (!((rlist >> r) & 1)) continue;
            if (r == 15) npc = rd32(c, a);
            else if (s && !loads_pc && user_regs) dsr_set_user_reg(c, (int)r, rd32(c, a));
            else R[r] = rd32(c, a);
            a += 4;
        }
        int do_wb = wb;
        if (wb && ((rlist >> rn) & 1)) {
            if (c->arch < 5) do_wb = 0;
            else do_wb = count == 1 || last != rn;     /* ARMv5: written back unless Rn is the last */
        }
        if (do_wb) R[rn] = wbv;
        if (loads_pc) st->next = s ? dsr_exception_return(c, npc) : dsr_ldpc(c, npc);
    } else {
        for (u32 r = 0; r < 16; r++) {
            if (!((rlist >> r) & 1)) continue;
            u32 v;
            if (r == 15) v = pc_store;
            else if (r == rn && wb && c->arch < 5 && r != first) v = wbv;     /* ARMv4 stores the new base unless first */
            else if (s && user_regs) v = dsr_get_user_reg(c, (int)r);
            else v = R[r];
            wr32(c, a, v);
            a += 4;
        }
        if (wb) R[rn] = wbv;
    }
}

enum { A_UNDEF, A_BX, A_BLX, A_CLZ, A_QADD, A_MRS, A_MSR_R, A_MSR_I, A_DSP, A_MUL, A_MULL, A_SWP, A_HALF,
       A_DP, A_SINGLE, A_BLOCK, A_B, A_SWI, A_CP15 };

/* the class of a conditional ARM instruction, in the order recomp/isa.py decode_arm tests them;
 * A_UNDEF ones run whatever their condition says */
static int arm_class(Cpu *c, u32 w) {
    int v5 = c->arch >= 5;
    u32 top = (w >> 25) & 7;
    switch (top) {
    case 0:
        if ((w & 0x0FFFFFF0) == 0x012FFF10) return A_BX;
        if ((w & 0x0FFFFFF0) == 0x012FFF30) return v5 ? A_BLX : A_UNDEF;
        if ((w & 0x0FFF0FF0) == 0x016F0F10) return v5 ? A_CLZ : A_UNDEF;
        if ((w & 0x0F900FF0) == 0x01000050) return v5 ? A_QADD : A_UNDEF;
        if ((w & 0x0FF000F0) == 0x01200070) return A_UNDEF;         /* BKPT */
        if ((w & 0x0FBF0FFF) == 0x010F0000) return A_MRS;
        if ((w & 0x0FB0FFF0) == 0x0120F000) return A_MSR_R;
        if ((w & 0x0F900090) == 0x01000080) return v5 ? A_DSP : A_UNDEF;
        if ((w & 0x0FC000F0) == 0x00000090) return A_MUL;
        if ((w & 0x0F8000F0) == 0x00800090) return A_MULL;
        if ((w & 0x0FB00FF0) == 0x01000090) return A_SWP;
        if ((w & 0x0E000090) == 0x00000090) {
            u32 sh = (w >> 5) & 3, rd = (w >> 12) & 15;
            if (!sh) return A_UNDEF;
            /* LDRD/STRD: ARMv5TE only, an even register below LR */
            if (!((w >> 20) & 1) && sh != 1 && (!v5 || (rd & 1) || rd == 14)) return A_UNDEF;
            return A_HALF;
        }
        if (((w >> 23) & 3) == 2 && !((w >> 20) & 1)) return A_UNDEF;      /* a compare without S */
        return A_DP;
    case 1:
        if ((w & 0x0FB0F000) == 0x0320F000) return A_MSR_I;
        if (((w >> 23) & 3) == 2 && !((w >> 20) & 1)) return A_UNDEF;
        return A_DP;
    case 2: return A_SINGLE;
    case 3: return (w & 0x10) ? A_UNDEF : A_SINGLE;
    case 4: return A_BLOCK;
    case 5: return A_B;
    case 6: return A_UNDEF;
    default:
        if (w & (1u << 24)) return A_SWI;
        if (w & 0x10) return (((w >> 8) & 15) != 15 || !v5) ? A_UNDEF : A_CP15;
        return A_UNDEF;
    }
}

static void arm_step(Cpu *c, u32 pc, u32 w, Step *st) {
    u32 cc = w >> 28;
    st->next = pc + 4;
    if (cc == 15) {
        if (c->arch >= 5 && (w & 0x0E000000) == 0x0A000000) {       /* BLX imm */
            R[14] = pc + 4; c->t = 1;
            st->next = pc + 8 + (u32)(sx(w & 0xFFFFFF, 24) * 4) + ((w >> 24) & 1) * 2;
            st->call = 1;
        } else if (c->arch >= 5 && (w & 0x0D70F000) == 0x0550F000) {    /* PLD: nothing to do */
        } else {
            undef(c, pc, w, st);
        }
        return;
    }
    int cls = arm_class(c, w);
    if (cls == A_UNDEF) { undef(c, pc, w, st); return; }
    if (!cond_ok(c, cc)) return;
    switch (cls) {
    case A_BX: st->next = dsr_bx(c, RG(w & 15, pc + 8)); break;
    case A_BLX: { u32 t = RG(w & 15, pc + 8); R[14] = pc + 4; st->next = dsr_bx(c, t); st->call = 1; break; }
    case A_CLZ: { u32 v = R[w & 15]; R[(w >> 12) & 15] = v ? (u32)__builtin_clz(v) : 32u; break; }
    case A_QADD: arm_qadd(c, w); break;
    case A_MRS: R[(w >> 12) & 15] = (w & (1u << 22)) ? dsr_get_spsr(c) : dsr_cpsr(c); break;
    case A_MSR_R: dsr_msr(c, (int)((w >> 22) & 1), (w >> 16) & 15, R[w & 15]); break;
    case A_MSR_I: dsr_msr(c, (int)((w >> 22) & 1), (w >> 16) & 15, ror32(w & 0xFF, ((w >> 8) & 15) * 2)); break;
    case A_DSP: arm_dsp_mul(c, w); break;
    case A_MUL: arm_mul(c, w); break;
    case A_MULL: arm_mull(c, w); break;
    case A_SWP: {
        u32 rn = (w >> 16) & 15, rd = (w >> 12) & 15, rm = w & 15, a = R[rn], v;
        if (w & (1u << 22)) { v = rd8(c, a); wr8(c, a, R[rm]); }
        else { v = ldr32(c, a); wr32(c, a, R[rm]); }
        R[rd] = v;
        break;
    }
    case A_HALF: arm_halfword(c, pc, w, st); break;
    case A_DP: arm_dataproc(c, pc, w, st); break;
    case A_SINGLE: arm_single(c, pc, w, st); break;
    case A_BLOCK:
        block_transfer(c, st, (w >> 16) & 15, w & 0xFFFF, (int)((w >> 24) & 1), (int)((w >> 23) & 1), (int)((w >> 22) & 1),
                       (int)((w >> 21) & 1), (int)((w >> 20) & 1), pc + 12, 1);
        break;
    case A_B:
        if (w & (1u << 24)) { R[14] = pc + 4; st->call = 1; }
        st->next = pc + 8 + (u32)(sx(w & 0xFFFFFF, 24) * 4);
        break;
    case A_SWI: {
        u32 r_ = dsr_swi(c, (w >> 16) & 0xFF, pc + 4);
        if (r_) st->next = r_, st->leave = 1;
        break;
    }
    case A_CP15: {
        u32 crn = (w >> 16) & 15, rd = (w >> 12) & 15, crm = w & 15, op2 = (w >> 5) & 7;
        if ((w >> 20) & 1) {
            u32 v = dsr_cp15_read(c, crn, crm, op2);
            if (rd == 15) { c->fn = v >> 31; c->fz = (v >> 30) & 1; c->fc = (v >> 29) & 1; c->fv = (v >> 28) & 1; }
            else R[rd] = v;
        } else if (dsr_cp15_write(c, crn, crm, op2, RG(rd, pc + 8))) {
            st->leave = 1;          /* wait for interrupt */
        }
        break;
    }
    }
}

/* ---- Thumb ------------------------------------------------------------------------------ */
static void thumb_alu(Cpu *c, u32 hw) {
    u32 o = (hw >> 6) & 15, rs = (hw >> 3) & 7, rd = hw & 7, co;
    u32 *d = &R[rd], s = R[rs];
    switch (o) {
    case 0: *d &= s; NZ(c, *d); break;
    case 1: *d ^= s; NZ(c, *d); break;
    case 2: *d = LSL_R(*d, s, c->fc, &co); c->fc = co; NZ(c, *d); break;
    case 3: *d = LSR_R(*d, s, c->fc, &co); c->fc = co; NZ(c, *d); break;
    case 4: *d = ASR_R(*d, s, c->fc, &co); c->fc = co; NZ(c, *d); break;
    case 5: *d = ADCS(c, *d, s); break;
    case 6: *d = SBCS(c, *d, s); break;
    case 7: *d = ROR_R(*d, s, c->fc, &co); c->fc = co; NZ(c, *d); break;
    case 8: NZ(c, *d & s); break;
    case 9: *d = SUBS(c, 0, s); break;
    case 10: SUBS(c, *d, s); break;
    case 11: ADDS(c, *d, s); break;
    case 12: *d |= s; NZ(c, *d); break;
    case 13: *d = *d * s; NZ(c, *d); break;
    case 14: *d &= ~s; NZ(c, *d); break;
    default: *d = ~s; NZ(c, *d); break;
    }
}

/* nonzero, with the halfword in *v, when `a` is in memory the CPU reads directly */
static int fetch16(Cpu *c, u32 a, u32 *v) {
    if (a >= 0x10000000u) return 0;
    u8 *p = c->rp[a >> PAGE_SHIFT];
    if (!p) return 0;
    u16 x; memcpy(&x, p + (a & 0xFFF), 2);
    *v = x;
    return 1;
}

static void thumb_step(Cpu *c, u32 pc, u32 hw, Step *st) {
    u32 pcv = pc + 4;
    st->next = pc + 2;
    switch (hw >> 13) {
    case 0:
        if (((hw >> 11) & 3) != 3) {        /* format 1: shift by immediate */
            u32 co = 0; int chg;
            u32 res = shift_imm(c, (hw >> 11) & 3, (hw >> 6) & 31, R[(hw >> 3) & 7], &co, &chg);
            if (chg) c->fc = co;
            NZ(c, res); R[hw & 7] = res;
        } else {                            /* format 2: add/subtract */
            u32 b = ((hw >> 10) & 1) ? (hw >> 6) & 7 : R[(hw >> 6) & 7];
            u32 rs = R[(hw >> 3) & 7];
            R[hw & 7] = ((hw >> 9) & 1) ? SUBS(c, rs, b) : ADDS(c, rs, b);
        }
        return;
    case 1: {                               /* format 3 */
        u32 rd = (hw >> 8) & 7, imm = hw & 0xFF;
        switch ((hw >> 11) & 3) {
        case 0: R[rd] = imm; NZ(c, R[rd]); break;
        case 1: SUBS(c, R[rd], imm); break;
        case 2: R[rd] = ADDS(c, R[rd], imm); break;
        default: R[rd] = SUBS(c, R[rd], imm); break;
        }
        return;
    }
    case 2:
        if ((hw >> 10) == 0x10) { thumb_alu(c, hw); return; }
        if ((hw >> 10) == 0x11) {           /* format 5: hi registers, BX */
            u32 o = (hw >> 8) & 3, h1 = (hw >> 7) & 1, h2 = (hw >> 6) & 1;
            u32 rs = ((hw >> 3) & 7) | (h2 << 3), rd = (hw & 7) | (h1 << 3);
            u32 src = RG(rs, pcv);
            if (o == 3) {
                if (h1) {                   /* BLX reg */
                    if (c->arch < 5) { undef(c, pc, hw, st); return; }
                    R[14] = (pc + 2) | 1; st->next = dsr_bx(c, src); st->call = 1;
                } else if (rs == 15) {      /* bx pc: the ARM code 4 bytes on (an interworking veneer) */
                    c->t = 0; st->next = (pc + 4) & ~3u;
                } else {
                    st->next = dsr_bx(c, src);
                }
                return;
            }
            if (o == 1) { SUBS(c, RG(rd, pcv), src); return; }
            u32 val = o == 0 ? RG(rd, pcv) + src : src;
            if (rd == 15) st->next = val & ~1u;
            else R[rd] = val;
            return;
        }
        if ((hw >> 11) == 9) {              /* format 6: PC-relative load */
            R[(hw >> 8) & 7] = rd32(c, (pcv & ~3u) + (hw & 0xFF) * 4);
            return;
        }
        {                                   /* formats 7 and 8: register offset */
            u32 rd = hw & 7, a = R[(hw >> 3) & 7] + R[(hw >> 6) & 7], o = (hw >> 10) & 3;
            if (hw & (1u << 9)) {
                switch (o) {
                case 0: wr16(c, a, R[rd]); break;
                case 1: R[rd] = (u32)(s32)(s8)rd8(c, a); break;
                case 2: R[rd] = ldrh(c, a); break;
                default: R[rd] = ldrsh(c, a); break;
                }
            } else {
                switch (o) {
                case 0: wr32(c, a, R[rd]); break;
                case 1: wr8(c, a, R[rd]); break;
                case 2: R[rd] = ldr32(c, a); break;
                default: R[rd] = rd8(c, a); break;
                }
            }
        }
        return;
    case 3: {                               /* format 9 */
        u32 l = (hw >> 11) & 1, off = (hw >> 6) & 31, rb = (hw >> 3) & 7, rd = hw & 7;
        if ((hw >> 12) & 1) {
            u32 a = R[rb] + off;
            if (l) R[rd] = rd8(c, a); else wr8(c, a, R[rd]);
        } else {
            u32 a = R[rb] + off * 4;
            if (l) R[rd] = ldr32(c, a); else wr32(c, a, R[rd]);
        }
        return;
    }
    case 4: {
        u32 l = (hw >> 11) & 1;
        if (!(hw & (1u << 12))) {           /* format 10: halfword */
            u32 a = R[(hw >> 3) & 7] + ((hw >> 6) & 31) * 2, rd = hw & 7;
            if (l) R[rd] = ldrh(c, a); else wr16(c, a, R[rd]);
        } else {                            /* format 11: SP-relative */
            u32 rd = (hw >> 8) & 7, a = R[13] + (hw & 0xFF) * 4;
            if (l) R[rd] = ldr32(c, a); else wr32(c, a, R[rd]);
        }
        return;
    }
    case 5:
        if (!(hw & (1u << 12))) {           /* format 12: load address */
            u32 rd = (hw >> 8) & 7, imm = (hw & 0xFF) * 4;
            R[rd] = (hw & (1u << 11)) ? R[13] + imm : (pcv & ~3u) + imm;
            return;
        }
        if ((hw >> 8) == 0xB0) {            /* format 13: add to SP */
            u32 imm = (hw & 0x7F) * 4;
            if (hw & 0x80) R[13] -= imm; else R[13] += imm;
            return;
        }
        if (((hw >> 9) & 3) == 2) {         /* format 14: push/pop */
            u32 l = (hw >> 11) & 1, rl = hw & 0xFF;
            if (hw & 0x100) rl |= l ? 1u << 15 : 1u << 14;
            if (l) block_transfer(c, st, 13, rl, 0, 1, 0, 1, 1, 0, 0);
            else block_transfer(c, st, 13, rl, 1, 0, 0, 1, 0, 0, 0);
            return;
        }
        undef(c, pc, hw, st);               /* BKPT and the unused encodings */
        return;
    case 6:
        if (!(hw & (1u << 12))) {           /* format 15: LDMIA/STMIA */
            block_transfer(c, st, (hw >> 8) & 7, hw & 0xFF, 0, 1, 0, 1, (int)((hw >> 11) & 1), pc + 6, 0);
            return;
        }
        {
            u32 cc = (hw >> 8) & 15;
            if (cc == 15) {                 /* format 17: SWI */
                u32 r_ = dsr_swi(c, hw & 0xFF, pc + 2);
                if (r_) st->next = r_, st->leave = 1;
                return;
            }
            if (cc == 14) { undef(c, pc, hw, st); return; }
            if (cond_ok(c, cc)) st->next = pcv + (u32)(sx(hw & 0xFF, 8) * 2);     /* format 16 */
        }
        return;
    default: {
        u32 o = (hw >> 11) & 3;
        if (o == 0) { st->next = pcv + (u32)(sx(hw & 0x7FF, 11) * 2); return; }     /* format 18: B */
        if (o == 2) {                       /* BL/BLX prefix */
            u32 hi = (u32)sx(hw & 0x7FF, 11) << 12, nx;
            if (fetch16(c, pc + 2, &nx) && ((nx >> 11) == 0x1F || (nx >> 11) == 0x1D) &&
                !(c->arch < 5 && (nx >> 11) == 0x1D)) {
                u32 t = pcv + hi + ((nx & 0x7FF) << 1);
                st->call = 1;
                if ((nx >> 11) == 0x1F) {   /* BL */
                    R[14] = (pc + 4) | 1;
                    st->next = t;
                } else {                    /* BLX */
                    if (nx & 1) { undef(c, pc, hw, st); return; }
                    R[14] = (pc + 4) | 1; c->t = 0;
                    st->next = t & ~3u;
                }
                return;
            }
            R[14] = pcv + hi;
            return;
        }
        /* a BL/BLX suffix on its own: jump to LR + offset */
        u32 lo = (hw & 0x7FF) << 1;
        if (o == 3) {
            u32 t = R[14] + lo;
            R[14] = (pc + 2) | 1;
            st->next = t & ~1u; st->call = 1;
        } else {
            if (c->arch < 5) { undef(c, pc, hw, st); return; }
            u32 t = (R[14] + lo) & ~3u;
            R[14] = (pc + 2) | 1; c->t = 0;
            st->next = t; st->call = 1;
        }
        return;
    }
    }
}

/* ---- the loop --------------------------------------------------------------------------- */
int dsr_interp_exit(Cpu *c, u32 pc);        /* cpu.c: the dispatcher has something to do at pc (2: an interrupt) */

/* Addresses already reported to missing.txt, one set per CPU: an open-addressing table of
 * address | Thumb bit, which grows. */
static u32 *seen_tab[2];
static u32 seen_cap[2], seen_n[2];
#define SEEN_EMPTY 0xFFFFFFFFu

static void seen_add(int k, u32 key) {
    u32 m = seen_cap[k] - 1, i = (key * 2654435761u >> 7) & m;
    while (seen_tab[k][i] != SEEN_EMPTY) i = (i + 1) & m;
    seen_tab[k][i] = key;
}

/* true the first time `key` is seen */
static int first_time(Cpu *c, u32 key) {
    int k = c->arch == 5 ? 0 : 1;
    if (seen_n[k] * 2 >= seen_cap[k]) {
        u32 old_cap = seen_cap[k], *old = seen_tab[k];
        seen_cap[k] = old_cap ? old_cap * 2 : 256;
        seen_tab[k] = malloc(seen_cap[k] * sizeof(u32));
        if (!seen_tab[k]) { seen_tab[k] = old; seen_cap[k] = old_cap; return 0; }
        memset(seen_tab[k], 0xFF, seen_cap[k] * sizeof(u32));
        for (u32 i = 0; i < old_cap; i++) if (old[i] != SEEN_EMPTY) seen_add(k, old[i]);
        free(old);
    }
    u32 m = seen_cap[k] - 1, i = (key * 2654435761u >> 7) & m;
    while (seen_tab[k][i] != SEEN_EMPTY) {
        if (seen_tab[k][i] == key) return 0;
        i = (i + 1) & m;
    }
    seen_tab[k][i] = key;
    seen_n[k]++;
    return 1;
}

static void note(Cpu *c, u32 pc) {
    if (c->log_miss && first_time(c, pc | c->t)) c->log_miss(c, pc, c->t);
}

/* A flow of interpreted code leaves the interpreter for the dispatcher without being done: its
 * time slice ran out, an interrupt is to be taken, it waits for one. The dispatcher brings it
 * back at the address where it stopped, which is not a new entry: neither counted nor logged.
 * Several can be waiting (an interrupted flow, then its handler's own slice ends). */
static void resume_add(Cpu *c, u32 key) {
    if (c->interp_nres == 4) { memmove(c->interp_res, c->interp_res + 1, 3 * sizeof c->interp_res[0]); c->interp_nres = 3; }
    c->interp_res[c->interp_nres++] = key;
}

static int resume_take(Cpu *c, u32 key) {
    for (u32 i = 0; i < c->interp_nres; i++)
        if (c->interp_res[i] == key) { c->interp_res[i] = c->interp_res[--c->interp_nres]; return 1; }
    return 0;
}

u32 dsr_interp(Cpu *c, u32 pc) {
    if (!resume_take(c, pc | c->t)) { c->interp_calls++; note(c, pc); }
    for (;;) {
        u32 w;
        int ok;
        if (c->t) { ok = fetch16(c, pc, &w); }
        else if (pc < 0x10000000u && c->rp[pc >> PAGE_SHIFT]) { memcpy(&w, c->rp[pc >> PAGE_SHIFT] + (pc & 0xFFF), 4); ok = 1; }
        else ok = 0;
        if (!ok) {      /* only RAM the CPU reads directly holds code: a stray jump must not read I/O registers */
            fprintf(stderr, "[ARM%d] no code at %08x (%s): nothing to fetch from there\n", c->arch == 5 ? 9 : 7, pc, c->t ? "Thumb" : "ARM");
            c->stop = 1;
            return pc;
        }
        Step st = {0, 0, 0};
        c->r[15] = pc;      /* what a dispatch to pc leaves there: unpredictable encodings (STM, SWP with Rn = PC) read it */
        c->budget -= 1;
        c->interp_insns++;
        if (c->t) thumb_step(c, pc, w, &st); else arm_step(c, pc, w, &st);
        if (c->stop) return st.next;
        if (st.leave || c->halted) {            /* a wait: the flow goes on after the instruction */
            resume_add(c, (pc + (c->t ? 2u : 4u)) | c->t);
            return st.next;
        }
        if (c->budget <= 0) { resume_add(c, st.next | c->t); return st.next; }
        int x = dsr_interp_exit(c, st.next);
        if (x) {
            if (x == 2) resume_add(c, st.next | c->t);
            return st.next;
        }
        if (st.call) note(c, st.next);      /* a function the discovery missed (the dispatcher logs a compiled one's entry never) */
        pc = st.next;
    }
}
