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
static u32 crc16_update(u32 crc, u32 byte) {
    crc ^= byte & 0xFF;
    for (int k = 0; k < 8; k++) crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
    return crc;
}

/* ---- decompression -------------------------------------------------------------------
 * LZ77, run-length and Huffman decoding are one state machine that asks for its source one
 * unit at a time: from memory (SWIs 11h and 14h run to the end inside the call) or from the
 * callbacks of the SWI's callback structure (the NDS's SWIs 12h, 13h and 15h: GBATEK "NDS/DSi
 * Decompression Callbacks"). A callback is guest code that may wait for hardware, so it runs as
 * guest code and the job is parked in `jobs`: bios_exec resumes it each time a callback returns
 * to BIOS_DEC_RET, as it does for IntrWait. */
#define BIOS_DEC_RET 0x310u     /* where a decompression callback returns to */
enum { REQ_OPEN, REQ_CLOSE, REQ_GET8, REQ_GET16, REQ_GET32, REQ_DONE };     /* the structure's order */
enum { DEC_LZ77 = 1, DEC_HUFF, DEC_RL };
enum { ST_START, ST_HEADER, ST_CLOSE, ST_FLAGS, ST_LZ_LIT, ST_LZ_B0, ST_LZ_B1, ST_RL_FLAG, ST_RL_RUN, ST_RL_LIT,
       ST_H_SIZE, ST_H_TREE, ST_H_WORD };

typedef struct {
    int active, mem, kind, w16; /* parked in a callback; source is memory; algorithm; 16-bit writes (VRAM) */
    u32 cbs, param;             /* callback structure, user parameter (Open's r2) */
    u32 src, addr, req;         /* next source address; address and kind of the pending request */
    u32 dst, size, out;         /* destination, bytes to write, bytes written */
    u32 st, flags, nblk, b0, run, bits;
    u32 tree, tsz, tpos, cur, acc, nacc;    /* Huffman: the tree copy in the guest's buffer, the walk, the word being built */
    u32 pend, result;           /* low byte waiting for its pair (16-bit writes); what the SWI returns */
    u32 ret_pc, ret_t, regs[5]; /* the caller: where to go on, r1-r3, r12, lr */
} BiosJob;

static BiosJob jobs[2];         /* by CPU: [1] is the ARM9 */

static int ask(BiosJob *j, u32 req, u32 unit) { j->req = req; j->addr = j->src; j->src += unit; return (int)req; }
static int dec_close(BiosJob *j) { j->st = ST_CLOSE; return ask(j, REQ_CLOSE, 0); }

/* 16-bit writes (SWIs 12h and 15h, for VRAM) pair bytes up. An LZ copy reads the destination
 * back from memory, so a byte still waiting for its pair reads stale: GBATEK's "disp=000h
 * doesn't work" on the 16-bit variant. */
static void dec_put(Cpu *c, BiosJob *j, u32 v) {
    u32 i = j->out++;
    if (!j->w16) wr8(c, j->dst + i, v);
    else if (i & 1) wr16(c, j->dst + i - 1, j->pend | (v << 8));
    else j->pend = v & 0xFF;
}

/* Take the result of the previous request (v) and return the next one. */
static int dec_step(Cpu *c, BiosJob *j, u32 v) {
    if (j->req == REQ_GET8) v &= 0xFF;
    else if (j->req == REQ_GET16) v &= 0xFFFF;
    switch (j->st) {
    case ST_START:
        j->st = ST_HEADER;
        return ask(j, REQ_OPEN, 0);
    case ST_HEADER:
        if ((s32)v < 0) { j->result = v; return REQ_DONE; }     /* Open refused */
        j->size = v >> 8; j->bits = v & 15; j->src += 4; j->nblk = 0;
        if (!j->size || (j->kind == DEC_HUFF && !j->bits)) return dec_close(j);
        if (j->kind == DEC_LZ77) goto lz_next;
        if (j->kind == DEC_RL) goto rl_next;
        j->st = ST_H_SIZE;
        return ask(j, REQ_GET8, 1);
    case ST_CLOSE:
        if (j->w16 && (j->out & 1)) {       /* an odd last byte: the byte beside it stays */
            u32 a = j->dst + j->out - 1;
            wr16(c, a, (rd16(c, a) & 0xFF00) | j->pend);
        }
        j->result = (s32)v < 0 ? v : j->size;
        return REQ_DONE;
    /* LZ77: flag byte, then eight blocks, each a literal or a (length, displacement) pair */
    lz_next:
        if (j->out >= j->size) return dec_close(j);
        if (!j->nblk) { j->st = ST_FLAGS; return ask(j, REQ_GET8, 1); }
        j->st = (j->flags & 0x80) ? ST_LZ_B0 : ST_LZ_LIT;
        j->flags <<= 1; j->nblk--;
        return ask(j, REQ_GET8, 1);
    case ST_FLAGS: j->flags = v; j->nblk = 8; goto lz_next;
    case ST_LZ_LIT: dec_put(c, j, v); goto lz_next;
    case ST_LZ_B0: j->b0 = v; j->st = ST_LZ_B1; return ask(j, REQ_GET8, 1);
    case ST_LZ_B1: {
        u32 len = (j->b0 >> 4) + 3, disp = (((j->b0 & 15) << 8) | v) + 1;
        for (; len && j->out < j->size; len--) dec_put(c, j, rd8(c, j->dst + j->out - disp));
        goto lz_next;
    }
    /* run-length: flag byte (bit 7: run of N+3, else N+1 literals), then the bytes */
    rl_next:
        if (j->out >= j->size) return dec_close(j);
        j->st = ST_RL_FLAG;
        return ask(j, REQ_GET8, 1);
    case ST_RL_FLAG:
        j->run = (v & 0x7F) + ((v & 0x80) ? 3 : 1);
        if (v & 0x80) { j->st = ST_RL_RUN; return ask(j, REQ_GET8, 1); }
    rl_lit:
        if (!j->run || j->out >= j->size) goto rl_next;
        j->st = ST_RL_LIT;
        return ask(j, REQ_GET8, 1);
    case ST_RL_RUN:
        while (j->run-- && j->out < j->size) dec_put(c, j, v);
        goto rl_next;
    case ST_RL_LIT: dec_put(c, j, v); j->run--; goto rl_lit;
    /* Huffman: the tree is copied into the caller's buffer (the callbacks can't be read twice),
     * then the bit stream, 32 bits a word, walks it from the root and fills words of data units */
    case ST_H_SIZE:
        j->tsz = ((v & 0xFF) + 1) * 2; j->tpos = 1;
        wr8(c, j->tree, v);
    h_tree:
        if (j->tpos < j->tsz) { j->st = ST_H_TREE; return ask(j, REQ_GET8, 1); }
        j->cur = 1; j->acc = j->nacc = 0;
    h_word:
        if (j->out >= j->size) return dec_close(j);
        j->st = ST_H_WORD;
        return ask(j, REQ_GET32, 4);
    case ST_H_TREE: wr8(c, j->tree + j->tpos++, v); goto h_tree;
    case ST_H_WORD:
        for (int n = 0; n < 32; n++, v <<= 1) {
            u32 node = rd8(c, j->tree + j->cur), bit = v >> 31;
            u32 child = ((j->cur & ~1u) + (node & 0x3F) * 2 + 2 + bit) & 0x1FF;
            if (!(node & (bit ? 0x40 : 0x80))) { j->cur = child; continue; }
            j->acc |= (rd8(c, j->tree + child) & ((1u << j->bits) - 1)) << j->nacc;
            j->cur = 1;
            if ((j->nacc += j->bits) >= 32) {
                wr32(c, j->dst + j->out, j->acc);
                j->out += 4; j->acc = j->nacc = 0;
                if (j->out >= j->size) return dec_close(j);
            }
        }
        goto h_word;
    }
    return REQ_DONE;
}

/* Run the job until it needs guest code or is done. Returns the address of the callback to run
 * (c->t set for it; r0.. hold its arguments and lr the way back), or 0 when the job is done. */
static u32 dec_run(Cpu *c, BiosJob *j, u32 v) {
    for (;;) {
        u32 req = (u32)dec_step(c, j, v);
        if (req == REQ_DONE) return 0;
        if (j->mem) {
            v = req == REQ_GET8 ? rd8(c, j->addr) : req == REQ_GET16 ? rd16(c, j->addr) : req == REQ_CLOSE ? 0 : rd32(c, j->addr);
            continue;
        }
        u32 fn = rd32(c, j->cbs + 4 * req);
        if (!fn && req == REQ_CLOSE) { v = 0; continue; }       /* Close is optional */
        if (!fn) { j->result = ~0u; return 0; }
        c->r[0] = j->addr;
        if (req == REQ_OPEN) { c->r[1] = j->dst; c->r[2] = j->param; }
        c->r[14] = c->bios_base + BIOS_DEC_RET + 1;
        return dsr_bx(c, fn);
    }
}

static void dec_return(Cpu *c, BiosJob *j) {
    c->r[0] = j->result;
    c->r[1] = j->regs[0]; c->r[2] = j->regs[1]; c->r[3] = j->regs[2]; c->r[12] = j->regs[3]; c->r[14] = j->regs[4];
}

/* SWI 12h, 13h, 15h: nonzero when a callback has to run first */
static u32 bios_decompress_cb(Cpu *c, u32 num, u32 next) {
    BiosJob *j = &jobs[c->arch == 5];
    memset(j, 0, sizeof *j);
    j->kind = num == 0x12 ? DEC_LZ77 : num == 0x13 ? DEC_HUFF : DEC_RL;
    j->w16 = num != 0x13;           /* Huffman writes whole words */
    j->src = c->r[0]; j->dst = c->r[1]; j->param = j->tree = c->r[2]; j->cbs = c->r[3];
    j->regs[0] = c->r[1]; j->regs[1] = c->r[2]; j->regs[2] = c->r[3]; j->regs[3] = c->r[12]; j->regs[4] = c->r[14];
    j->ret_pc = next; j->ret_t = c->t;
    u32 pc = dec_run(c, j, 0);
    if (pc) { j->active = 1; return pc; }
    dec_return(c, j);
    return 0;
}

/* SWI 11h, 14h: memory to memory */
static void bios_decompress(Cpu *c, int kind) {
    BiosJob j;
    memset(&j, 0, sizeof j);
    j.mem = 1; j.kind = kind; j.src = c->r[0]; j.dst = c->r[1];
    dec_run(c, &j, 0);
}

/* Diff8bitUnFilter and Diff16bitUnFilter (SWI 16h, 18h; ARM9): the units are running sums */
static void bios_unfilter(Cpu *c, u32 src, u32 dst, int wide) {
    u32 size = rd32(c, src) >> 8, sum = 0, unit = wide ? 2 : 1;
    src += 4;
    for (u32 i = 0; i + unit <= size; i += unit) {
        if (wide) { sum = (sum + rd16(c, src + i)) & 0xFFFF; wr16(c, dst + i, sum); }
        else { sum = (sum + rd8(c, src + i)) & 0xFF; wr8(c, dst + i, sum); }
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

/* GetSineTable (ARM7, SWI 1Ah): 64 entries, 7FFFh * sin(i * 90 / 64 degrees) rounded to nearest.
 * That scale reproduces GBATEK's last entry, 7FF5h (8000h would give 7FF6h). The series for sin
 * needs no libm. Outside the 64 GBATEK says "garbage": this returns 0. */
static u32 bios_sine(u32 i) {
    static u16 tab[64];
    static int ready;
    if (!ready) {
        for (int k = 0; k < 64; k++) {
            double x = k * 3.14159265358979323846 / 128, term = x, sum = x;
            for (int n = 1; n < 16; n++) { term *= -x * x / ((2 * n) * (2 * n + 1)); sum += term; }
            tab[k] = (u16)(32767.0 * sum + 0.5);
        }
        ready = 1;
    }
    return i < 64 ? tab[i] : 0;
}

/* SoftReset (GBATEK "BIOS Reset Functions"): clear the 200h bytes holding the stacks and the BIOS
 * interrupt vector and flags, give the three stacks their fixed addresses (the ARM9's are
 * hardcoded for the DTCM's default base, 00800000h: GBATEK's "0803FC0h" is that plus 3FC0h), zero r0-r12 and the svc and irq link and saved status
 * registers, enter system mode and jump through the entry point the cartridge header left in
 * main RAM. The other CPU is untouched. Interrupts stay off until the program enables them. */
static u32 bios_softreset(Cpu *c) {
    int a9 = c->arch == 5;
    u32 zero = a9 ? 0x00803E00u : 0x0380FE00u;
    u32 sp_svc = a9 ? 0x00803FC0u : 0x0380FFDCu, sp_irq = a9 ? 0x00803FA0u : 0x0380FFB0u, sp_sys = a9 ? 0x00803EC0u : 0x0380FF00u;
    u32 entry = rd32(c, a9 ? 0x027FFE24u : 0x027FFE34u);
    for (u32 i = 0; i < 0x200; i += 4) wr32(c, zero + i, 0);
    if (a9) dsr_cp15_write(c, 1, 0, 0, 0x12078);
    dsr_set_cpsr(c, 0xC0 | MODE_SVC, 0xFF);
    c->r[13] = sp_svc; c->r[14] = 0; c->spsr = 0;
    dsr_set_cpsr(c, 0xC0 | MODE_IRQ, 0xFF);
    c->r[13] = sp_irq; c->r[14] = 0; c->spsr = 0;
    dsr_set_cpsr(c, 0xC0 | MODE_SYS, 0xFF);
    for (int i = 0; i < 13; i++) c->r[i] = 0;
    c->r[13] = sp_sys;
    c->r[14] = entry;
    c->halted = 0; c->wait_mask = 0;
    memset(&jobs[a9], 0, sizeof jobs[a9]);
    return dsr_bx(c, entry);
}

/* The ARM7's CpuSet and CpuFastSet quietly do nothing when the source start or end is in the BIOS */
static int bios_src_protected(Cpu *c, u32 src, u32 bytes) {
    return c->arch == 4 && (src < 0x4000 || (u32)(src + bytes - 1) < 0x4000);
}

#define BIOS_INTRWAIT 0x300u

u32 dsr_swi(Cpu *c, u32 num, u32 next) {
    c->budget -= 8;
    switch (num) {
    case 0x00:  /* SoftReset */
        return bios_softreset(c);
    case 0x03: { /* WaitByLoop: SUB r0,1 / BGT, 4 cycles an iteration; r0 <= 0 still runs once */
        u32 n = (s32)c->r[0] > 0 ? c->r[0] : 1;
        c->budget -= n < 0x1FFFFFFFu ? (s32)(n * 4) : 0x7FFFFFFC;
        c->r[0] = (s32)c->r[0] > 0 ? 0 : c->r[0] - 1;
        return 0;
    }
    case 0x04:  /* IntrWait */
    case 0x05:  /* VBlankIntrWait */
        if (num == 5) { c->r[0] = 1; c->r[1] = 1; }
        if (c->r[0] & 1) wr32(c, c->irq_check_addr, rd32(c, c->irq_check_addr) & ~c->r[1]);
        c->wait_mask = c->r[1];
        c->wait_ret = next; c->wait_ret_t = c->t;
        c->write_slow(c, 0x04000208, 1, 4);     /* the BIOS enables IME while it waits */
        c->t = 0;
        return c->bios_base + BIOS_INTRWAIT;    /* the wait loop runs as BIOS code (bios_exec) */
    case 0x06:  /* Halt (the ARM9's goes through CP15 with r0 = 0) */
        if (c->arch == 5) c->r[0] = 0;
        c->halted = 1;
        return next;
    case 0x07:  /* Sleep (ARM7) */
        if (c->arch == 5) goto unimplemented;
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
        if (bios_src_protected(c, src, fill ? (word ? 4 : 2) : n * (word ? 4 : 2))) return 0;
        if (word) { u32 v = rd32(c, src); for (u32 i = 0; i < n; i++) { if (!fill) v = rd32(c, src + 4 * i); wr32(c, dst + 4 * i, v); } }
        else { u32 v = rd16(c, src); for (u32 i = 0; i < n; i++) { if (!fill) v = rd16(c, src + 2 * i); wr16(c, dst + 2 * i, v); } }
        c->budget -= (s32)n;
        return 0;
    }
    case 0x0C: { /* CpuFastSet */
        u32 src = c->r[0], dst = c->r[1], ctl = c->r[2], n = ctl & 0x1FFFFF;     /* any word count, unlike the GBA's multiples of 8 */
        int fill = (ctl >> 24) & 1;
        if (bios_src_protected(c, src, fill ? 4 : n * 4)) return 0;
        u32 v = rd32(c, src);
        for (u32 i = 0; i < n; i++) { if (!fill) v = rd32(c, src + 4 * i); wr32(c, dst + 4 * i, v); }
        c->budget -= (s32)n;
        return 0;
    }
    case 0x0D:  /* Sqrt */
        c->r[0] = isqrt(c->r[0]);
        return 0;
    case 0x0E: { /* GetCRC16 */
        u32 crc = c->r[0] & 0xFFFF, a = c->r[1], n = c->r[2];
        for (u32 i = 0; i < n; i += 2) {        /* by halfwords: r3 ends as the last one */
            u32 h = rd16(c, a + i);
            crc = crc16_update(crc16_update(crc, h), h >> 8);
            c->r[3] = h;
        }
        c->r[0] = crc;
        c->budget -= (s32)n;
        return 0;
    }
    case 0x0F:  /* IsDebugger */
        c->r[0] = 0;
        return 0;
    case 0x10:
        bios_bitunpack(c, c->r[0], c->r[1], c->r[2]);
        return 0;
    case 0x11:  /* LZ77UnCompReadNormalWrite8bit */
        bios_decompress(c, DEC_LZ77);
        return 0;
    case 0x14:  /* RLUnCompReadNormalWrite8bit */
        bios_decompress(c, DEC_RL);
        return 0;
    case 0x12:  /* LZ77UnCompReadByCallbackWrite16bit */
    case 0x13:  /* HuffUnCompReadByCallback */
    case 0x15:  /* RLUnCompReadByCallbackWrite16bit */
        return bios_decompress_cb(c, num, next);
    case 0x16:  /* Diff8bitUnFilterWrite8bit (ARM9) */
    case 0x18:  /* Diff16bitUnFilter (ARM9) */
        if (c->arch == 4) goto unimplemented;
        bios_unfilter(c, c->r[0], c->r[1], num == 0x18);
        return 0;
    case 0x1A:  /* GetSineTable (ARM7) */
        if (c->arch == 5) goto unimplemented;
        c->r[0] = bios_sine(c->r[0]);
        return 0;
    case 0x1F:
        if (c->arch == 5) {     /* CustomPost: POSTFLG */
            c->write_slow(c, 0x04000300, c->r[0], 4);
            return 0;
        }
        c->write_slow(c, 0x04000301, c->r[2] & 0xFF, 1);        /* CustomHalt: HALTCNT, 80h halt, C0h sleep */
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
    if (off == BIOS_DEC_RET) {  /* a decompression callback returned its result in r0 */
        BiosJob *j = &jobs[c->arch == 5];
        if (j->active) {
            u32 pc = dec_run(c, j, c->r[0]);
            if (pc) { c->r[15] = pc; c->budget -= 6; return 1; }
            j->active = 0;
            dec_return(c, j);
            c->r[15] = j->ret_pc; c->t = j->ret_t;
            return 1;
        }
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
    memset(&jobs[arch == 5], 0, sizeof jobs[0]);
}
