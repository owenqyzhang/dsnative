/* dsnative runtime: CPU state, the dispatcher, exceptions, the BIOS (high-level) and CP15. */
#include <stdio.h>
#include <stdlib.h>
#include "dsr.h"

/* ---- modes and banked registers ----------------------------------------- */
static int bank_of(u32 mode) {
    switch (mode) {
    case MODE_FIQ: return 1;
    case MODE_IRQ: return 2;
    case MODE_SVC: return 3;
    case MODE_ABT: return 4;
    case MODE_UND: return 5;
    default: return 0;      /* usr, sys */
    }
}

static void set_mode(Cpu *c, u32 mode) {
    u32 old = c->mode;
    if (old == mode) return;
    int ob = bank_of(old), nb = bank_of(mode);
    if (ob != nb) {
        c->bank_r13[ob] = c->r[13];
        c->bank_r14[ob] = c->r[14];
        c->bank_spsr[ob] = c->spsr;
        if (old == MODE_FIQ) {
            memcpy(c->fiq_r8_12, &c->r[8], 20);
            memcpy(&c->r[8], c->usr_r8_12, 20);
        }
        if (mode == MODE_FIQ) {
            memcpy(c->usr_r8_12, &c->r[8], 20);
            memcpy(&c->r[8], c->fiq_r8_12, 20);
        }
        c->r[13] = c->bank_r13[nb];
        c->r[14] = c->bank_r14[nb];
        c->spsr = c->bank_spsr[nb];
    }
    c->mode = mode;
}

u32 dsr_cpsr(Cpu *c) {
    return (c->fn << 31) | (c->fz << 30) | (c->fc << 29) | (c->fv << 28) | (c->fq << 27) |
           (c->irq_off << 7) | (c->fiq_off << 6) | (c->t << 5) | c->mode;
}

void dsr_set_cpsr(Cpu *c, u32 v, u32 mask) {
    if (mask & 0xFF000000u) {
        c->fn = v >> 31; c->fz = (v >> 30) & 1; c->fc = (v >> 29) & 1; c->fv = (v >> 28) & 1;
        if (c->arch >= 5) c->fq = (v >> 27) & 1;
    }
    if (mask & 0xFFu) {
        c->irq_off = (v >> 7) & 1;
        c->fiq_off = (v >> 6) & 1;
        if (mask & 0x20u) c->t = (v >> 5) & 1;
        set_mode(c, (v & 0x1F) | 0x10);
    }
}

static int has_spsr(Cpu *c) { return c->mode != MODE_USR && c->mode != MODE_SYS; }

u32 dsr_get_spsr(Cpu *c) { return has_spsr(c) ? c->spsr : dsr_cpsr(c); }

void dsr_set_spsr(Cpu *c, u32 v, u32 mask) {
    if (has_spsr(c)) c->spsr = (c->spsr & ~mask) | (v & mask);
}

void dsr_msr(Cpu *c, int spsr, u32 fm, u32 v) {
    u32 mask = 0;
    if (fm & 8) mask |= 0xFF000000u;
    if (fm & 4) mask |= 0x00FF0000u;
    if (fm & 2) mask |= 0x0000FF00u;
    if (fm & 1) mask |= 0x000000FFu;
    if (spsr) { dsr_set_spsr(c, v, mask); return; }
    if (c->mode == MODE_USR) mask &= 0xFF000000u;
    dsr_set_cpsr(c, v, mask & ~0x20u);      /* MSR does not change the state bit */
}

u32 dsr_exception_return(Cpu *c, u32 target) {
    if (has_spsr(c)) dsr_set_cpsr(c, c->spsr, 0xFFFFFFFFu);
    return c->t ? (target & ~1u) : (target & ~3u);
}

u32 dsr_get_user_reg(Cpu *c, int n) {
    if (n >= 13 && n <= 14 && has_spsr(c)) return n == 13 ? c->bank_r13[0] : c->bank_r14[0];
    if (n >= 8 && n <= 12 && c->mode == MODE_FIQ) return c->usr_r8_12[n - 8];
    return c->r[n];
}

void dsr_set_user_reg(Cpu *c, int n, u32 v) {
    if (n >= 13 && n <= 14 && has_spsr(c)) { if (n == 13) c->bank_r13[0] = v; else c->bank_r14[0] = v; return; }
    if (n >= 8 && n <= 12 && c->mode == MODE_FIQ) { c->usr_r8_12[n - 8] = v; return; }
    c->r[n] = v;
}

u32 dsr_undefined(Cpu *c, u32 pc, u32 word) {
    fprintf(stderr, "[ARM%d] undefined instruction %08x at %08x\n", c->arch == 5 ? 9 : 7, word, pc);
    c->stop = 1;
    return pc;
}

u32 dsr_bad_entry(Cpu *c, u32 pc, u32 fn) {
    fprintf(stderr, "[ARM%d] dispatch to %08x entered function %08x without that label\n", c->arch == 5 ? 9 : 7, pc, fn);
    c->stop = 1;
    return pc;
}

/* ---- CP15 (ARM9 only) --------------------------------------------------- */
u32 dsr_cp15_read(Cpu *c, u32 crn, u32 crm, u32 op2) {
    if (crn == 0 && crm == 0) {
        if (op2 == 1) return 0x0F0D2112u;   /* cache type */
        if (op2 == 2) return 0x00140180u;   /* TCM size */
        return 0x41059461u;                 /* ARM946E-S */
    }
    if (crn == 1 && crm == 0) return c->cp15_ctrl;
    if (crn == 9 && crm == 1) return op2 == 0 ? c->cp15_dtcm : c->cp15_itcm;
    return 0;
}

int dsr_cp15_write(Cpu *c, u32 crn, u32 crm, u32 op2, u32 v) {
    if (crn == 1 && crm == 0) { c->cp15_ctrl = v | 0x78; c->bios_base = (v & (1u << 13)) ? 0xFFFF0000u : 0; if (c->remap) c->remap(c); return 0; }
    if (crn == 9 && crm == 1) {
        if (op2 == 0) c->cp15_dtcm = v; else c->cp15_itcm = v;
        c->irq_check_addr = (c->cp15_dtcm & 0xFFFFF000u) + 0x3FF8;
        if (c->remap) c->remap(c);
        return 0;
    }
    if ((crn == 7 && crm == 0 && op2 == 4) || (crn == 7 && crm == 8 && op2 == 2)) {
        c->halted = 1;      /* wait for interrupt */
        return 1;
    }
    return 0;               /* caches, protection unit: nothing to do */
}

/* ---- BIOS (high-level) ---------------------------------------------------- */
static u16 crc16_update(u16 crc, const u8 *p, u32 n) {
    for (u32 i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
    }
    return crc;
}

static void bios_lz77(Cpu *c, u32 src, u32 dst) {
    u32 hdr = rd32(c, src); src += 4;
    u32 size = hdr >> 8;
    u32 out = 0;
    while (out < size) {
        u32 flags = rd8(c, src++);
        for (int i = 0; i < 8 && out < size; i++, flags <<= 1) {
            if (flags & 0x80) {
                u32 b0 = rd8(c, src++), b1 = rd8(c, src++);
                u32 len = (b0 >> 4) + 3, disp = (((b0 & 0xF) << 8) | b1) + 1;
                for (u32 k = 0; k < len && out < size; k++, out++) wr8(c, dst + out, rd8(c, dst + out - disp));
            } else {
                wr8(c, dst + out, rd8(c, src++)); out++;
            }
        }
    }
}

static void bios_rl(Cpu *c, u32 src, u32 dst) {
    u32 hdr = rd32(c, src); src += 4;
    u32 size = hdr >> 8, out = 0;
    while (out < size) {
        u32 f = rd8(c, src++);
        if (f & 0x80) { u32 n = (f & 0x7F) + 3, b = rd8(c, src++); while (n-- && out < size) wr8(c, dst + out++, b); }
        else { u32 n = (f & 0x7F) + 1; while (n-- && out < size) wr8(c, dst + out++, rd8(c, src++)); }
    }
}

static void bios_bitunpack(Cpu *c, u32 src, u32 dst, u32 info) {
    u32 len = rd16(c, info), sw = rd8(c, info + 2), dw = rd8(c, info + 3), off = rd32(c, info + 4);
    u32 zero = off >> 31; off &= 0x7FFFFFFF;
    u32 acc = 0, accbits = 0;
    for (u32 i = 0; i < len; i++) {
        u32 b = rd8(c, src + i);
        for (u32 k = 0; k < 8; k += sw) {
            u32 v = (b >> k) & ((1u << sw) - 1);
            if (v || zero) v += off;
            acc |= v << accbits; accbits += dw;
            if (accbits >= 32) { wr32(c, dst, acc); dst += 4; acc = 0; accbits = 0; }
        }
    }
}

static u32 isqrt(u32 v) {
    u32 r = 0, bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit) { if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; } else r >>= 1; bit >>= 2; }
    return r;
}

/* exp(x) without libm (the generated build and tests/difftest.py link cpu.c alone): halve x
 * into the series' range, sum the Taylor series and square back. */
static double bios_exp(double x) {
    int n = 0;
    while (x > 0.5 || x < -0.5) { x /= 2; n++; }
    double term = 1, sum = 1;
    for (int k = 1; k < 24; k++) { term *= x / k; sum += term; }
    while (n--) sum *= sum;
    return sum;
}

/* The ARM7 BIOS's sound tables, which the SDK's sound driver reads with SWI 1Bh and 1Ch
 * (GBATEK "DS BIOS Sound Functions"). Generated from what they hold, not stored:
 *  - pitch, 768 entries (12 semitones of 64 steps): 2^(i/768) as 16.16 fixed point, less the
 *    1.0, rounded to nearest (the last entry is FF8Ah, as GBATEK says; rounding down gives FF89h);
 *  - volume, 724 entries: i - 723 is a level in tenths of a decibel (the SDK's -723 to 0), the
 *    amplitude 127 * 10^(dB / 200) rounded to nearest, scaled up by the divider the driver
 *    then sets in SOUNDxCNT, which it picks from the same level: 16 (the hardware's Div16)
 *    below -240, 4 below -120, 2 below -60, else 1. Each range so ramps from 32 or 64 to 127
 *    and the channel's volume divider scales it back.
 * Outside the tables GBATEK says "garbage" (what follows them in the ROM): this returns 0. */
#define BIOS_PITCH_LEN 768
#define BIOS_VOLUME_LEN 724
static u32 bios_sound_table(int volume, u32 i) {
    static u16 pitch[BIOS_PITCH_LEN];
    static u8 vol[BIOS_VOLUME_LEN];
    static int ready;
    const double ln2 = 0.6931471805599453, ln10 = 2.302585092994046;
    if (!ready) {
        for (int k = 0; k < BIOS_PITCH_LEN; k++) pitch[k] = (u16)((u32)(65536.0 * bios_exp(k * ln2 / BIOS_PITCH_LEN) + 0.5) - 65536u);
        for (int k = 0; k < BIOS_VOLUME_LEN; k++) {
            int db = k - 723;
            double div = db < -240 ? 16 : db < -120 ? 4 : db < -60 ? 2 : 1;
            vol[k] = (u8)(127.0 * div * bios_exp(db * ln10 / 200.0) + 0.5);
        }
        ready = 1;
    }
    if (volume) return i < BIOS_VOLUME_LEN ? vol[i] : 0;
    return i < BIOS_PITCH_LEN ? pitch[i] : 0;
}

#define BIOS_INTRWAIT 0x300u

u32 dsr_swi(Cpu *c, u32 num, u32 next) {
    c->budget -= 8;
    switch (num) {
    case 0x03:  /* WaitByLoop */
        c->budget -= (s32)(c->r[0] * 4);
        return 0;
    case 0x04:  /* IntrWait */
    case 0x05:  /* VBlankIntrWait */
        if (num == 5) { c->r[0] = 1; c->r[1] = 1; }
        if (c->r[0] & 1) wr32(c, c->irq_check_addr, rd32(c, c->irq_check_addr) & ~c->r[1]);
        c->wait_mask = c->r[1];
        c->wait_ret = next; c->wait_ret_t = c->t;
        c->write_slow(c, 0x04000208, 1, 4);     /* the BIOS enables IME while it waits */
        c->t = 0;
        return c->bios_base + BIOS_INTRWAIT;    /* the wait loop runs as BIOS code (bios_exec) */
    case 0x06:  /* Halt */
        c->halted = 1;
        return next;
    case 0x07:  /* Sleep (ARM7) */
        c->halted = 1;
        return next;
    case 0x08: { /* SoundBias (ARM7): SOUNDBIAS to 0 (r0 = 0) or 200h in steps of 1, r1 delay loops per step */
        if (c->arch == 5) goto unimplemented;
        u32 cur = rd16(c, 0x04000504), tgt = c->r[0] ? 0x200 : 0, lo = cur & 0x3FF;
        c->write_slow(c, 0x04000504, (cur & ~0x3FFu) | tgt, 2);     /* the upper bits stay */
        c->budget -= (s32)((lo > tgt ? lo - tgt : tgt - lo) * (c->r[1] * 4 + 12));
        return 0;
    }
    case 0x1B:  /* GetPitchTable (ARM7) */
    case 0x1C:  /* GetVolumeTable (ARM7) */
        if (c->arch == 5) goto unimplemented;
        c->r[0] = bios_sound_table(num == 0x1C, c->r[0]);
        return 0;
    case 0x09: { /* Div */
        s32 n = (s32)c->r[0], d = (s32)c->r[1];
        if (d == 0) { c->r[0] = n < 0 ? (u32)-1 : 1; c->r[1] = (u32)n; c->r[3] = 1; return 0; }
        if (n == (s32)0x80000000 && d == -1) { c->r[0] = 0x80000000u; c->r[1] = 0; c->r[3] = 0x80000000u; return 0; }
        s32 q = n / d;
        c->r[0] = (u32)q; c->r[1] = (u32)(n % d); c->r[3] = (u32)(q < 0 ? -q : q);
        return 0;
    }
    case 0x0B: { /* CpuSet */
        u32 src = c->r[0], dst = c->r[1], ctl = c->r[2], n = ctl & 0x1FFFFF;
        int fill = (ctl >> 24) & 1, word = (ctl >> 26) & 1;
        if (word) { u32 v = rd32(c, src); for (u32 i = 0; i < n; i++) { if (!fill) v = rd32(c, src + 4 * i); wr32(c, dst + 4 * i, v); } }
        else { u32 v = rd16(c, src); for (u32 i = 0; i < n; i++) { if (!fill) v = rd16(c, src + 2 * i); wr16(c, dst + 2 * i, v); } }
        c->budget -= (s32)n;
        return 0;
    }
    case 0x0C: { /* CpuFastSet */
        u32 src = c->r[0], dst = c->r[1], ctl = c->r[2], n = ((ctl & 0x1FFFFF) + 7) & ~7u;
        int fill = (ctl >> 24) & 1;
        u32 v = rd32(c, src);
        for (u32 i = 0; i < n; i++) { if (!fill) v = rd32(c, src + 4 * i); wr32(c, dst + 4 * i, v); }
        c->budget -= (s32)n;
        return 0;
    }
    case 0x0D:  /* Sqrt */
        c->r[0] = isqrt(c->r[0]);
        return 0;
    case 0x0E: { /* GetCRC16 */
        u16 crc = (u16)c->r[0];
        u32 a = c->r[1], n = c->r[2];
        for (u32 i = 0; i < n; i++) { u8 b = (u8)rd8(c, a + i); crc = crc16_update(crc, &b, 1); }
        c->r[0] = crc;
        return 0;
    }
    case 0x0F:  /* IsDebugger */
        c->r[0] = 0;
        return 0;
    case 0x10:
        bios_bitunpack(c, c->r[0], c->r[1], c->r[2]);
        return 0;
    case 0x11:
        bios_lz77(c, c->r[0], c->r[1]);
        return 0;
    case 0x14:
        bios_rl(c, c->r[0], c->r[1]);
        return 0;
    case 0x1F:  /* CustomHalt (ARM7) */
        c->halted = 1;
        return next;
    default:
    unimplemented:
        fprintf(stderr, "[ARM%d] BIOS call %02x is not implemented (r0=%08x r1=%08x r2=%08x)\n", c->arch == 5 ? 9 : 7, num, c->r[0], c->r[1], c->r[2]);
        return 0;
    }
}

/* ---- the dispatcher --------------------------------------------------------- */
#define CACHE_BITS 16
typedef struct { u32 key; const DispatchEntry *e; } CacheLine;
static CacheLine cache9[1 << CACHE_BITS], cache7[1 << CACHE_BITS];

void dsr_flush_cache(Cpu *c) {
    memset(c->arch == 5 ? cache9 : cache7, 0, sizeof(cache9));
}

static int entry_matches(Cpu *c, const DispatchEntry *e, u32 pc) {
    for (u32 i = 0; i < e->check_len; i++)
        if (rd8(c, pc + i) != e->check[i]) return 0;
    return 1;
}

static const DispatchEntry *lookup(Cpu *c, u32 key) {
    CacheLine *cl = &(c->arch == 5 ? cache9 : cache7)[(key >> 1) & ((1 << CACHE_BITS) - 1)];
    u32 pc = key & ~1u;
    if (cl->e && cl->key == key && (!cl->e->check_len || entry_matches(c, cl->e, pc))) return cl->e;
    u32 lo = 0, hi = c->table_len;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (c->table[mid].key < key) lo = mid + 1; else hi = mid;
    }
    for (u32 i = lo; i < c->table_len && c->table[i].key == key; i++) {
        const DispatchEntry *e = &c->table[i];
        if (!e->check_len || entry_matches(c, e, pc)) { cl->key = key; cl->e = e; return e; }
    }
    return 0;
}

/* Code with no address of its own (see PicEntry): the stub whose bytes are in memory at pc.
 * Only RAM the CPU reads directly is looked at: a stray jump must not read I/O registers. */
static const PicEntry *lookup_pic(Cpu *c, u32 pc) {
    if (c->t || (pc & 3)) return 0;
    for (u32 i = 0; i < c->pic_len; i++) {
        const PicEntry *p = &c->pic[i];
        u32 n = 0;
        for (; n < p->len; n += 4) {
            u32 a = pc + n, v, w;
            const u8 *page = a >= pc && a < 0x10000000u ? c->rp[a >> PAGE_SHIFT] : 0;
            if (!page) break;
            memcpy(&v, page + (a & 0xFFF), 4);
            memcpy(&w, p->bytes + n, 4);
            if (v != w) break;
        }
        if (n == p->len) return p;
    }
    return 0;
}

static void enter_irq(Cpu *c) {
    u32 cpsr = dsr_cpsr(c);
    u32 ret = c->r[15];
    set_mode(c, MODE_IRQ);
    c->spsr = cpsr;
    c->r[14] = ret + 4;
    c->irq_off = 1;
    c->t = 0;
    c->r[15] = c->bios_base + 0x18;
}

/* BIOS code, run in C: the interrupt handler and its return */
#define BIOS_IRQ_RET 0x200u
static int irq_pending(Cpu *c) { return !c->irq_off && c->irq_line && c->irq_line(c); }

static int bios_exec(Cpu *c) {
    u32 off = c->r[15] - c->bios_base;
    if (off == BIOS_INTRWAIT) {     /* IntrWait: until the handler has set one of the flags */
        u32 f = rd32(c, c->irq_check_addr);
        if (f & c->wait_mask) {
            wr32(c, c->irq_check_addr, f & ~c->wait_mask);
            c->r[15] = c->wait_ret; c->t = c->wait_ret_t;
            return 1;
        }
        if (irq_pending(c)) return 1;       /* taken at the top of the loop; it returns here */
        c->halted = 1;
        c->budget = 0;
        return 0;
    }
    if (off == 0x18) {          /* stmfd sp!, {r0-r3, r12, lr}; ldr pc, [handler] */
        u32 sp = c->r[13] - 24;
        u32 regs[6] = {c->r[0], c->r[1], c->r[2], c->r[3], c->r[12], c->r[14]};
        for (int i = 0; i < 6; i++) wr32(c, sp + 4 * i, regs[i]);
        c->r[13] = sp;
        c->r[14] = c->bios_base + BIOS_IRQ_RET;
        u32 handler = rd32(c, c->irq_check_addr + 4);
        c->r[15] = dsr_ldpc(c, handler);
        c->budget -= 10;
        return 1;
    }
    if (off == BIOS_IRQ_RET) {  /* ldmfd sp!, {r0-r3, r12, lr}; subs pc, lr, #4 */
        u32 sp = c->r[13];
        c->r[0] = rd32(c, sp); c->r[1] = rd32(c, sp + 4); c->r[2] = rd32(c, sp + 8);
        c->r[3] = rd32(c, sp + 12); c->r[12] = rd32(c, sp + 16); c->r[14] = rd32(c, sp + 20);
        c->r[13] = sp + 24;
        c->r[15] = dsr_exception_return(c, c->r[14] - 4);
        c->budget -= 10;
        return 1;
    }
    fprintf(stderr, "[ARM%d] jump into the BIOS at %08x\n", c->arch == 5 ? 9 : 7, c->r[15]);
    c->stop = 1;
    return 0;
}

static int in_bios(Cpu *c, u32 pc) {
    if (c->bios_base) return pc >= c->bios_base;
    return pc < 0x4000;
}

/* Run for `cycles` cycles (or until stopped). Returns the cycles run. */
s32 dsr_run(Cpu *c, s32 cycles) {
    c->budget += cycles;
    s32 start = c->budget;
    while (c->budget > 0 && !c->stop) {
        if (c->halted) {
            if (c->irq_wake && c->irq_wake(c)) c->halted = 0;
            else { c->budget = 0; break; }
        }
        if (irq_pending(c)) enter_irq(c);
        u32 pc = c->r[15];
        if (pc == c->exit_pc) { c->stop = 1; break; }
        if (in_bios(c, pc)) { if (!bios_exec(c) && !c->halted) break; continue; }
        const DispatchEntry *e = lookup(c, pc | c->t);
        if (!e) {
            const PicEntry *p = lookup_pic(c, pc);
            if (p) {
                c->dispatches++;
                c->r[15] = p->fn(c, pc);
                continue;
            }
            c->misses++;
            c->last_miss = pc | c->t;
            if (c->log_miss) c->log_miss(c, pc, c->t);
            fprintf(stderr, "[ARM%d] no recompiled code at %08x (%s)\n", c->arch == 5 ? 9 : 7, pc, c->t ? "Thumb" : "ARM");
            c->stop = 1;
            break;
        }
        c->dispatches++;
        c->r[15] = e->fn(c, pc);
    }
    s32 ran = start - c->budget;
    c->cycles += (u64)ran;
    return ran;
}

void dsr_reset(Cpu *c, int arch, u32 entry) {
    memset(c->r, 0, sizeof c->r);
    c->arch = (u32)arch;
    c->mode = MODE_SVC;
    c->irq_off = 1; c->fiq_off = 1;
    c->t = entry & 1;
    c->r[15] = entry & ~1u;
    c->cp15_ctrl = 0x2078;
    c->bios_base = arch == 5 ? 0xFFFF0000u : 0;
    c->cp15_dtcm = 0x0300000Au;     /* what the firmware leaves: DTCM at 0x03000000? (games set their own) */
    c->irq_check_addr = arch == 5 ? 0x03003FF8u : 0x0380FFF8u;
    c->exit_pc = 0xFFFFFFFFu;
}
