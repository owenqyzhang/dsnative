/* dsnative runtime: the DS machine. References: GBATEK (DS Memory Maps, I/O ports, DMA,
 * timers, IPC, cartridge protocol, SPI devices) and melonDS's direct boot for what the
 * firmware leaves in memory. */
#include <stdio.h>
#include <stdlib.h>
#include "nds.h"

s32 dsr_run(Cpu *c, s32 cycles);
void dsr_reset(Cpu *c, int arch, u32 entry);
void dsr_flush_cache(Cpu *c);

#define CPU_OF(c) ((c)->arch == 5 ? 0 : 1)
#define SYS(c) ((Nds *)(c)->sys)

/* ---- VRAM banks ------------------------------------------------------------ */
static const u32 bank_off[9] = {0x00000, 0x20000, 0x40000, 0x60000, 0x80000, 0x90000, 0x94000, 0x98000, 0xA0000};
static const u32 bank_size[9] = {0x20000, 0x20000, 0x20000, 0x20000, 0x10000, 0x4000, 0x4000, 0x8000, 0x4000};

enum { R_NONE, R_ABG, R_BBG, R_AOBJ, R_BOBJ, R_LCDC, R_ARM7, R_TEX, R_TEXPAL, R_ABGEXT, R_AOBJEXT, R_BBGEXT, R_BOBJEXT };

/* where bank b is mapped: region and offset within it */
static int bank_region(Nds *s, int b, u32 *off) {
    u8 cnt = s->vramcnt[b];
    if (!(cnt & 0x80)) return R_NONE;
    u32 mst = cnt & 7, ofs = (cnt >> 3) & 3;
    *off = 0;
    switch (b) {
    case 0: case 1:
        mst &= 3;
        if (mst == 0) return R_LCDC;
        if (mst == 1) { *off = 0x20000 * ofs; return R_ABG; }
        if (mst == 2) { *off = 0x20000 * (ofs & 1); return R_AOBJ; }
        *off = 0x20000 * ofs; return R_TEX;
    case 2: case 3:
        if (mst == 0) return R_LCDC;
        if (mst == 1) { *off = 0x20000 * ofs; return R_ABG; }
        if (mst == 2) { *off = 0x20000 * (ofs & 1); return R_ARM7; }
        if (mst == 3) { *off = 0x20000 * ofs; return R_TEX; }
        if (mst == 4) return b == 2 ? R_BBG : R_BOBJ;
        return R_NONE;
    case 4:
        if (mst == 0) return R_LCDC;
        if (mst == 1) return R_ABG;
        if (mst == 2) return R_AOBJ;
        if (mst == 3) return R_TEXPAL;
        if (mst == 4) return R_ABGEXT;
        return R_NONE;
    case 5: case 6:
        if (mst == 0) return R_LCDC;
        *off = 0x4000 * (ofs & 1) + 0x10000 * (ofs >> 1);
        if (mst == 1) return R_ABG;
        if (mst == 2) return R_AOBJ;
        if (mst == 3) { *off = 0x4000 * ((ofs & 1) + 4 * (ofs >> 1)); return R_TEXPAL; }
        if (mst == 4) { *off = 0x4000 * (ofs & 1); return R_ABGEXT; }
        if (mst == 5) { *off = 0; return R_AOBJEXT; }
        return R_NONE;
    case 7:
        mst &= 3;
        if (mst == 0) return R_LCDC;
        if (mst == 1) return R_BBG;
        if (mst == 2) return R_BBGEXT;
        return R_NONE;
    case 8:
        mst &= 3;
        if (mst == 0) return R_LCDC;
        if (mst == 1) { *off = 0x8000; return R_BBG; }
        if (mst == 2) return R_BOBJ;
        if (mst == 3) return R_BOBJEXT;
        return R_NONE;
    }
    return R_NONE;
}

/* engine-relative lookup tables, 16 KB blocks, rebuilt by nds_remap */
static u8 *abg[32], *bbg[8], *aobj[16], *bobj[8];
u8 *vram_texslot[4], *vram_texpal[8], *vram_extpal[4][4];    /* ext: [region][slot 8 KB] */

u8 *vram_engine_ptr(Nds *s, int engine, int obj, u32 offset) {
    (void)s;
    u8 **t; u32 n;
    if (engine == 0) { t = obj ? aobj : abg; n = obj ? 16 : 32; }
    else { t = obj ? bobj : bbg; n = 8; }
    u32 blk = (offset >> 14) % n;
    return t[blk] ? t[blk] + (offset & 0x3FFF) : 0;
}

static void map_pages(Cpu *c, u32 addr, u32 size, u8 *mem, int rw, int rw8) {
    for (u32 o = 0; o < size; o += 0x1000) {
        u32 p = (addr + o) >> PAGE_SHIFT;
        if (p >= PAGE_COUNT) break;
        c->rp[p] = mem + o;
        c->wp[p] = rw ? mem + o : 0;
        c->wp8[p] = rw8 ? mem + o : 0;
    }
}

static void map_mirror(Cpu *c, u32 start, u32 end, u32 period, u32 off, u8 *mem, u32 size, int rw8) {
    for (u32 base = start; base < end; base += period) map_pages(c, base + off, size, mem, 1, rw8);
}

void nds_remap(Nds *s) {
    Cpu *a9 = &s->arm9, *a7 = &s->arm7;
    for (int k = 0; k < 2; k++) {
        Cpu *c = k ? a7 : a9;
        memset(c->rp, 0, sizeof c->rp);
        memset(c->wp, 0, sizeof c->wp);
        memset(c->wp8, 0, sizeof c->wp8);
        map_mirror(c, 0x02000000, 0x03000000, 0x400000, 0, s->ram, 0x400000, 1);
    }
    /* shared WRAM */
    u8 *sw9 = 0, *sw7 = 0; u32 sw9n = 0, sw7n = 0;
    switch (s->wramcnt & 3) {
    case 0: sw9 = s->swram; sw9n = 0x8000; break;
    case 1: sw9 = s->swram + 0x4000; sw9n = 0x4000; sw7 = s->swram; sw7n = 0x4000; break;
    case 2: sw9 = s->swram; sw9n = 0x4000; sw7 = s->swram + 0x4000; sw7n = 0x4000; break;
    case 3: sw7 = s->swram; sw7n = 0x8000; break;
    }
    if (sw9) map_mirror(a9, 0x03000000, 0x04000000, sw9n, 0, sw9, sw9n, 1);
    if (sw7) map_mirror(a7, 0x03000000, 0x03800000, sw7n, 0, sw7, sw7n, 1);
    else map_mirror(a7, 0x03000000, 0x03800000, 0x10000, 0, s->wram7, 0x10000, 1);
    map_mirror(a7, 0x03800000, 0x04000000, 0x10000, 0, s->wram7, 0x10000, 1);
    /* VRAM */
    memset(abg, 0, sizeof abg); memset(bbg, 0, sizeof bbg); memset(aobj, 0, sizeof aobj); memset(bobj, 0, sizeof bobj);
    memset(vram_texslot, 0, sizeof vram_texslot); memset(vram_texpal, 0, sizeof vram_texpal); memset(vram_extpal, 0, sizeof vram_extpal);
    for (int b = 0; b < 9; b++) {
        u32 off; int r = bank_region(s, b, &off);
        u8 *m = s->vram + bank_off[b];
        u32 sz = bank_size[b];
        switch (r) {
        case R_LCDC: map_pages(a9, 0x06800000 + bank_off[b], sz, m, 1, 0); break;
        case R_ABG:
            for (u32 o = 0; o < sz; o += 0x4000) abg[((off + o) >> 14) & 31] = m + o;
            map_mirror(a9, 0x06000000, 0x06200000, 0x80000, off, m, sz, 0); break;
        case R_BBG:
            for (u32 o = 0; o < sz; o += 0x4000) bbg[((off + o) >> 14) & 7] = m + o;
            map_mirror(a9, 0x06200000, 0x06400000, 0x20000, off, m, sz, 0); break;
        case R_AOBJ:
            for (u32 o = 0; o < sz; o += 0x4000) aobj[((off + o) >> 14) & 15] = m + o;
            map_mirror(a9, 0x06400000, 0x06600000, 0x40000, off, m, sz, 0); break;
        case R_BOBJ:
            for (u32 o = 0; o < sz; o += 0x4000) bobj[((off + o) >> 14) & 7] = m + o;
            map_mirror(a9, 0x06600000, 0x06800000, 0x20000, off, m, sz, 0); break;
        case R_ARM7: map_mirror(a7, 0x06000000, 0x07000000, 0x40000, off, m, sz, 1); break;
        case R_TEX: vram_texslot[(off >> 17) & 3] = m; break;
        case R_TEXPAL: for (u32 o = 0; o < sz; o += 0x4000) vram_texpal[((off + o) >> 14) & 7] = m + o; break;
        case R_ABGEXT: for (u32 o = 0; o < sz && o < 0x8000; o += 0x2000) vram_extpal[0][((off >> 13) + (o >> 13)) & 3] = m + o; break;
        case R_AOBJEXT: vram_extpal[1][0] = m; break;
        case R_BBGEXT: for (u32 o = 0; o < 0x8000; o += 0x2000) vram_extpal[2][o >> 13] = m + o; break;
        case R_BOBJEXT: vram_extpal[3][0] = m; break;
        }
    }
    /* TCM (ARM9): ITCM mirrored below 32 MB, DTCM at its base; both override the rest */
    if (a9->cp15_ctrl & (1u << 18)) {
        u32 isize = 512u << ((a9->cp15_itcm >> 1) & 0x1F);
        if (isize > 0x2000000) isize = 0x2000000;
        map_mirror(a9, 0, isize, 0x8000, 0, s->itcm, 0x8000, 1);
    }
    if (a9->cp15_ctrl & (1u << 16)) {
        u32 base = a9->cp15_dtcm & 0xFFFFF000u;
        u32 dsize = 512u << ((a9->cp15_dtcm >> 1) & 0x1F);
        if (dsize < 0x4000) dsize = 0x4000;
        if (dsize > 0x100000) dsize = 0x100000;
        map_mirror(a9, base, base + dsize, 0x4000, 0, s->dtcm, 0x4000, 1);
    }
}

static void remap_cb(Cpu *c) { nds_remap(SYS(c)); }

/* ---- interrupts ------------------------------------------------------------- */
void nds_request_irq(Nds *s, int cpu, u32 bits) { s->ifl[cpu] |= bits; }
static int irq_line(Cpu *c) { Nds *s = SYS(c); int k = CPU_OF(c); return (s->ime[k] & 1) && (s->ie[k] & s->ifl[k]); }
static int irq_wake(Cpu *c) { Nds *s = SYS(c); int k = CPU_OF(c); return (s->ie[k] & s->ifl[k]) != 0; }

/* ---- DMA ---------------------------------------------------------------------- */
static int dma_timing(int cpu, u32 cnt) { return cpu == 0 ? (cnt >> 27) & 7 : (cnt >> 28) & 3; }

static void dma_run(Nds *s, int cpu, int ch) {
    Dma *d = &s->dma[cpu][ch];
    Cpu *c = cpu ? &s->arm7 : &s->arm9;
    u32 cnt = d->cnt;
    int word = (cnt >> 26) & 1;
    u32 unit = word ? 4 : 2;
    int sc = (cnt >> 23) & 3, dc = (cnt >> 21) & 3;
    s32 ss = sc == 0 ? (s32)unit : sc == 1 ? -(s32)unit : 0;
    s32 ds = (dc == 0 || dc == 3) ? (s32)unit : dc == 1 ? -(s32)unit : 0;
    for (u32 i = 0; i < d->icount; i++) {
        if (word) wr32(c, d->idst, rd32(c, d->isrc));
        else wr16(c, d->idst, rd16(c, d->isrc));
        d->isrc += (u32)ss; d->idst += (u32)ds;
    }
    c->budget -= (s32)d->icount;
    int timing = dma_timing(cpu, cnt);
    if ((cnt & (1u << 25)) && timing != 0 && !(cpu == 0 && timing == 7)) {
        u32 mask = cpu == 0 ? 0x1FFFFF : (ch == 3 ? 0xFFFF : 0x3FFF);
        d->icount = d->cnt & mask; if (!d->icount) d->icount = mask + 1;
        if (dc == 3) d->idst = d->dst;
    } else {
        d->cnt &= ~0x80000000u;
        u32 o = 0xB8 + 12 * ch;
        memcpy(&s->io[cpu][o], &d->cnt, 4);
    }
    if (cnt & (1u << 30)) nds_request_irq(s, cpu, IRQ_DMA0 << ch);
}

static void dma_trigger(Nds *s, int cpu, int timing) {
    for (int ch = 0; ch < 4; ch++) {
        Dma *d = &s->dma[cpu][ch];
        if ((d->cnt & 0x80000000u) && dma_timing(cpu, d->cnt) == timing) dma_run(s, cpu, ch);
    }
}

static void dma_write_cnt(Nds *s, int cpu, int ch, u32 v) {
    Dma *d = &s->dma[cpu][ch];
    u32 old = d->cnt;
    d->cnt = v;
    if (!(old & 0x80000000u) && (v & 0x80000000u)) {
        u32 mask = cpu == 0 ? 0x1FFFFF : (ch == 3 ? 0xFFFF : 0x3FFF);
        d->isrc = d->src; d->idst = d->dst;
        d->icount = v & mask; if (!d->icount) d->icount = mask + 1;
        int t = dma_timing(cpu, v);
        if (t == 0 || (cpu == 0 && t == 7)) dma_run(s, cpu, ch);
        else if (((cpu == 0 && t == 5) || (cpu == 1 && t == 2)) && s->card_left) dma_run(s, cpu, ch);
    }
}

/* ---- timers --------------------------------------------------------------------- */
static void timer_tick(Nds *s, int cpu, int i, u32 ticks);

static void timer_overflow(Nds *s, int cpu, int i) {
    Timer *t = &s->tim[cpu][i];
    if (t->ctrl & 0x40) nds_request_irq(s, cpu, IRQ_TIMER0 << i);
    if (i < 3) {
        Timer *n = &s->tim[cpu][i + 1];
        if ((n->ctrl & 0x80) && (n->ctrl & 4)) timer_tick(s, cpu, i + 1, 1);
    }
}

static void timer_tick(Nds *s, int cpu, int i, u32 ticks) {
    Timer *t = &s->tim[cpu][i];
    while (ticks) {
        u32 room = 0x10000u - t->counter;
        if (ticks < room) { t->counter = (u16)(t->counter + ticks); return; }
        ticks -= room;
        t->counter = t->reload;
        timer_overflow(s, cpu, i);
    }
}

static void timers_advance(Nds *s, u32 cycles) {
    static const int shift[4] = {0, 6, 8, 10};
    for (int cpu = 0; cpu < 2; cpu++)
        for (int i = 0; i < 4; i++) {
            Timer *t = &s->tim[cpu][i];
            if (!(t->ctrl & 0x80) || (i && (t->ctrl & 4))) continue;
            int sh = shift[t->ctrl & 3];
            t->frac += cycles;
            u32 ticks = t->frac >> sh;
            t->frac &= (1u << sh) - 1;
            if (ticks) timer_tick(s, cpu, i, ticks);
        }
}

/* ---- IPC ----------------------------------------------------------------------- */
static int fifo_full(Fifo *f) { return f->count == 16; }

static void fifo_send(Nds *s, int cpu, u32 v) {
    if (!(s->fifocnt[cpu] & 0x8000)) return;
    Fifo *f = &s->fifo[cpu];
    if (fifo_full(f)) { s->fifocnt[cpu] |= 0x4000; return; }
    int was_empty = f->count == 0;
    f->data[(f->head + f->count) & 15] = v; f->count++;
    if (was_empty && (s->fifocnt[cpu ^ 1] & 0x400)) nds_request_irq(s, cpu ^ 1, IRQ_FIFO_NOT_EMPTY);
}

static u32 fifo_recv(Nds *s, int cpu) {
    Fifo *f = &s->fifo[cpu ^ 1];
    if (!(s->fifocnt[cpu] & 0x8000)) return f->count ? f->data[f->head] : s->fifo_last[cpu];
    if (!f->count) { s->fifocnt[cpu] |= 0x4000; return s->fifo_last[cpu]; }
    u32 v = f->data[f->head];
    f->head = (f->head + 1) & 15; f->count--;
    s->fifo_last[cpu] = v;
    if (!f->count && (s->fifocnt[cpu ^ 1] & 4)) nds_request_irq(s, cpu ^ 1, IRQ_FIFO_EMPTY);
    return v;
}

static u16 fifocnt_read(Nds *s, int cpu) {
    Fifo *snd = &s->fifo[cpu], *rcv = &s->fifo[cpu ^ 1];
    u16 v = s->fifocnt[cpu] & 0xC404;
    if (!snd->count) v |= 1;
    if (fifo_full(snd)) v |= 2;
    if (!rcv->count) v |= 0x100;
    if (fifo_full(rcv)) v |= 0x200;
    return v;
}

static void fifocnt_write(Nds *s, int cpu, u16 v) {
    u16 old = s->fifocnt[cpu];
    if (v & 8) { s->fifo[cpu].count = 0; s->fifo[cpu].head = 0; }
    if (v & 0x4000) old &= ~0x4000;
    s->fifocnt[cpu] = (old & 0x4000) | (v & 0x8404);
    if (!(old & 4) && (v & 4) && !s->fifo[cpu].count) nds_request_irq(s, cpu, IRQ_FIFO_EMPTY);
    if (!(old & 0x400) && (v & 0x400) && s->fifo[cpu ^ 1].count) nds_request_irq(s, cpu, IRQ_FIFO_NOT_EMPTY);
}

/* ---- card --------------------------------------------------------------------- */
static void card_finish(Nds *s) {
    s->romctrl &= ~((1u << 31) | (1u << 23));
    if (s->auxspicnt & 0x4000) nds_request_irq(s, (s->exmemcnt >> 11) & 1, IRQ_CARD_DONE);
}

static void card_start(Nds *s, u32 ctrl) {
    s->romctrl = ctrl & ~(1u << 23);
    if (!(ctrl & (1u << 31))) return;
    u32 bs = (ctrl >> 24) & 7;
    u32 len = bs == 0 ? 0 : bs == 7 ? 4 : 0x100u << bs;
    u8 *cmd = s->card_cmd;
    s->card_left = len;
    switch (cmd[0]) {
    case 0xB7: {
        u32 a = ((u32)cmd[1] << 24) | ((u32)cmd[2] << 16) | ((u32)cmd[3] << 8) | cmd[4];
        if (a < 0x8000) a = 0x8000 + (a & 0x1FF);
        s->card_addr = a;
        break;
    }
    default:
        s->card_addr = 0xFFFFFFFFu;     /* chip ID and the rest: answered with the chip ID */
        break;
    }
    if (!len) { card_finish(s); return; }
    s->romctrl |= 1u << 23;
    int cpu = (s->exmemcnt >> 11) & 1;
    dma_trigger(s, cpu, cpu == 0 ? 5 : 2);
}

static u32 card_read(Nds *s) {
    if (!s->card_left) return 0xFFFFFFFFu;
    u32 v;
    if (s->card_cmd[0] == 0xB7) {
        u32 a = s->card_addr;
        if (a < s->rom_size && s->rom_size - a >= 4) memcpy(&v, s->rom + a, 4); else v = 0xFFFFFFFFu;
        s->card_addr += 4;
    } else v = s->chip_id;
    s->card_left -= 4;
    if (!s->card_left) card_finish(s);
    return v;
}

/* ---- SPI (ARM7): power manager, firmware, touch screen ---------------------------- */
static u8 spi_transfer(Nds *s, u8 b) {
    int dev = (s->spicnt >> 8) & 3;
    u8 out = 0;
    int pos = s->spi_pos++;
    if (dev == 0) {
        if (pos == 0) s->pm_index = b;
        else if (s->pm_index & 0x80) out = s->pm_regs[s->pm_index & 7];
        else s->pm_regs[s->pm_index & 7] = b;
    } else if (dev == 1) {
        if (pos == 0) { s->spi_cmd = b; s->spi_addr = 0; }
        else if (s->spi_cmd == 0x03 || s->spi_cmd == 0x0B) {
            if (pos <= 3) s->spi_addr = (s->spi_addr << 8) | b;
            else if (s->spi_cmd == 0x03 || pos > 4) out = s->firmware[s->spi_addr++ & 0x3FFFF];
        } else if (s->spi_cmd == 0x9F) out = (u8[]){0x20, 0x40, 0x12}[(pos - 1) % 3];
        else if (s->spi_cmd == 0x05) out = 0;
    } else if (dev == 2) {
        if (b & 0x80) {
            int ch = (b >> 4) & 7;
            u32 adc = 0;
            /* calibration from the firmware's user settings: ADC 0x200 at pixel 0x20 to ADC 0xE00/0x800 at 0xE0/0xA0 */
            if (ch == 5) adc = s->touch_down ? 0x200 + (u32)(s->touch_x - 0x1F) * (0xE00 - 0x200) / (0xE0 - 0x20) : 0;
            else if (ch == 1) adc = s->touch_down ? 0x200 + (u32)(s->touch_y - 0x1F) * (0x800 - 0x200) / (0xA0 - 0x20) : 0xFFF;
            else adc = 0x800;
            s->touch_adc = (u16)(adc & 0xFFF);
            out = 0;
            s->spi_pos = 1;
        } else if (pos == 1) out = (u8)(s->touch_adc >> 5);
        else out = (u8)(s->touch_adc << 3);
    }
    if (!(s->spicnt & 0x800)) s->spi_pos = 0;
    if (s->spicnt & 0x4000) nds_request_irq(s, 1, IRQ_SPI);
    return out;
}

/* ---- the divider and square root (ARM9) ------------------------------------------- */
static void div_compute(Nds *s) {
    int mode = s->divcnt & 3;
    s64 n = mode == 0 ? (s64)(s32)s->div_numer : s->div_numer;
    s64 d = mode == 2 ? s->div_denom : (s64)(s32)s->div_denom;
    s->divcnt = (u16)((s->divcnt & ~0x4000) | (s->div_denom == 0 ? 0x4000 : 0));
    if (d == 0) {
        s->div_rem = n;
        s->div_result = n < 0 ? 1 : -1;
        if (mode == 0) s->div_result = (s64)((u64)s->div_result ^ 0xFFFFFFFF00000000ull);
    } else if (mode == 0 && n == INT32_MIN && d == -1) {
        s->div_result = (s64)(u64)0x80000000u; s->div_rem = 0;
    } else if (n == INT64_MIN && d == -1) {
        s->div_result = INT64_MIN; s->div_rem = 0;
    } else {
        s->div_result = n / d; s->div_rem = n % d;
    }
}

static void sqrt_compute(Nds *s) {
    u64 v = (s->sqrtcnt & 1) ? s->sqrt_param : (u32)s->sqrt_param;
    u64 r = 0, bit = 1ull << 62;
    while (bit > v) bit >>= 2;
    while (bit) { if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; } else r >>= 1; bit >>= 2; }
    s->sqrt_result = (u32)r;
}

/* ---- I/O registers ----------------------------------------------------------------- */
static u16 raw16(Nds *s, int cpu, u32 a) { u16 v; memcpy(&v, &s->io[cpu][a & 0x1FFE], 2); return v; }
static u32 raw32(Nds *s, int cpu, u32 a) { u32 v; memcpy(&v, &s->io[cpu][a & 0x1FFC], 4); return v; }

static u16 dispstat_read(Nds *s, int cpu) {
    u16 v = s->dispstat[cpu] & 0xFFB8;
    if (s->vcount >= 192 && s->vcount < 262) v |= 1;
    v |= s->dispstat[cpu] & 2;
    u32 match = ((s->dispstat[cpu] >> 8) & 0xFF) | ((s->dispstat[cpu] & 0x80u) << 1);
    if (s->vcount == match) v |= 4;
    return v;
}

static u32 io_read32(Nds *s, int cpu, u32 a);

static u16 io_read16(Nds *s, int cpu, u32 a) {
    switch (a) {
    case 0x004: return dispstat_read(s, cpu);
    case 0x006: return (u16)s->vcount;
    case 0x100: case 0x104: case 0x108: case 0x10C: return s->tim[cpu][(a - 0x100) >> 2].counter;
    case 0x102: case 0x106: case 0x10A: case 0x10E: return s->tim[cpu][(a - 0x100) >> 2].ctrl;
    case 0x130: return (u16)(~s->keys & 0x3FF);
    case 0x136:
        if (cpu == 1) return (u16)(0x7F & ~((s->keys & KEY_X) ? 1 : 0) & ~((s->keys & KEY_Y) ? 2 : 0) & ~(s->touch_down ? 0x40 : 0));
        return 0;
    case 0x180: return (u16)((s->ipcsync[cpu] & 0x4F00) | ((s->ipcsync[cpu ^ 1] >> 8) & 0xF));
    case 0x184: return fifocnt_read(s, cpu);
    case 0x1A0: return s->auxspicnt;
    case 0x1A2: return 0xFF;            /* no save memory yet */
    case 0x1C0: return cpu == 1 ? (u16)(s->spicnt & ~0x80) : 0;
    case 0x1C2: return cpu == 1 ? s->spi_data : 0;
    case 0x204: return s->exmemcnt;
    case 0x208: return (u16)s->ime[cpu];
    case 0x240: return cpu == 1 ? (u16)(((s->vramcnt[2] & 0x87) == 0x82) | (((s->vramcnt[3] & 0x87) == 0x82) << 1) | (s->wramcnt << 8)) : raw16(s, cpu, a);
    case 0x280: return cpu == 0 ? s->divcnt : 0;
    case 0x2B0: return cpu == 0 ? s->sqrtcnt : 0;
    case 0x304: return cpu == 0 ? s->powcnt1 : raw16(s, cpu, a);
    case 0x600: return 0;
    case 0x602: return cpu == 0 ? 0x0600 : 0;   /* GXSTAT: FIFO empty, under half full */
    }
    if ((a >= 0x210 && a < 0x218) || (a >= 0x1A4 && a < 0x1A8) || (cpu == 0 && a >= 0x2A0 && a < 0x2B8)) {
        u32 v = io_read32(s, cpu, a & ~3u);
        return (u16)(v >> ((a & 2) * 8));
    }
    if (a < 0x2000) return raw16(s, cpu, a);
    return 0;
}

static u32 io_read32(Nds *s, int cpu, u32 a) {
    switch (a) {
    case 0x210: return s->ie[cpu];
    case 0x214: return s->ifl[cpu];
    case 0x1A4: return s->romctrl;
    case 0x100000: return fifo_recv(s, cpu);
    case 0x100010: return card_read(s);
    }
    if (cpu == 0 && a >= 0x2A0 && a < 0x2B8) {
        switch (a) {
        case 0x2A0: return (u32)s->div_result;
        case 0x2A4: return (u32)((u64)s->div_result >> 32);
        case 0x2A8: return (u32)s->div_rem;
        case 0x2AC: return (u32)((u64)s->div_rem >> 32);
        case 0x2B4: return s->sqrt_result;
        }
    }
    if (cpu == 0 && a >= 0x290 && a < 0x2A0) return raw32(s, cpu, a);
    if (cpu == 0 && a >= 0x2B8 && a < 0x2C0) return raw32(s, cpu, a);
    return io_read16(s, cpu, a) | ((u32)io_read16(s, cpu, a + 2) << 16);
}

static void vramcnt_write(Nds *s, int idx, u8 v) {
    if (idx == 7) { s->wramcnt = v & 3; nds_remap(s); return; }
    int b = idx < 7 ? idx : idx - 1;
    if (s->vramcnt[b] != v) { s->vramcnt[b] = v; nds_remap(s); }
}

static void debug_char(Nds *s, int ch) {
    if (ch == '\n' || s->dbg_len >= (int)sizeof s->dbg - 1) {
        s->dbg[s->dbg_len] = 0;
        if (s->debug_line) s->debug_line(s->dbg); else printf("[debug] %s\n", s->dbg);
        s->dbg_len = 0;
        if (ch == '\n') return;
    }
    s->dbg[s->dbg_len++] = (char)ch;
}

static void io_write32(Nds *s, int cpu, u32 a, u32 v);

static void io_write16(Nds *s, int cpu, u32 a, u16 v) {
    if (a < 0x2000) memcpy(&s->io[cpu][a], &v, 2);
    switch (a) {
    case 0x004: s->dispstat[cpu] = (u16)((s->dispstat[cpu] & 7) | (v & 0xFFB8)); return;
    case 0x100: case 0x104: case 0x108: case 0x10C: s->tim[cpu][(a - 0x100) >> 2].reload = v; return;
    case 0x102: case 0x106: case 0x10A: case 0x10E: {
        Timer *t = &s->tim[cpu][(a - 0x100) >> 2];
        if (!(t->ctrl & 0x80) && (v & 0x80)) { t->counter = t->reload; t->frac = 0; }
        t->ctrl = v & 0xC7;
        return;
    }
    case 0x180: {
        s->ipcsync[cpu] = v & 0x4F00;
        if ((v & 0x2000) && (s->ipcsync[cpu ^ 1] & 0x4000)) nds_request_irq(s, cpu ^ 1, IRQ_IPCSYNC);
        return;
    }
    case 0x184: fifocnt_write(s, cpu, v); return;
    case 0x188: case 0x18A: fifo_send(s, cpu, v); return;
    case 0x1A0: s->auxspicnt = v; return;
    case 0x1A2: return;
    case 0x1A8: case 0x1AA: case 0x1AC: case 0x1AE:
        s->card_cmd[a - 0x1A8] = (u8)v; s->card_cmd[a - 0x1A8 + 1] = (u8)(v >> 8); return;
    case 0x1C0: if (cpu == 1) s->spicnt = v; return;
    case 0x1C2: if (cpu == 1 && (s->spicnt & 0x8000)) s->spi_data = spi_transfer(s, (u8)v); return;
    case 0x204:
        if (cpu == 0) s->exmemcnt = (u16)((s->exmemcnt & 0x007F) | (v & 0xFF80));
        else s->exmemcnt = (u16)((s->exmemcnt & 0xFF80) | (v & 0x7F));
        return;
    case 0x208: s->ime[cpu] = v & 1; return;
    case 0x240: case 0x242: case 0x244: case 0x246: case 0x248:
        if (cpu == 0) { vramcnt_write(s, a - 0x240, (u8)v); vramcnt_write(s, a - 0x240 + 1, (u8)(v >> 8)); }
        return;
    case 0x280: if (cpu == 0) { s->divcnt = v & 3; div_compute(s); } return;
    case 0x2B0: if (cpu == 0) { s->sqrtcnt = v & 1; sqrt_compute(s); } return;
    case 0x300: return;
    case 0x304: if (cpu == 0) s->powcnt1 = v; return;
    }
    if ((a >= 0xB0 && a < 0xE0) || (a >= 0x210 && a < 0x218) || (a >= 0x1A4 && a < 0x1A8) ||
        (cpu == 0 && ((a >= 0x290 && a < 0x2A0) || (a >= 0x2B8 && a < 0x2C0)))) {
        u32 w = a & ~3u;
        u32 cur = raw32(s, cpu, w);
        if (w == 0x214) cur = 0;
        if (w == 0x210) cur = s->ie[cpu];
        if (w == 0x1A4) cur = s->romctrl;
        u32 sh = (a & 2) * 8;
        u32 nv = (cur & ~(0xFFFFu << sh)) | ((u32)v << sh);
        if (w == 0x214) nv = (u32)v << sh;
        io_write32(s, cpu, w, nv);
    }
}

static void io_write32(Nds *s, int cpu, u32 a, u32 v) {
    if (a >= 0xB0 && a < 0xE0) {
        memcpy(&s->io[cpu][a], &v, 4);
        int ch = (a - 0xB0) / 12, r = (a - 0xB0) % 12;
        Dma *d = &s->dma[cpu][ch];
        if (r == 0) d->src = v & (cpu == 0 ? 0x0FFFFFFF : 0x07FFFFFF);
        else if (r == 4) d->dst = v & (cpu == 0 ? 0x0FFFFFFF : 0x07FFFFFF);
        else dma_write_cnt(s, cpu, ch, v);
        return;
    }
    if (cpu == 0 && a >= 0xE0 && a < 0xF0) { memcpy(&s->io[cpu][a], &v, 4); s->dmafill[(a - 0xE0) >> 2] = v; return; }
    switch (a) {
    case 0x188: fifo_send(s, cpu, v); return;
    case 0x210: s->ie[cpu] = v; return;
    case 0x214: s->ifl[cpu] &= ~v; return;
    case 0x1A4: card_start(s, v); return;
    case 0x1A8: case 0x1AC: memcpy(&s->card_cmd[a - 0x1A8], &v, 4); return;
    case 0xFFFA1C: case 0xFFFA18: debug_char(s, v & 0xFF); return;
    case 0xFFFA10: case 0xFFFA14: {
        Cpu *c = cpu ? &s->arm7 : &s->arm9;
        for (u32 i = 0; i < 256; i++) { u32 ch = rd8(c, v + i); if (!ch) break; debug_char(s, (int)ch); }
        if (a == 0xFFFA14) debug_char(s, '\n');
        return;
    }
    }
    if (cpu == 0 && a >= 0x290 && a < 0x2A0) {
        memcpy(&s->io[cpu][a], &v, 4);
        memcpy(&s->div_numer, &s->io[0][0x290], 8);
        memcpy(&s->div_denom, &s->io[0][0x298], 8);
        div_compute(s);
        return;
    }
    if (cpu == 0 && a >= 0x2B8 && a < 0x2C0) {
        memcpy(&s->io[cpu][a], &v, 4);
        memcpy(&s->sqrt_param, &s->io[0][0x2B8], 8);
        sqrt_compute(s);
        return;
    }
    if (a >= 0x400 && a < 0x700 && cpu == 0) { memcpy(&s->io[cpu][a], &v, 4); return; }   /* 3D: not yet */
    if (a < 0x2000) { io_write16(s, cpu, a, (u16)v); io_write16(s, cpu, a + 2, (u16)(v >> 16)); }
}

static void io_write8(Nds *s, int cpu, u32 a, u8 v) {
    if (cpu == 0 && a >= 0x240 && a < 0x24A) { vramcnt_write(s, a - 0x240, v); return; }
    if (a == 0x300) return;
    if (a == 0x301 && cpu == 1) { if (v & 0xC0) { s->arm7.halted = 1; s->arm7.budget = 0; } return; }
    if (a >= 0x1A8 && a < 0x1B0) { s->card_cmd[a - 0x1A8] = v; return; }
    if (a == 0x1C2 && cpu == 1) { io_write16(s, cpu, 0x1C2, v); return; }
    if (a >= 0x214 && a < 0x218) { s->ifl[cpu] &= ~((u32)v << ((a & 3) * 8)); return; }
    if (a == 0x208) { s->ime[cpu] = v & 1; return; }
    if (a >= 0xFFFA00) { if (a == 0xFFFA1C) debug_char(s, v); return; }
    if (a >= 0x2000) return;
    u16 cur = io_read16(s, cpu, a & ~1u);
    if (a >= 0x210 && a < 0x214) cur = (u16)(s->ie[cpu] >> ((a & 2) * 8));
    u16 nv = (a & 1) ? (u16)((cur & 0x00FF) | (v << 8)) : (u16)((cur & 0xFF00) | v);
    io_write16(s, cpu, a & ~1u, nv);
}

/* ---- the slow memory path ------------------------------------------------------------- */
/* The subsystems in files of their own get the first look at each I/O access. */
static int sub_io_read(Nds *s, int cpu, u32 off, int size, u32 *v) {
    return rtc_io_read(s, cpu, off, size, v) || save_io_read(s, cpu, off, size, v) ||
           sound_io_read(s, cpu, off, size, v) || gpu3d_io_read(s, cpu, off, size, v);
}

static int sub_io_write(Nds *s, int cpu, u32 off, int size, u32 v) {
    return rtc_io_write(s, cpu, off, size, v) || save_io_write(s, cpu, off, size, v) ||
           sound_io_write(s, cpu, off, size, v) || gpu3d_io_write(s, cpu, off, size, v);
}

int nds_option(Nds *s, const char *name, const char *value) {
    return rtc_option(s, name, value) || save_option(s, name, value) ||
           sound_option(s, name, value) || gpu3d_option(s, name, value);
}

void nds_shutdown(Nds *s) {
    rtc_shutdown(s); save_shutdown(s); sound_shutdown(s); gpu3d_shutdown(s);
}

static u32 read_slow(Cpu *c, u32 a, int size) {
    Nds *s = SYS(c);
    int cpu = CPU_OF(c);
    switch (a >> 24) {
    case 0x04: {
        u32 off = a - 0x04000000u;
        u32 hv;
        if (sub_io_read(s, cpu, off, size, &hv)) return hv;
        if (size == 4) return io_read32(s, cpu, off);
        if (size == 2) return io_read16(s, cpu, off);
        if (off == 0x100000 || off == 0x100010) return io_read32(s, cpu, off) & 0xFF;
        return (io_read16(s, cpu, off & ~1u) >> ((off & 1) * 8)) & 0xFF;
    }
    case 0x05: if (cpu == 0) { u32 v = 0; memcpy(&v, &s->pal[a & 0x7FF], (size_t)size); return v; } break;
    case 0x07: if (cpu == 0) { u32 v = 0; memcpy(&v, &s->oam[a & 0x7FF], (size_t)size); return v; } break;
    case 0x08: case 0x09: return size == 1 ? 0xFF : size == 2 ? 0xFFFF : 0xFFFFFFFFu;   /* no GBA cartridge */
    }
    return 0;
}

static void write_slow(Cpu *c, u32 a, u32 v, int size) {
    Nds *s = SYS(c);
    int cpu = CPU_OF(c);
    switch (a >> 24) {
    case 0x04: {
        u32 off = a - 0x04000000u;
        if (sub_io_write(s, cpu, off, size, v)) return;
        if (size == 4) io_write32(s, cpu, off, v);
        else if (size == 2) io_write16(s, cpu, off, (u16)v);
        else io_write8(s, cpu, off, (u8)v);
        return;
    }
    case 0x05: if (cpu == 0 && size > 1) memcpy(&s->pal[a & 0x7FF], &v, (size_t)size); return;
    case 0x07: if (cpu == 0 && size > 1) memcpy(&s->oam[a & 0x7FF], &v, (size_t)size); return;
    }
}

static void log_miss(Cpu *c, u32 pc, u32 thumb) {
    FILE *f = fopen("missing.txt", "a");
    if (f) { fprintf(f, "%d %08x %s\n", c->arch == 5 ? 9 : 7, pc, thumb ? "t" : "a"); fclose(f); }
}

/* ---- the machine -------------------------------------------------------------------- */
static void make_firmware(Nds *s) {
    u8 *f = s->firmware = malloc(0x40000);
    memset(f, 0xFF, 0x40000);
    memset(f, 0, 0x200);
    f[0x20] = 0xC0; f[0x21] = 0x7F;                 /* user settings at 0x7FC0 * 8 = 0x3FE00 */
    f[0x1D] = 0xFF;                                 /* console type: DS */
    for (int copy = 0; copy < 2; copy++) {
        u8 *u = f + 0x3FE00 + copy * 0x100;
        memset(u, 0, 0x100);
        u[0x00] = 5; u[0x02] = 11; u[0x03] = 1; u[0x04] = 1;
        const char *name = "dsnative";
        for (int i = 0; name[i]; i++) u[0x06 + 2 * i] = (u8)name[i];
        u[0x1A] = 8;
        u16 cal[8] = {0x200, 0x200, 0x20, 0x20, 0xE00, 0x800, 0xE0, 0xA0};   /* touch calibration */
        memcpy(u + 0x58, &cal[0], 2); memcpy(u + 0x5A, &cal[1], 2); u[0x5C] = 0x20; u[0x5D] = 0x20;
        memcpy(u + 0x5E, &cal[4], 2); memcpy(u + 0x60, &cal[5], 2); u[0x62] = 0xE0; u[0x63] = 0xA0;
        u[0x64] = 0x01 | 0x08;                      /* English, GBA screen bottom */
        u[0x66] = 26;                               /* year 2026 */
        u[0x70] = (u8)copy;
        u16 crc = 0xFFFF;
        for (int i = 0; i < 0x70; i++) { crc ^= u[i]; for (int k = 0; k < 8; k++) crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1; }
        memcpy(u + 0x72, &crc, 2);
    }
}

Nds *nds_create(const u8 *rom, u32 rom_size) {
    Nds *s = calloc(1, sizeof *s);
    s->ram = calloc(1, 0x400000);
    s->vram = calloc(1, 0xA4000);
    s->rom = rom; s->rom_size = rom_size;
    s->chip_id = 0x00001FC2;
    make_firmware(s);
    for (int k = 0; k < 2; k++) {
        Cpu *c = k ? &s->arm7 : &s->arm9;
        dsr_reset(c, k ? 4 : 5, 0);
        c->sys = s;
        c->read_slow = read_slow; c->write_slow = write_slow;
        c->irq_line = irq_line; c->irq_wake = irq_wake;
        c->remap = remap_cb; c->log_miss = log_miss;
        dsr_flush_cache(c);
    }
    s->wramcnt = 3;
    nds_remap(s);
    rtc_init(s); save_init(s); sound_init(s); gpu3d_init(s);
    return s;
}

static void w32(Nds *s, u32 a, u32 v) { memcpy(s->ram + (a & 0x3FFFFF), &v, 4); }
static void w16(Nds *s, u32 a, u16 v) { memcpy(s->ram + (a & 0x3FFFFF), &v, 2); }

static void cpu_mode_sp(Cpu *c, u32 mode, u32 sp) {
    extern void dsr_set_cpsr(Cpu *c, u32 v, u32 mask);
    dsr_set_cpsr(c, 0xC0 | mode, 0xFF);
    c->r[13] = sp;
}

void nds_direct_boot(Nds *s, const u8 *arm9, u32 arm9_len) {
    const u8 *h = s->rom;
    u32 a9ram, a9entry, a7rom, a7ram, a7entry, a7size;
    memcpy(&a9entry, h + 0x24, 4); memcpy(&a9ram, h + 0x28, 4);
    memcpy(&a7rom, h + 0x30, 4); memcpy(&a7entry, h + 0x34, 4); memcpy(&a7ram, h + 0x38, 4); memcpy(&a7size, h + 0x3C, 4);
    /* the binaries */
    for (u32 i = 0; i < arm9_len; i++) {
        u32 a = a9ram + i;
        if ((a >> 24) == 2) s->ram[a & 0x3FFFFF] = arm9[i];
    }
    for (u32 i = 0; i < a7size; i++) {
        u32 a = a7ram + i;
        if ((a >> 24) == 2) s->ram[a & 0x3FFFFF] = h[a7rom + i];
        else if (a >= 0x03800000) s->wram7[a & 0xFFFF] = h[a7rom + i];
    }
    /* what the firmware leaves */
    u16 hcrc; memcpy(&hcrc, h + 0x15E, 2);
    u16 scrc; memcpy(&scrc, h + 0x6C, 2);
    w32(s, 0x027FF800, s->chip_id); w32(s, 0x027FF804, s->chip_id);
    w16(s, 0x027FF808, hcrc); w16(s, 0x027FF80A, scrc);
    w16(s, 0x027FF850, 0x5835);
    w32(s, 0x027FFC00, s->chip_id); w32(s, 0x027FFC04, s->chip_id);
    w16(s, 0x027FFC08, hcrc); w16(s, 0x027FFC0A, scrc);
    w16(s, 0x027FFC10, 0x5835); w16(s, 0x027FFC30, 0xFFFF); w16(s, 0x027FFC40, 0x0001);
    memcpy(s->ram + 0x3FFE00, h, 0x170);
    memcpy(s->ram + 0x3FFC80, s->firmware + 0x3FE00, 0x70);
    s->io[0][0x300] = s->io[1][0x300] = 1;      /* POSTFLG */
    s->wramcnt = 3;
    s->powcnt1 = 0x820F;
    s->exmemcnt = 0x6000;
    s->pm_regs[0] = 0x0D;                       /* power manager: sound and backlights on */
    Cpu *c9 = &s->arm9, *c7 = &s->arm7;
    c9->cp15_ctrl = 0x00012078; c9->cp15_dtcm = 0x0300000A; c9->cp15_itcm = 0x00000020;
    c9->bios_base = 0xFFFF0000u;
    c9->irq_check_addr = 0x03003FF8u;
    cpu_mode_sp(c9, MODE_IRQ, 0x03003F80); cpu_mode_sp(c9, MODE_SVC, 0x03003FC0); cpu_mode_sp(c9, MODE_SYS, 0x03002F7C);
    cpu_mode_sp(c7, MODE_IRQ, 0x0380FF80); cpu_mode_sp(c7, MODE_SVC, 0x0380FFC0); cpu_mode_sp(c7, MODE_SYS, 0x0380FD80);
    c9->r[12] = c9->r[14] = c9->r[15] = a9entry; c9->t = 0;
    c7->r[12] = c7->r[14] = c7->r[15] = a7entry; c7->t = 0;
    nds_remap(s);
}

/* ---- the frame ----------------------------------------------------------------------- */
#define CHUNK 256

static void run_cycles(Nds *s, u32 cycles) {
    while (cycles) {
        u32 n = cycles < CHUNK ? cycles : CHUNK;
        if (!s->arm9.stop) dsr_run(&s->arm9, (s32)(2 * n));
        if (!s->arm7.stop) dsr_run(&s->arm7, (s32)n);
        timers_advance(s, n);
        s->time += n;
        cycles -= n;
    }
}

static void line_start(Nds *s, u32 line) {
    s->vcount = line;
    for (int cpu = 0; cpu < 2; cpu++) {
        u16 ds = s->dispstat[cpu];
        u32 match = ((ds >> 8) & 0xFF) | ((ds & 0x80u) << 1);
        if (line == match && (ds & 0x20)) nds_request_irq(s, cpu, IRQ_VCOUNT);
        if (line == 192) {
            if (ds & 0x08) nds_request_irq(s, cpu, IRQ_VBLANK);
            dma_trigger(s, cpu, 1);
        }
    }
}

void nds_run_frame(Nds *s) {
    for (u32 line = 0; line < 263; line++) {
        line_start(s, line);
        run_cycles(s, 1536);
        if (line < 192) {
            int swap = (s->powcnt1 >> 15) & 1;
            gpu2d_render_line(s, 0, (int)line, s->fb[swap ? 0 : 1] + line * SCREEN_W);
            gpu2d_render_line(s, 1, (int)line, s->fb[swap ? 1 : 0] + line * SCREEN_W);
        }
        for (int cpu = 0; cpu < 2; cpu++) {
            s->dispstat[cpu] |= 2;
            if (s->dispstat[cpu] & 0x10) nds_request_irq(s, cpu, IRQ_HBLANK);
        }
        if (line < 192) dma_trigger(s, 0, 2);
        run_cycles(s, 594);
        s->dispstat[0] &= ~2; s->dispstat[1] &= ~2;
        sound_line(s, line);
        gpu3d_line(s, line);
        rtc_line(s, line);
    }
    s->frame++;
}
