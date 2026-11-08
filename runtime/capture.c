/* dsnative runtime: engine A's display capture and the main memory display FIFO. Reference:
 * GBATEK, "DS Video Capture and Main Memory Display Mode" (DISPCAPCNT, DISP_MMEM_FIFO). */
#include "nds.h"

#define BUSY 0x80000000u
/* the writable bits: EVA, EVB, write bank, write offset, size, source A, source B, read offset, source, enable */
#define WRITABLE 0xEF3F1F1Fu

static const u16 size_w[4] = {128, 256, 256, 256}, size_h[4] = {128, 64, 128, 192};

static u32 bank_offset(u32 field) { return field * 0x8000u; }

static u32 dispcnt_a(Nds *s) { u32 v; memcpy(&v, s->io[0], 4); return v; }

int capture_io_read(Nds *s, int cpu, u32 off, int size, u32 *v) {
    if (cpu != 0 || off < 0x64 || off >= 0x68) return 0;
    u32 r = s->cap.cnt >> ((off & 3) * 8);
    *v = size == 4 ? r : size == 2 ? r & 0xFFFF : r & 0xFF;
    return 1;
}

int capture_io_write(Nds *s, int cpu, u32 off, int size, u32 v) {
    Capture *c = &s->cap;
    if (cpu != 0) return 0;
    if (off >= 0x64 && off < 0x68) {
        u32 sh = (off & 3) * 8, m = (size == 4 ? 0xFFFFFFFFu : (1u << (size * 8)) - 1) << sh;
        c->cnt = ((c->cnt & ~m) | ((v << sh) & m)) & WRITABLE;
        if (!(c->cnt & BUSY)) c->run = 0;               /* clearing the enable bit stops a capture */
        return 1;
    }
    if (off >= 0x68 && off < 0x6C) {
        if (size == 1) return 1;
        for (int i = 0; i < size / 2; i++) {            /* a word is two pixels, the first in the low half */
            if (c->fifo_count == CAPTURE_FIFO) break;
            c->fifo[(c->fifo_head + c->fifo_count++) % CAPTURE_FIFO] = (u16)(v >> (16 * i));
        }
        return 1;
    }
    return 0;
}

void capture_line_start(Nds *s, u32 line) {
    Capture *c = &s->cap;
    if (line == 0) {
        c->run = (c->cnt & BUSY) ? c->cnt : 0;
        c->fifo_head = c->fifo_count = 0;               /* each frame starts the stream afresh */
        c->dma_ok = 0;
        for (int ch = 0; ch < 4; ch++) {
            const Dma *d = &s->dma[0][ch];
            if ((d->cnt & 0x80000000u) && ((d->cnt >> 27) & 7) == 4) c->dma_ok = 1;
        }
    } else if (line == 192 && c->run) {
        c->cnt &= ~BUSY;
        c->run = 0;
    }
}

int capture_fifo_wanted(Nds *s) {
    return s->cap.dma_ok && (((dispcnt_a(s) >> 16) & 3) == 3 || (s->cap.run && capture_wants_fifo(s)));
}

int capture_covers(Nds *s, int line) {
    u32 run = s->cap.run;
    return run && line < size_h[(run >> 20) & 3];
}

int capture_wants_graphics(Nds *s) {
    u32 run = s->cap.run, src = (run >> 29) & 3;
    return src != 1 && !(run & (1u << 24));
}

int capture_wants_fifo(Nds *s) {
    u32 run = s->cap.run;
    return ((run >> 29) & 3) != 0 && (run & (1u << 25));
}

void capture_fifo_take(Nds *s, u16 *px) {
    Capture *c = &s->cap;
    for (int x = 0; x < SCREEN_W; x++) {
        if (c->fifo_count) { px[x] = c->fifo[c->fifo_head]; c->fifo_head = (c->fifo_head + 1) % CAPTURE_FIFO; c->fifo_count--; }
        else px[x] = 0;
    }
}

/* Dest = A * EVA + B * EVB per channel, each term only where its source is opaque (bit 15) */
static u16 blend(u16 a, u16 b, u32 eva, u32 evb) {
    u32 fa = (a & 0x8000) ? eva : 0, fb = (b & 0x8000) ? evb : 0;
    u32 out = 0;
    for (int sh = 0; sh < 15; sh += 5) {
        u32 v = ((((u32)a >> sh) & 31) * fa + (((u32)b >> sh) & 31) * fb) >> 4;
        out |= (v > 31 ? 31 : v) << sh;
    }
    return (u16)(out | ((fa || fb) ? 0x8000 : 0));
}

void capture_line(Nds *s, int line, const u16 *gfx, const u16 *fifo, int vram_display) {
    Capture *c = &s->cap;
    u32 run = c->run;
    u32 src = (run >> 29) & 3, w = size_w[(run >> 20) & 3];
    u32 eva = run & 31, evb = (run >> 8) & 31;
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    u16 a[SCREEN_W], b[SCREEN_W];
    if (src != 1) {
        if (run & (1u << 24)) { u8 alpha[SCREEN_W]; gpu3d_bg0_line(s, line, a, alpha); }
        else for (u32 x = 0; x < w; x++) a[x] = gfx[x] | 0x8000;
    }
    if (src != 0) {
        if (run & (1u << 25)) for (u32 x = 0; x < w; x++) b[x] = fifo[x] | 0x8000;     /* bit 15 of a FIFO pixel is unused */
        else {
            /* GBATEK's block diagram has one "Select Display VRAM" feeding both the screen and source B,
             * so B reads the display's lines, 256 pixels apart whatever the capture size (the
             * destination, below, is packed w pixels a line) */
            const u8 *bank = s->vram + ((dispcnt_a(s) >> 18) & 3) * 0x20000u;
            u32 base = vram_display ? 0 : bank_offset((run >> 26) & 3);
            for (u32 x = 0; x < w; x++) {
                u32 o = (base + ((u32)line * SCREEN_W + x) * 2) & 0x1FFFF;
                memcpy(&b[x], bank + o, 2);
            }
        }
    }
    u8 *dst = s->vram + ((run >> 16) & 3) * 0x20000u;
    u32 base = bank_offset((run >> 18) & 3);
    for (u32 x = 0; x < w; x++) {
        u16 v = src == 0 ? a[x] : src == 1 ? b[x] : blend(a[x], b[x], eva, evb);
        u32 o = (base + ((u32)line * w + x) * 2) & 0x1FFFF;
        memcpy(dst + o, &v, 2);
    }
}
