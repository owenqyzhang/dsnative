/* Display capture and the main memory display FIFO (capture.c, gpu2d.c) against GBATEK ("DS Video Capture
 * and Main Memory Display Mode"): DISPCAPCNT's fields, the sizes and offsets, source A (the composed 2D
 * output or the 3D layer alone), source B (a VRAM bank or the FIFO), the blend and its alpha rule, and
 * when the capture runs and its busy bit clears.
 *
 * Whole frames run with both CPUs stopped, so the video and the line hooks are all that happens. What a
 * game would do in the middle of a frame is done by DMA, which nds.c triggers at the real times: HBlank
 * DMA writes a table of values into a register one line at a time (or copies a register into RAM, to
 * read it at each line), and VBlank DMA reads a register at the start of line 192. Expected values are
 * worked out by hand from GBATEK's formulas, never recorded from a run. */
#include <stdio.h>
#include <string.h>
#include "nds.h"

static Nds *s;
static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("%s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define IO 0x04000000u
#define DISPCAPCNT 0x64u
#define BUSY 0x80000000u
#define RGB(r, g, b) ((u32)(r) | (u32)(g) << 5 | (u32)(b) << 10)
#define OPAQUE 0x8000u

/* DISPCAPCNT: EVA, EVB, write bank and offset, size, sources A and B, read offset, capture source, enable */
#define CAP(eva, evb, wb, woff, size, srca, srcb, roff, src) \
    ((u32)(eva) | (u32)(evb) << 8 | (u32)(wb) << 16 | (u32)(woff) << 18 | (u32)(size) << 20 | (u32)(srca) << 24 | \
     (u32)(srcb) << 25 | (u32)(roff) << 26 | (u32)(src) << 29 | BUSY)
enum { SZ128, SZ256x64, SZ256x128, SZ256x192 };
enum { SRC_A, SRC_B, SRC_BLEND };

static void io32(u32 off, u32 v) { wr32(&s->arm9, IO + off, v); }
static u32 get32(u32 off) { return rd32(&s->arm9, IO + off); }
static u16 *bank(int b) { return (u16 *)(s->vram + (u32)b * 0x20000); }
static void fill(int b, u16 v) { for (u32 i = 0; i < 0x10000; i++) bank(b)[i] = v; }
static u32 argb(u32 c) {
    u32 r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return 0xFF000000u | (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
}
static u32 screen(int x, int y) { return s->fb[0][y * SCREEN_W + x]; }      /* engine A is the upper screen */

static const u32 A_COL = RGB(20, 10, 30);       /* the backdrop: what the graphics screen shows with no layers on */

static void fresh(void) {
    static u8 rom[0x1000];
    if (s) nds_shutdown(s);
    s = nds_create(rom, sizeof rom);
    s->arm9.stop = s->arm7.stop = 1;
    s->powcnt1 = 0x8207;                /* both screens on, engine A on top; the 3D renderer off, so a frame poked into the 3D engine stays */
    for (int b = 0; b < 4; b++) wr8(&s->arm9, IO + 0x240 + (u32)b, 0x80);     /* VRAM A-D: LCDC */
    wr16(&s->arm9, 0x05000000, A_COL);
    io32(0, 1u << 16);                  /* graphics display, no layers: the backdrop */
}

static void frame(void) { nds_run_frame(s); }

/* halfword i of a bank pattern: opaque, and different for i and i + 0x8000 (a read offset of 0x10000 bytes
 * must not look like none) */
static u16 pat(u32 i) { return (u16)(OPAQUE | ((i + (i >> 15) * 0x1357) & 0x7FFF)); }
static void pattern(int b) { for (u32 i = 0; i < 0x10000; i++) bank(b)[i] = pat(i); }

/* ---- the register ----------------------------------------------------------------------------- */
static void test_register(void) {
    fresh();
    io32(DISPCAPCNT, 0xFFFFFFFFu);
    CHECK(get32(DISPCAPCNT) == 0xEF3F1F1Fu, "unused bits read 0: %08x", get32(DISPCAPCNT));
    io32(DISPCAPCNT, 0);
    wr8(&s->arm9, IO + DISPCAPCNT, 0xFF);
    CHECK(get32(DISPCAPCNT) == 0x1F, "byte write, EVA: %08x", get32(DISPCAPCNT));
    wr8(&s->arm9, IO + DISPCAPCNT + 1, 0xFF);
    wr16(&s->arm9, IO + DISPCAPCNT + 2, 0x003F);
    CHECK(get32(DISPCAPCNT) == 0x003F1F1F, "byte and halfword writes: %08x", get32(DISPCAPCNT));
    wr8(&s->arm9, IO + DISPCAPCNT + 3, 0xFF);
    CHECK(get32(DISPCAPCNT) == 0xEF3F1F1Fu, "top byte: %08x", get32(DISPCAPCNT));
    wr8(&s->arm9, IO + DISPCAPCNT + 3, 0x00);
    CHECK(get32(DISPCAPCNT) == 0x003F1F1F, "enable cleared by writing it");
}

/* ---- sizes, offsets, sources ------------------------------------------------------------------- */
static void test_sizes(void) {
    static const u32 w[4] = {128, 256, 256, 256}, h[4] = {128, 64, 128, 192};
    for (int k = 0; k < 4; k++) {
        fresh();
        fill(1, 0x1357);
        io32(DISPCAPCNT, CAP(0, 0, 1, 0, k, 0, 0, 0, SRC_A));
        frame();
        u32 n = w[k] * h[k];
        int bad = 0;
        for (u32 i = 0; i < n; i++) bad += bank(1)[i] != (A_COL | OPAQUE);
        CHECK(!bad, "size %u x %u: %d of %u captured pixels wrong", w[k], h[k], bad, n);
        /* the bitmap is packed: w pixels a line, so nothing is written past w * h */
        int stray = 0;
        for (u32 i = n; i < 0x10000; i++) stray += bank(1)[i] != 0x1357;
        CHECK(!stray, "size %u x %u: %d pixels written beyond it", w[k], h[k], stray);
        CHECK(!(get32(DISPCAPCNT) & BUSY), "busy bit cleared after the frame");
        CHECK(bank(0)[0] == 0 && bank(2)[0] == 0 && bank(3)[0] == 0, "other banks untouched");
    }
}

static void test_write_offset(void) {
    for (u32 off = 0; off < 4; off++) {
        fresh();
        pattern(0);                                 /* source B: bank A, read from offset 0 */
        fill(2, 0x1357);
        io32(0, 1u << 16 | 0u << 18);
        io32(DISPCAPCNT, CAP(0, 0, 2, off, SZ256x192, 0, 0, 0, SRC_B));
        frame();
        /* pixel i lands at write offset + 2 i, wrapping at 128 KB */
        int bad = 0;
        for (u32 i = 0; i < 256 * 192; i++) bad += bank(2)[((off * 0x4000u) + i) & 0xFFFF] != pat(i);
        CHECK(!bad, "write offset %05x: %d pixels wrong", off * 0x8000, bad);
    }
    /* 256x192 at 0x18000 runs past the end: line 64 is the first to wrap, to offset 0 */
    fresh();
    pattern(0);
    fill(2, 0x1357);
    io32(DISPCAPCNT, CAP(0, 0, 2, 3, SZ256x192, 0, 0, 0, SRC_B));
    frame();
    CHECK(bank(2)[0x18000 / 2] == pat(0) && bank(2)[0] == pat(64 * 256) && bank(2)[1] == pat(64 * 256 + 1), "wrap at 128 KB: %04x %04x",
          bank(2)[0], bank(2)[0x18000 / 2]);
    /* a 128x128 capture at offset 0x10000 */
    fresh();
    pattern(0);
    fill(3, 0x1357);
    io32(DISPCAPCNT, CAP(0, 0, 3, 2, SZ128, 0, 0, 0, SRC_B));
    frame();
    CHECK(bank(3)[0x8000] == pat(0) && bank(3)[0x8000 + 127] == pat(127) && bank(3)[0x8000 + 128] == pat(256), "128 wide, packed, at 0x10000");
    CHECK(bank(3)[0x7FFF] == 0x1357 && bank(3)[0x8000 + 128 * 128] == 0x1357, "and nothing around it");
}

static void test_source_b_vram(void) {
    /* the read block is DISPCNT.18-19: bank C holds the pattern here */
    fresh();
    pattern(2); fill(0, 0x1357); fill(1, 0x2468);
    io32(0, 1u << 16 | 2u << 18);
    io32(DISPCAPCNT, CAP(0, 0, 1, 0, SZ256x192, 0, 0, 0, SRC_B));
    frame();
    int bad = 0;
    for (u32 i = 0; i < 256 * 192; i++) bad += bank(1)[i] != pat(i);
    CHECK(!bad, "source B from the bank DISPCNT names: %d wrong", bad);
    /* the read offset (0x8000 steps) applies in graphics display mode ... */
    fresh();
    pattern(0); fill(1, 0x2468);
    io32(0, 1u << 16);
    io32(DISPCAPCNT, CAP(0, 0, 1, 0, SZ256x192, 0, 0, 2, SRC_B));
    frame();
    bad = 0;
    for (u32 i = 0; i < 256 * 192; i++) bad += bank(1)[i] != pat((0x8000 + i) & 0xFFFF);     /* 0x10000 bytes on: halfword 0x8000, wrapping at 128 KB */
    CHECK(!bad, "read offset 0x10000: %d wrong", bad);
    /* ... and is ignored (zero) in VRAM display mode, which displays that same bank */
    fresh();
    pattern(0); fill(1, 0x2468);
    io32(0, 2u << 16);
    io32(DISPCAPCNT, CAP(0, 0, 1, 0, SZ256x192, 0, 0, 2, SRC_B));
    frame();
    bad = 0;
    for (u32 i = 0; i < 256 * 192; i++) bad += bank(1)[i] != pat(i);
    CHECK(!bad, "read offset ignored in VRAM display mode: %d wrong", bad);
    /* the read offset wraps at 128 KB too */
    fresh();
    pattern(0); fill(1, 0x2468);
    io32(0, 1u << 16);
    io32(DISPCAPCNT, CAP(0, 0, 1, 0, SZ256x192, 0, 0, 3, SRC_B));
    frame();
    CHECK(bank(1)[0] == pat(0xC000) && bank(1)[0x4000 - 1] == pat(0xFFFF) && bank(1)[0x4000] == pat(0), "read offset 0x18000 wraps: %04x %04x", bank(1)[0x4000 - 1], bank(1)[0x4000]);
    /* a 128-wide capture reads the display's lines, 256 pixels apart (the destination is packed) */
    fresh();
    pattern(0); fill(1, 0x2468);
    io32(0, 1u << 16);
    io32(DISPCAPCNT, CAP(0, 0, 1, 0, SZ128, 0, 0, 0, SRC_B));
    frame();
    CHECK(bank(1)[0] == pat(0) && bank(1)[127] == pat(127) && bank(1)[128] == pat(256) && bank(1)[128 * 127 + 5] == pat(256 * 127 + 5),
          "128x128 from source B: %04x %04x", bank(1)[128], bank(1)[128 * 127 + 5]);
}

/* The captured graphics screen is the composed output before master brightness and whatever the
 * display shows. The capture reads it from the same line that the display does. */
static void test_source_a_graphics(void) {
    fresh();
    io32(0, 0);                                     /* display off (white): the capture takes the 2D output regardless */
    io32(DISPCAPCNT, CAP(0, 0, 1, 0, SZ256x192, 0, 0, 0, SRC_A));
    frame();
    CHECK(bank(1)[0] == (A_COL | OPAQUE) && bank(1)[256 * 192 - 1] == (A_COL | OPAQUE), "captured while the display is off: %04x", bank(1)[0]);
    fresh();
    wr16(&s->arm9, IO + 0x6C, 2 << 14 | 16);        /* master brightness: black */
    io32(DISPCAPCNT, CAP(0, 0, 1, 0, SZ256x192, 0, 0, 0, SRC_A));
    frame();
    CHECK(bank(1)[0] == (A_COL | OPAQUE), "before master brightness: %04x", bank(1)[0]);
    CHECK(screen(0, 0) == 0xFF000000u, "while the screen shows it dimmed: %08x", screen(0, 0));
    /* engine B's backdrop isn't captured: capture is for engine A only */
    fresh();
    wr16(&s->arm9, 0x05000400, RGB(1, 2, 3));
    io32(DISPCAPCNT, CAP(0, 0, 1, 0, SZ256x192, 0, 0, 0, SRC_A));
    frame();
    CHECK(bank(1)[0] == (A_COL | OPAQUE), "engine A only");
}

/* ---- the blend --------------------------------------------------------------------------------- */
static u32 blend_case(u32 eva, u32 evb, u32 b_alpha, u32 src) {
    fresh();
    fill(0, (u16)(RGB(10, 31, 4) | b_alpha));       /* source B: bank A, one colour */
    io32(DISPCAPCNT, CAP(eva, evb, 1, 0, SZ256x64, 0, 0, 0, src));
    frame();
    int same = 1;
    for (u32 i = 1; i < 256 * 64; i++) same &= bank(1)[i] == bank(1)[0];
    CHECK(same, "uniform inputs must give a uniform capture");
    return bank(1)[0];
}

static void test_blend(void) {
    /* A = (20, 10, 30), B = (10, 31, 4): Dest = (A * EVA + B * EVB) / 16 per channel, where a source's term
     * counts only when its alpha (bit 15) is set, saturating at 31; Dest alpha = (A alpha and EVA > 0) or
     * (B alpha and EVB > 0) */
    u32 got;
    got = blend_case(8, 8, OPAQUE, SRC_BLEND);
    CHECK(got == (OPAQUE | RGB(15, 20, 17)), "8/16 + 8/16: %04x", got);
    got = blend_case(4, 12, OPAQUE, SRC_BLEND);
    CHECK(got == (OPAQUE | RGB(12, 25, 10)), "4/16 + 12/16: %04x", got);
    got = blend_case(16, 16, OPAQUE, SRC_BLEND);
    CHECK(got == (OPAQUE | RGB(30, 31, 31)), "16 + 16 saturates: %04x", got);
    got = blend_case(31, 31, OPAQUE, SRC_BLEND);
    CHECK(got == (OPAQUE | RGB(30, 31, 31)), "coefficients above 16 count as 16: %04x", got);
    got = blend_case(16, 0, OPAQUE, SRC_BLEND);
    CHECK(got == (OPAQUE | A_COL), "A only by coefficients: %04x", got);
    got = blend_case(0, 16, OPAQUE, SRC_BLEND);
    CHECK(got == (OPAQUE | RGB(10, 31, 4)), "B only by coefficients: %04x", got);
    got = blend_case(0, 0, OPAQUE, SRC_BLEND);
    CHECK(got == 0, "no weight at all: black and transparent: %04x", got);
    /* 3 selects the blend too */
    got = blend_case(8, 8, OPAQUE, 3);
    CHECK(got == (OPAQUE | RGB(15, 20, 17)), "capture source 3 blends as 2: %04x", got);
    /* the coefficients matter only when blending */
    got = blend_case(0, 0, OPAQUE, SRC_A);
    CHECK(got == (OPAQUE | A_COL), "source A alone ignores EVA/EVB: %04x", got);
    got = blend_case(3, 5, OPAQUE, SRC_B);
    CHECK(got == (OPAQUE | RGB(10, 31, 4)), "source B alone: %04x", got);
    /* B without its alpha bit counts for nothing, and gives no alpha */
    got = blend_case(8, 8, 0, SRC_BLEND);
    CHECK(got == (OPAQUE | RGB(10, 5, 15)), "B transparent: A's half only: %04x", got);
    got = blend_case(0, 8, 0, SRC_BLEND);
    CHECK(got == 0, "B transparent and A weighted 0: %04x", got);
    got = blend_case(16, 8, 0, SRC_BLEND);
    CHECK(got == (OPAQUE | A_COL), "B transparent: %04x", got);
    /* source B alone keeps B's alpha bit as it is */
    got = blend_case(0, 0, 0, SRC_B);
    CHECK(got == RGB(10, 31, 4), "B alone, transparent: %04x", got);
}

/* ---- the 3D layer ------------------------------------------------------------------------------ */
/* the 3D engine's frame as the rasterizer leaves it: 6-bit colour in bits 0-17, 5-bit alpha in 18-22 */
static u32 g3d_px(u32 r6, u32 g6, u32 b6, u32 alpha) { return r6 | g6 << 6 | b6 << 12 | alpha << 18; }

static void poke_3d(void) {
    memset(s->g3d.color, 0, sizeof s->g3d.color);
    for (int y = 0; y < 192; y++) for (int x = 0; x < 256; x++) {
        u32 v = 0;
        if (x < 128) v = g3d_px(40, 20, 60, 31);                    /* opaque (20, 10, 30) */
        if (x >= 64 && x < 72) v = g3d_px(40, 20, 60, 5);           /* translucent: still a pixel */
        s->g3d.color[y * 256 + x] = v;
    }
}

static void test_source_a_3d(void) {
    int bad;
    /* source A = the 3D screen (bit 24): the 3D layer by itself, whatever DISPCNT says; clear alpha is a
     * transparent black pixel */
    fresh();
    poke_3d();
    io32(DISPCAPCNT, CAP(0, 0, 1, 0, SZ256x192, 1, 0, 0, SRC_A));
    frame();
    bad = 0;
    for (int y = 0; y < 192; y++) for (int x = 0; x < 256; x++) {
        u16 want = x < 128 ? (u16)(OPAQUE | A_COL) : 0;
        bad += bank(1)[y * 256 + x] != want;
    }
    CHECK(!bad, "3D alone, with 3D and BG0 off in DISPCNT: %d pixels wrong (first %04x, %04x)", bad, bank(1)[0], bank(1)[255]);
    CHECK(bank(1)[65] == (OPAQUE | A_COL), "3D pixel of alpha 5 is opaque in the capture: %04x", bank(1)[65]);
    /* source A = the graphics screen (bit 24 = 0) with 3D as BG0: the layer over the backdrop */
    fresh();
    poke_3d();
    io32(0, 1u << 16 | 8 | 0x100);
    io32(DISPCAPCNT, CAP(0, 0, 1, 0, SZ256x192, 0, 0, 0, SRC_A));
    frame();
    CHECK(bank(1)[0] == (OPAQUE | A_COL), "graphics with 3D as BG0: layer pixel %04x", bank(1)[0]);
    CHECK(bank(1)[200] == (OPAQUE | A_COL), "graphics with 3D as BG0: behind a transparent pixel is the backdrop: %04x", bank(1)[200]);
    /* engine A's 3D layer both captured and shown: palette 0 differs from the 3D colour so each is told apart */
    fresh();
    poke_3d();
    wr16(&s->arm9, 0x05000000, RGB(1, 2, 3));
    io32(0, 1u << 16 | 8 | 0x100);
    io32(DISPCAPCNT, CAP(0, 0, 1, 0, SZ256x192, 0, 0, 0, SRC_A));
    frame();
    CHECK(bank(1)[10] == (OPAQUE | A_COL) && bank(1)[200] == (OPAQUE | RGB(1, 2, 3)), "3D over the backdrop in the capture: %04x %04x", bank(1)[10], bank(1)[200]);
    CHECK(screen(10, 0) == argb(A_COL) && screen(200, 0) == argb(RGB(1, 2, 3)), "and on the screen: %08x %08x", screen(10, 0), screen(200, 0));
    /* graphics without BG0 on: the backdrop everywhere */
    fresh();
    poke_3d();
    io32(DISPCAPCNT, CAP(0, 0, 1, 0, SZ256x192, 0, 0, 0, SRC_A));
    frame();
    CHECK(bank(1)[10] == (OPAQUE | A_COL) && bank(1)[200] == (OPAQUE | A_COL), "3D off: backdrop");
    /* A (3D) blended with B: where the 3D pixel is transparent only B counts, and gives the alpha */
    fresh();
    poke_3d();
    fill(0, (u16)(OPAQUE | RGB(10, 31, 4)));
    io32(DISPCAPCNT, CAP(8, 8, 1, 0, SZ256x192, 1, 0, 0, SRC_BLEND));
    frame();
    CHECK(bank(1)[10] == (OPAQUE | RGB(15, 20, 17)), "3D + B where the 3D layer is drawn: %04x", bank(1)[10]);
    CHECK(bank(1)[200] == (OPAQUE | RGB(5, 15, 2)), "3D + B where it is not: B's half: %04x", bank(1)[200]);
    fill(0, (u16)RGB(10, 31, 4));
    io32(DISPCAPCNT, CAP(8, 8, 1, 0, SZ256x192, 1, 0, 0, SRC_BLEND));
    frame();
    CHECK(bank(1)[200] == 0, "neither has alpha: transparent black: %04x", bank(1)[200]);
}

/* ---- timing: when it runs, when the busy bit clears -------------------------------------------- */
#define RAM(off) (0x02000000u + (off))
#define TABLE RAM(0x100000)                 /* DMA writers' values */
#define PROBE RAM(0x110000)                 /* what the probes read */
static u32 *ram32(u32 addr) { return (u32 *)(s->ram + (addr & 0x3FFFFF)); }
static void dma(int ch, u32 src, u32 dst, u32 cnt) {
    io32(0xB0 + 12u * (u32)ch, src); io32(0xB4 + 12u * (u32)ch, dst); io32(0xB8 + 12u * (u32)ch, cnt);
}
enum { REPEAT = 1u << 25, WORD = 1u << 26, EN = 1u << 31, DST_FIX = 2u << 21, SRC_FIX = 2u << 23, T_VBLANK = 1u << 27, T_HBLANK = 2u << 27, T_MMEM = 4u << 27 };

/* At the end of each visible line (HBlank DMA) copy the next word of a table into a register, and at the
 * start of line 192 (VBlank DMA) copy a register to RAM */
static void line_writer(int ch, u32 reg_off) { dma(ch, TABLE, IO + reg_off, 1 | DST_FIX | REPEAT | WORD | T_HBLANK | EN); }
static void line_probe(int ch, u32 reg_off) { dma(ch, IO + reg_off, PROBE, 1 | SRC_FIX | REPEAT | WORD | T_HBLANK | EN); }
static void vblank_probe(int ch, u32 reg_off) { dma(ch, IO + reg_off, PROBE + 4 * 256, 1 | SRC_FIX | DST_FIX | WORD | T_VBLANK | EN); }
static void dma_off(void) { for (int ch = 0; ch < 4; ch++) io32(0xB8 + 12u * (u32)ch, 0); }

static void test_busy_timing(void) {
    /* enabled before the frame: busy through line 191 (read at the end of each line), cleared by line 192
     * whatever the size (a 128x128 capture is finished in line 128) */
    for (int k = SZ128; k <= SZ256x192; k += 3) {
        fresh();
        fill(1, 0x1357);
        io32(DISPCAPCNT, CAP(0, 0, 1, 0, k, 0, 0, 0, SRC_A));
        line_probe(1, DISPCAPCNT);
        vblank_probe(2, DISPCAPCNT);
        frame();
        int busy_lines = 0;
        for (int l = 0; l < 192; l++) busy_lines += (*ram32(PROBE + 4u * (u32)l) & BUSY) != 0;
        CHECK(busy_lines == 192, "size %d: busy at the end of %d of 192 lines", k, busy_lines);
        CHECK(!(*ram32(PROBE + 4 * 256) & BUSY), "size %d: still busy at line 192", k);
        CHECK(!(get32(DISPCAPCNT) & BUSY), "size %d: busy after the frame", k);
        CHECK(bank(1)[0] == (A_COL | OPAQUE), "size %d: captured", k);
        /* done: another frame doesn't capture again unless enabled again */
        dma_off();
        fill(1, 0x1357);
        frame();
        CHECK(bank(1)[0] == 0x1357 && !(get32(DISPCAPCNT) & BUSY), "size %d: no second capture", k);
    }
    /* enabled in the middle of a frame (written at the end of line 99): the capture starts at the next line 0,
     * so this frame writes nothing and the bit stays set, and the next frame captures all lines */
    fresh();
    fill(1, 0x1357);
    for (int l = 0; l < 192; l++) *ram32(TABLE + 4u * (u32)l) = l < 99 ? CAP(0, 0, 1, 0, SZ256x192, 0, 0, 0, SRC_A) & ~BUSY : CAP(0, 0, 1, 0, SZ256x192, 0, 0, 0, SRC_A);
    line_writer(1, DISPCAPCNT);
    vblank_probe(2, DISPCAPCNT);
    frame();
    int touched = 0;
    for (u32 i = 0; i < 256 * 192; i++) touched += bank(1)[i] != 0x1357;
    CHECK(!touched, "enabled mid-frame: %d pixels written in that frame", touched);
    CHECK((get32(DISPCAPCNT) & BUSY) && (*ram32(PROBE + 4 * 256) & BUSY), "enabled mid-frame: still busy at line 192 and after");
    dma_off();
    frame();
    int bad = 0;
    for (u32 i = 0; i < 256 * 192; i++) bad += bank(1)[i] != (A_COL | OPAQUE);
    CHECK(!bad && !(get32(DISPCAPCNT) & BUSY), "the frame after: %d wrong, busy %08x", bad, get32(DISPCAPCNT));
    /* cleared in the middle of a capture (end of line 49): lines 0-49 are done, the rest never is */
    fresh();
    fill(1, 0x1357);
    for (int l = 0; l < 192; l++) *ram32(TABLE + 4u * (u32)l) = l < 49 ? CAP(0, 0, 1, 0, SZ256x192, 0, 0, 0, SRC_A) : 0;
    io32(DISPCAPCNT, CAP(0, 0, 1, 0, SZ256x192, 0, 0, 0, SRC_A));
    line_writer(1, DISPCAPCNT);
    frame();
    int done = 0, left = 0;
    for (u32 i = 0; i < 256 * 192; i++) { if (bank(1)[i] == (A_COL | OPAQUE)) done++; else if (bank(1)[i] == 0x1357) left++; }
    CHECK(done == 50 * 256 && left == 142 * 256, "stopped after line 49: %d pixels captured, %d untouched", done, left);
    CHECK(!(get32(DISPCAPCNT) & BUSY), "stopped: not busy");
    /* the register is latched at line 0: a write of other fields while it runs changes nothing (the enable
     * bit stays set in the writes); the new fields apply to the next capture */
    fresh();
    fill(1, 0x1357); fill(2, 0x1357);
    for (int l = 0; l < 192; l++) *ram32(TABLE + 4u * (u32)l) = l < 20 ? CAP(0, 0, 1, 0, SZ256x192, 0, 0, 0, SRC_A) : CAP(0, 0, 2, 0, SZ256x192, 0, 0, 0, SRC_A);
    io32(DISPCAPCNT, CAP(0, 0, 1, 0, SZ256x192, 0, 0, 0, SRC_A));
    line_writer(1, DISPCAPCNT);
    frame();
    CHECK(bank(1)[192 * 256 - 1] == (A_COL | OPAQUE) && bank(2)[0] == 0x1357, "fields latched at line 0: %04x %04x", bank(1)[192 * 256 - 1], bank(2)[0]);
}

/* Black 2's own register value, 0xC033040C: blend (EVA 12, EVB 4) the graphics screen with VRAM, bank D,
 * 256x192, VRAM read block from DISPCNT. With the screen in VRAM display mode of bank D that is a motion
 * blur through a single bank (GBATEK's example): the screen shows the previous capture, the new one is the
 * graphics at 12/16 and the previous capture at 4/16. */
static void test_feedback(void) {
    fresh();
    io32(0, 2u << 16 | 3u << 18);                   /* VRAM display, bank D */
    fill(3, (u16)(OPAQUE | RGB(4, 8, 12)));
    io32(DISPCAPCNT, 0xC033040Cu);
    frame();
    CHECK(screen(10, 10) == argb(RGB(4, 8, 12)) && screen(255, 191) == argb(RGB(4, 8, 12)), "frame 1 shows the old bank: %08x", screen(10, 10));
    u32 d1 = OPAQUE | RGB((20 * 12 + 4 * 4) >> 4, (10 * 12 + 8 * 4) >> 4, (30 * 12 + 12 * 4) >> 4);     /* (16, 9, 25) */
    CHECK(bank(3)[0] == d1 && bank(3)[256 * 192 - 1] == d1 && bank(3)[256 * 192] == (OPAQUE | RGB(4, 8, 12)), "frame 1's capture: %04x, expected %04x", bank(3)[0], d1);
    io32(DISPCAPCNT, 0xC033040Cu);
    frame();
    CHECK(screen(10, 10) == argb(d1), "frame 2 shows frame 1's capture, the display read it before the capture rewrote it: %08x", screen(10, 10));
    u32 d2 = OPAQUE | RGB((20 * 12 + 16 * 4) >> 4, (10 * 12 + 9 * 4) >> 4, (30 * 12 + 25 * 4) >> 4);   /* (19, 9, 28) */
    CHECK(bank(3)[0] == d2 && bank(3)[256 * 192 - 1] == d2, "frame 2's capture: %04x, expected %04x", bank(3)[0], d2);
}

/* ---- main memory display ---------------------------------------------------------------------- */
static u16 mm_pix(u32 i) { return (u16)((i * 5 + 1) & 0x7FFF); }        /* bit 15 stays clear: it is unused */

static void mm_setup(void) {
    for (u32 i = 0; i < 256 * 192; i++) memcpy(s->ram + 0x100000 + 2 * i, &(u16){mm_pix(i)}, 2);
}
static void mm_dma(void) { dma(0, RAM(0x100000), IO + 0x68, 4 | DST_FIX | REPEAT | WORD | T_MMEM | EN); }

static void test_main_memory_display(void) {
    /* DMA in main memory display timing, 4 words a burst to DISP_MMEM_FIFO; DISPCNT mode 3 shows the stream */
    fresh();
    mm_setup(); mm_dma();
    io32(0, 3u << 16);
    frame();
    int bad = 0;
    for (int y = 0; y < 192; y++) for (int x = 0; x < 256; x++) bad += screen(x, y) != argb(mm_pix((u32)(y * 256 + x)));
    CHECK(!bad, "main memory display: %d pixels wrong", bad);
    /* a capture takes the same stream as source B (bit 25); its pixels count as opaque */
    fresh();
    mm_setup(); mm_dma();
    io32(0, 1u << 16);
    io32(DISPCAPCNT, CAP(0, 0, 1, 0, SZ256x192, 0, 1, 0, SRC_B));
    frame();
    bad = 0;
    for (u32 i = 0; i < 256 * 192; i++) bad += bank(1)[i] != (OPAQUE | mm_pix(i));
    CHECK(!bad, "capture of the FIFO: %d pixels wrong", bad);
    /* blended with the graphics screen: A (20,10,30) at 8/16, B at 8/16 */
    fresh();
    mm_setup(); mm_dma();
    io32(DISPCAPCNT, CAP(8, 8, 1, 0, SZ256x192, 0, 1, 0, SRC_BLEND));
    frame();
    bad = 0;
    for (u32 i = 0; i < 256 * 192; i++) {
        u32 b = mm_pix(i);
        u32 want = OPAQUE | RGB((20 * 8 + (b & 31) * 8) >> 4, (10 * 8 + ((b >> 5) & 31) * 8) >> 4, (30 * 8 + ((b >> 10) & 31) * 8) >> 4);
        bad += bank(1)[i] != want;
    }
    CHECK(!bad, "A blended with the FIFO: %d pixels wrong", bad);
    /* the display and the capture share the stream: both get every pixel */
    fresh();
    mm_setup(); mm_dma();
    io32(0, 3u << 16);
    io32(DISPCAPCNT, CAP(0, 0, 1, 0, SZ256x64, 0, 1, 0, SRC_B));
    frame();
    CHECK(screen(7, 63) == argb(mm_pix(63 * 256 + 7)) && bank(1)[63 * 256 + 7] == (OPAQUE | mm_pix(63 * 256 + 7)), "one stream for the display and the capture");
    /* a DMA enabled in the middle of a frame starts at the next frame (GBATEK): this one is blank, the next
     * is the picture from the first pixel (nothing was sent) */
    fresh();
    mm_setup();
    io32(0, 3u << 16);
    for (int l = 0; l < 192; l++) *ram32(TABLE + 4u * (u32)l) = l < 99 ? 0 : (4 | DST_FIX | REPEAT | WORD | T_MMEM | EN);
    dma(0, RAM(0x100000), IO + 0x68, 0);
    dma(1, TABLE, IO + 0xB8, 1 | DST_FIX | REPEAT | WORD | T_HBLANK | EN);
    frame();
    CHECK(screen(10, 10) == argb(0) && screen(10, 150) == argb(0), "DMA enabled in mid-frame: this frame is blank: %08x %08x", screen(10, 10), screen(10, 150));
    io32(0xB8 + 12, 0);                             /* the writer off, channel 0 stays on */
    frame();
    CHECK(screen(10, 10) == argb(mm_pix(10 * 256 + 10)) && screen(255, 191) == argb(mm_pix(256 * 192 - 1)), "and the next frame shows it: %08x", screen(10, 10));
}

int main(void) {
    test_register();
    test_sizes();
    test_write_offset();
    test_source_b_vram();
    test_source_a_graphics();
    test_blend();
    test_source_a_3d();
    test_busy_timing();
    test_feedback();
    test_main_memory_display();
    if (s) nds_shutdown(s);
    if (!fails) printf("display capture: register, sizes, offsets, sources, blend, 3D layer, timing, main memory display\n");
    return fails != 0;
}
