/* The 2D engines (gpu2d.c) against GBATEK ("DS Video", "LCD I/O Window Feature", "Mosaic Function",
 * "DS Window Glitches", "DS Video OBJs", "DS Video Extended Palettes"): lines are rendered from VRAM,
 * palettes and OAM written through the ARM9's bus, and checked pixel by pixel against values worked
 * out by hand, never against a recorded run. */
#include <stdio.h>
#include <string.h>
#include "nds.h"

static Nds *s;
static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("%s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define IO 0x04000000u
enum { RED = 0x001F, GREEN = 0x03E0, BLUE = 0x7C00, YELLOW = 0x03FF, BACKDROP = 0x5555, WHITE = 0x7FFF };
static const u16 BGCOL[4] = {RED, GREEN, BLUE, YELLOW};

static void io16(u32 off, u32 v) { wr16(&s->arm9, IO + off, v); }
static void io32(u32 off, u32 v) { wr32(&s->arm9, IO + off, v); }
static void pal(int engine, u32 i, u16 c) { wr16(&s->arm9, 0x05000000u + (u32)engine * 0x400 + i * 2, c); }
static void oam(int engine, int i, u32 a0, u32 a1, u32 a2) {
    u32 a = 0x07000000u + (u32)engine * 0x400 + (u32)i * 8;
    wr16(&s->arm9, a, a0); wr16(&s->arm9, a + 2, a1); wr16(&s->arm9, a + 4, a2);
}
static void hide_oam(int engine) { for (int i = 0; i < 128; i++) oam(engine, i, 0x200, 0, 0); }
/* VRAM as an engine sees it (bg or obj region), through the banks' current mapping */
static void v8(int engine, int obj, u32 off, u32 v) { u8 *p = vram_engine_ptr(s, engine, obj, off); if (p) *p = (u8)v; }
static void v16(int engine, int obj, u32 off, u32 v) { v8(engine, obj, off, v & 0xFF); v8(engine, obj, off + 1, v >> 8); }

static u32 argb(u32 c) {                /* BGR555 as the screen shows it */
    u32 r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return 0xFF000000u | (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
}
static u32 px(int engine, int x, int y) {
    u32 line[SCREEN_W];
    gpu2d_render_line(s, engine, y, line);
    return line[x];
}
#define PX(x, y) px(0, x, y)

static void fresh(void) {
    static u8 rom[0x1000];
    if (s) nds_shutdown(s);
    s = nds_create(rom, sizeof rom);
    s->powcnt1 = 0x820F;                /* nds_create leaves the screens off: the boot value */
    wr8(&s->arm9, IO + 0x240, 0x81);    /* A: engine A BG */
    wr8(&s->arm9, IO + 0x241, 0x82);    /* B: engine A OBJ */
    wr8(&s->arm9, IO + 0x242, 0x84);    /* C: engine B BG */
    wr8(&s->arm9, IO + 0x243, 0x84);    /* D: engine B OBJ */
    hide_oam(0); hide_oam(1);
    pal(0, 0, BACKDROP); pal(1, 0, BACKDROP);
}

/* graphics display of engine A with these DISPCNT bits (layers, windows, mappings) */
static void show(u32 bits) { io32(0, 1u << 16 | bits); }

/* text BG n of engine A: one 256-colour tile of palette index n+1 over the whole 256x256 map, at
 * char block 4+n and screen block n */
static void solid_bg(int n, int prio) {
    u32 cb = 0x10000u + 0x4000u * (u32)n;
    for (u32 i = 0; i < 64; i++) v8(0, 0, cb + i, (u32)n + 1);
    pal(0, (u32)n + 1, BGCOL[n]);
    io16(8 + 2 * (u32)n, (u32)prio | (4u + (u32)n) << 2 | 0x80 | (u32)n << 8);
}

/* ---- windows ------------------------------------------------------------------------------- */
enum { WIN0H = 0x40, WIN1H = 0x42, WIN0V = 0x44, WIN1V = 0x46, WININ = 0x48, WINOUT = 0x4A, MOSAIC = 0x4C,
       BLDCNT = 0x50, BLDALPHA = 0x52, BLDY = 0x54 };
#define WIN_ON(n) (0x2000u << (n))      /* DISPCNT 13, 14, 15 */
#define BGS 0xF00u

static void four_bgs(void) { fresh(); for (int n = 0; n < 4; n++) solid_bg(n, 0); }

static void test_window_rect(void) {
    four_bgs();
    /* window 0 over x 64..191, y 32..159 (the far edges are exclusive): inside only BG1, outside only BG0 */
    io16(WIN0H, 64 << 8 | 192); io16(WIN0V, 32 << 8 | 160); io16(WININ, 0x02); io16(WINOUT, 0x01);
    show(BGS | WIN_ON(0));
    CHECK(PX(100, 100) == argb(GREEN), "inside shows BG1: %08x", PX(100, 100));
    CHECK(PX(64, 32) == argb(GREEN) && PX(191, 159) == argb(GREEN), "the window's corners are inside");
    CHECK(PX(63, 100) == argb(RED) && PX(192, 100) == argb(RED), "x just outside shows BG0");
    CHECK(PX(100, 31) == argb(RED) && PX(100, 160) == argb(RED), "y just outside shows BG0");
    /* the outside region exists as soon as any window is on, and the layer enables of DISPCNT still apply */
    show(0x200 | WIN_ON(0));            /* only BG1 enabled */
    CHECK(PX(10, 10) == argb(BACKDROP) && PX(100, 100) == argb(GREEN), "DISPCNT enables and the window mask both apply");
    /* no window on: all layers, regardless of the registers */
    show(BGS);
    CHECK(PX(100, 100) == argb(RED), "no window: BG0 in front");
}

static void test_window_edges(void) {
    four_bgs();
    io16(WININ, 0x02); io16(WINOUT, 0x01);
    /* X2 = 0 means 256 (GBATEK, DS Window Glitches) */
    io16(WIN0H, 64 << 8); io16(WIN0V, 0 << 8 | 192);
    show(BGS | WIN_ON(0));
    CHECK(PX(63, 50) == argb(RED) && PX(64, 50) == argb(GREEN) && PX(255, 50) == argb(GREEN), "X2 = 0 reaches the right edge");
    /* X1 = X2 = 0: no window at all */
    io16(WIN0H, 0);
    CHECK(PX(0, 50) == argb(RED) && PX(100, 50) == argb(RED) && PX(255, 50) == argb(RED), "X1 = X2 = 0 is no window");
    /* X1 = 0 starts at the left edge */
    io16(WIN0H, 0 << 8 | 10);
    CHECK(PX(0, 50) == argb(GREEN) && PX(9, 50) == argb(GREEN) && PX(10, 50) == argb(RED), "X1 = 0");
    /* X1 above X2: the window reaches the right edge */
    io16(WIN0H, 200 << 8 | 100);
    CHECK(PX(199, 50) == argb(RED) && PX(200, 50) == argb(GREEN) && PX(255, 50) == argb(GREEN), "X1 > X2");
    /* Y2 over 192, and Y1 above Y2: the window reaches the last line */
    io16(WIN0H, 0 << 8 | 0x80);
    io16(WIN0V, 150 << 8 | 250);
    CHECK(PX(10, 149) == argb(RED) && PX(10, 150) == argb(GREEN) && PX(10, 191) == argb(GREEN), "Y2 > 192");
    io16(WIN0V, 100 << 8 | 50);
    CHECK(PX(10, 99) == argb(RED) && PX(10, 100) == argb(GREEN) && PX(10, 191) == argb(GREEN), "Y1 > Y2");
    /* the DS compares 8 bits of a 0..262 line counter: Y1 of 1..6 fires in lines 257..262, so the window
     * is already on in line 0 and stays on until Y2 */
    io16(WIN0V, 3 << 8 | 10);
    CHECK(PX(10, 0) == argb(GREEN) && PX(10, 2) == argb(GREEN) && PX(10, 9) == argb(GREEN) && PX(10, 10) == argb(RED), "Y1 = 3 glitch");
    io16(WIN0V, 7 << 8 | 10);
    CHECK(PX(10, 0) == argb(RED) && PX(10, 6) == argb(RED) && PX(10, 7) == argb(GREEN) && PX(10, 9) == argb(GREEN) && PX(10, 10) == argb(RED),
          "Y1 = 7 is outside the glitch");
    io16(WIN0V, 0 << 8 | 10);
    CHECK(PX(10, 0) == argb(GREEN) && PX(10, 9) == argb(GREEN) && PX(10, 10) == argb(RED), "Y1 = 0");
    /* a window that is off in DISPCNT does nothing, whatever its registers say */
    show(BGS);
    CHECK(PX(10, 5) == argb(RED), "window 0 disabled");
}

static void test_window_priority(void) {
    four_bgs();
    /* window 0 (x, y 50..149) shows BG2, window 1 (100..199) BG3, outside BG0; they overlap in 100..149 */
    io16(WIN0H, 50 << 8 | 150); io16(WIN0V, 50 << 8 | 150);
    io16(WIN1H, 100 << 8 | 200); io16(WIN1V, 100 << 8 | 200);
    io16(WININ, 0x08 << 8 | 0x04); io16(WINOUT, 0x01);
    show(BGS | WIN_ON(0) | WIN_ON(1));
    CHECK(PX(125, 125) == argb(BLUE), "window 0 is in front of window 1: %08x", PX(125, 125));
    CHECK(PX(175, 125) == argb(YELLOW), "window 1 alone");
    CHECK(PX(75, 75) == argb(BLUE) && PX(10, 10) == argb(RED), "window 0 alone, outside");
    show(BGS | WIN_ON(1));              /* window 0 off: window 1 where they overlap */
    CHECK(PX(125, 125) == argb(YELLOW) && PX(75, 75) == argb(RED), "window 1 without window 0");
}

static void test_window_obj(void) {
    four_bgs();
    /* an 8x8 sprite in OBJ window mode (attribute 0 bits 10-11 = 2) at (100, 100): its opaque pixels are the
     * OBJ window, which is itself invisible; inside it WINOUT's high byte applies (BG1), outside the low (BG0) */
    for (u32 i = 0; i < 32; i++) v8(0, 1, i, 0x11);
    oam(0, 0, 100 | 2u << 10, 100, 0);
    io16(WINOUT, 0x0200 | 0x01);
    show(BGS | 1u << 12 | WIN_ON(2));
    CHECK(PX(104, 104) == argb(GREEN) && PX(100, 100) == argb(GREEN) && PX(107, 107) == argb(GREEN), "inside the OBJ window: %08x", PX(104, 104));
    CHECK(PX(99, 104) == argb(RED) && PX(108, 104) == argb(RED) && PX(104, 108) == argb(RED), "outside it");
    /* it needs both DISPCNT bit 12 (OBJ) and bit 15 (OBJ window) */
    show(BGS | 1u << 12);
    CHECK(PX(104, 104) == argb(RED), "OBJ window sprites are off without DISPCNT.15");
    show(BGS | WIN_ON(2));
    CHECK(PX(104, 104) == argb(RED), "no OBJ window without DISPCNT.12: only the outside region is left");
    /* transparent pixels of the sprite are no window: colour 0 of a 4 bpp tile */
    for (u32 i = 0; i < 32; i++) v8(0, 1, i, 0x10);     /* left pixel of each pair transparent */
    show(BGS | 1u << 12 | WIN_ON(2));
    CHECK(PX(100, 100) == argb(RED) && PX(101, 100) == argb(GREEN) && PX(102, 100) == argb(RED) && PX(103, 100) == argb(GREEN), "only opaque pixels count");
    /* window 0 is in front of the OBJ window */
    io16(WIN0H, 90 << 8 | 110); io16(WIN0V, 90 << 8 | 110); io16(WININ, 0x04);
    show(BGS | 1u << 12 | WIN_ON(0) | WIN_ON(2));
    CHECK(PX(101, 100) == argb(BLUE), "window 0 over the OBJ window: %08x", PX(101, 100));
}

static void test_window_effects(void) {
    four_bgs();
    /* BG0 is the first target of "brightness increase" by 16/16: white wherever effects are on. Window 0
     * (x 64..191) has effects on in WININ bit 5; the outside (WINOUT bit 5 clear) has them off. */
    io16(BLDCNT, 0x01 | 2 << 6); io16(BLDY, 16);
    io16(WIN0H, 64 << 8 | 192); io16(WIN0V, 0 << 8 | 192);
    io16(WININ, 0x01 | 0x20); io16(WINOUT, 0x01);
    show(BGS | WIN_ON(0));
    CHECK(PX(100, 50) == argb(WHITE), "effects on inside: %08x", PX(100, 50));
    CHECK(PX(10, 50) == argb(RED), "effects off outside: %08x", PX(10, 50));
    io16(WININ, 0x01);
    CHECK(PX(100, 50) == argb(RED), "WININ bit 5 clear");
    io16(WINOUT, 0x01 | 0x20);
    CHECK(PX(10, 50) == argb(WHITE), "WINOUT bit 5 set");
    show(BGS);                          /* without windows effects are on everywhere */
    CHECK(PX(10, 50) == argb(WHITE) && PX(100, 50) == argb(WHITE), "no window: effects everywhere");
}

/* ---- mosaic -------------------------------------------------------------------------------- */
/* text BG0 over a map of tile 1, whose pixels are palette entries 1..64 left to right, top to bottom
 * (entry i is the colour i): the colour on screen says which source pixel was used */
static int pat_idx(int x, int y) { return (y & 7) * 8 + (x & 7) + 1; }

static void patterned_bg(void) {
    fresh();
    for (u32 i = 0; i < 64; i++) { v8(0, 0, 0x4000 + 64 + i, i + 1); pal(0, i + 1, (u16)(i + 1)); }
    for (u32 i = 0; i < 1024; i++) v16(0, 0, 0x800 + i * 2, 1);
    io16(8, 1u << 2 | 0x80 | 1u << 8);              /* char block 1, 256 colours, screen block 1 */
}

static void test_mosaic_bg(void) {
    patterned_bg();
    show(0x100);
    int bad = 0;
    for (int y = 0; y < 24; y++) for (int x = 0; x < 40; x++) bad += PX(x, y) != argb((u32)pat_idx(x, y));
    CHECK(!bad, "baseline: %d pixels differ", bad);
    /* BG mosaic 4 wide, 3 tall: every pixel of a block shows the block's upper left (BGxCNT.6 enables it) */
    io16(MOSAIC, 3 | 2 << 4);
    io16(8, 1u << 2 | 0x80 | 1u << 8 | 0x40);
    bad = 0;
    for (int y = 0; y < 24; y++) for (int x = 0; x < 40; x++) {
        u32 want = argb((u32)pat_idx(x - x % 4, y - y % 3));
        if (PX(x, y) != want) { if (!bad++) printf("  first at (%d,%d): %08x, expected %08x\n", x, y, PX(x, y), want); }
    }
    CHECK(!bad, "4x3 mosaic: %d pixels differ", bad);
    /* the BG's own enable bit decides: with it clear the MOSAIC register does nothing */
    io16(8, 1u << 2 | 0x80 | 1u << 8);
    CHECK(PX(5, 5) == argb((u32)pat_idx(5, 5)), "mosaic off in BGxCNT");
    /* horizontal only, and the vertical size of 1 leaves lines alone */
    io16(8, 1u << 2 | 0x80 | 1u << 8 | 0x40);
    io16(MOSAIC, 7);
    CHECK(PX(13, 5) == argb((u32)pat_idx(8, 5)) && PX(7, 6) == argb((u32)pat_idx(0, 6)), "8x1 mosaic");
    /* all zero: effectively off even when enabled */
    io16(MOSAIC, 0);
    CHECK(PX(5, 5) == argb((u32)pat_idx(5, 5)), "MOSAIC = 0");
    /* the OBJ fields (bits 8-15) don't touch BGs */
    io16(MOSAIC, 0xFF00);
    CHECK(PX(5, 5) == argb((u32)pat_idx(5, 5)), "OBJ mosaic size leaves the BG alone");
}

static void test_mosaic_obj(void) {
    fresh();
    for (u32 i = 0; i < 64; i++) { v8(0, 1, i, i + 1); pal(0, 0x100 + i + 1, (u16)(i + 1)); }
    /* an 8x8 8 bpp sprite at (21, 30), mosaic 4 wide (OBJ H = 3) and 2 tall (V = 1); blocks are aligned to the
     * screen (x multiples of 4, even lines), not to the sprite */
    oam(0, 0, 30 | 0x2000 | 0x1000, 21, 0);
    io16(MOSAIC, 3 << 8 | 1 << 12);
    show(1u << 12);
    int bad = 0;
    for (int y = 30; y < 38; y++) for (int x = 24; x <= 28; x++) {
        int sx = x - x % 4 - 21, sy = y - 30 - y % 2;
        if (PX(x, y) != argb((u32)(sy * 8 + sx + 1))) bad++;
    }
    CHECK(!bad, "OBJ mosaic: %d pixels differ", bad);
    /* the sprite's own mosaic flag (attribute 0 bit 12) decides */
    oam(0, 0, 30 | 0x2000, 21, 0);
    CHECK(PX(26, 31) == argb((u32)((1) * 8 + (26 - 21) + 1)), "no mosaic flag: %08x", PX(26, 31));
    /* and the BG fields of MOSAIC don't touch it */
    oam(0, 0, 30 | 0x2000 | 0x1000, 21, 0);
    io16(MOSAIC, 0x00FF);
    CHECK(PX(26, 31) == argb((u32)((1) * 8 + (26 - 21) + 1)), "BG mosaic size leaves the OBJ alone");
}

/* ---- bitmap OBJs ---------------------------------------------------------------------------- */
/* the source bitmap's pixel (tx, ty) of a 16x16 sprite: opaque, a different colour in each */
static u32 bmp_col(int tx, int ty) { return 0x8000u | (u32)((ty * 16 + tx) * 3 + 1); }

static void bmp_sprite(u32 tile, u32 alpha) { oam(0, 0, 50 | 3u << 10, 60 | 1u << 14, tile | alpha << 12); }

static int check_bmp(u32 base, u32 stride) {
    int bad = 0;
    for (int ty = 0; ty < 16; ty++) for (int tx = 0; tx < 16; tx++) v16(0, 1, base + (u32)ty * stride + (u32)tx * 2, bmp_col(tx, ty));
    for (int ty = 0; ty < 16; ty++) for (int tx = 0; tx < 16; tx++) bad += PX(60 + tx, 50 + ty) != argb(bmp_col(tx, ty));
    return bad;
}

static void test_bitmap_obj(void) {
    fresh();
    /* 1D mapping (DISPCNT.6), 128-byte boundary: tile 2 is at 256, rows as wide as the sprite */
    bmp_sprite(2, 15);
    show(1u << 12 | 1u << 6);
    CHECK(!check_bmp(2 * 128, 32), "1D, boundary 128");
    /* DISPCNT.22 makes the boundary 256 bytes (engine A only) */
    fresh();
    bmp_sprite(2, 15);
    show(1u << 12 | 1u << 6 | 1u << 22);
    CHECK(!check_bmp(2 * 256, 32), "1D, boundary 256");
    /* 2D mapping, source bitmap 128 pixels wide (DISPCNT.5 = 0): the tile number is y index * 16 + x index;
     * 2D_BitmapVramAddress = (tile & MaskX) * 0x10 + (tile & ~MaskX) * 0x80, rows 256 bytes apart */
    fresh();
    bmp_sprite(0x11, 15);
    show(1u << 12);
    CHECK(!check_bmp(0x10 + 0x10 * 0x80, 256), "2D, 128 wide");
    /* 256 wide (DISPCNT.5): MaskX is 1Fh, rows 512 bytes apart */
    fresh();
    bmp_sprite(0x21, 15);
    show(1u << 12 | 1u << 5);
    CHECK(!check_bmp(0x10 + 0x20 * 0x80, 512), "2D, 256 wide");
    /* bit 15 of a source pixel is its alpha: clear is transparent */
    fresh();
    bmp_sprite(2, 15);
    show(1u << 12 | 1u << 6);
    v16(0, 1, 2 * 128 + 4, 0x03E0);                         /* pixel (2, 0) without alpha */
    v16(0, 1, 2 * 128 + 6, 0x83E0);
    CHECK(PX(62, 50) == argb(BACKDROP) && PX(63, 50) == argb(GREEN), "alpha bit: %08x %08x", PX(62, 50), PX(63, 50));
    /* a sprite behind a BG of lower priority number is hidden: the OBJ's attribute 2 priority bits (10-11) are
     * not part of the alpha (12-15) */
    fresh();
    for (u32 i = 0; i < 64; i++) v8(0, 0, 0x10000 + i, 1);
    pal(0, 1, RED);
    io16(8, 0 | 4u << 2 | 0x80);                            /* BG0 priority 0, char block 4, map block 0 (all tile 0) */
    oam(0, 0, 50 | 3u << 10, 60 | 1u << 14, 2 | 1u << 10 | 15u << 12);      /* OBJ priority 1: behind BG0 */
    for (int i = 0; i < 16; i++) v16(0, 1, 2 * 128 + (u32)i * 2, 0x83E0);
    show(1u << 12 | 1u << 6 | 0x100);
    CHECK(PX(60, 50) == argb(RED), "BG0 (priority 0) in front of the OBJ (priority 1): %08x", PX(60, 50));
}

/* The alpha of a bitmap OBJ is in attribute 2 bits 12-15. GBATEK gives no formula; EVA = (alpha + 1) / 16
 * against the pixel behind is assumed here, so 15 is opaque. */
static void test_bitmap_obj_alpha(void) {
    fresh();
    for (u32 i = 0; i < 64; i++) v8(0, 0, 0x10000 + i, 1);
    pal(0, 1, RED);
    io16(8, 1 | 4u << 2 | 0x80);
    for (int i = 0; i < 16; i++) v16(0, 1, 2 * 128 + (u32)i * 2, 0x83E0);       /* green */
    bmp_sprite(2, 15);
    io16(BLDCNT, 0x100);                                    /* BG0 is a second target */
    show(1u << 12 | 1u << 6 | 0x100);
    CHECK(PX(60, 50) == argb(GREEN), "alpha 15 is opaque: %08x", PX(60, 50));
    bmp_sprite(2, 7);                                       /* 8/16 of green over 8/16 of red */
    CHECK(PX(60, 50) == argb(15 | 15 << 5), "alpha 7 blends half and half: %08x", PX(60, 50));
    io16(BLDCNT, 0);                                        /* the layer behind is no second target: no blending */
    CHECK(PX(60, 50) == argb(GREEN), "no blending without a second target: %08x", PX(60, 50));
}

/* ---- the large 512x1024 bitmap ------------------------------------------------------------- */
static void map_all_bg(void) {          /* banks A-D as engine A's BG VRAM in order: 512 KB */
    for (u32 b = 0; b < 4; b++) wr8(&s->arm9, IO + 0x240 + b, 0x81 | b << 3);
}

static void test_large_bitmap(void) {
    fresh();
    map_all_bg();
    pal(0, 7, RED); pal(0, 9, GREEN); pal(0, 5, BLUE);
    v8(0, 0, 600 * 512 + 300, 7);
    v8(0, 0, 0x7FFFF, 9);                                   /* the last byte of the 512 KB */
    v8(0, 0, 504, 5);
    io32(0, 1u << 16 | 6 | 1u << 10);                       /* mode 6: BG2 is the large bitmap */
    io16(8 + 4, 0);                                         /* 512x1024 */
    io16(0x20, 0x100); io16(0x22, 0); io16(0x24, 0); io16(0x26, 0x100);
    io32(0x28, 300 << 8); io32(0x2C, 600 << 8);
    CHECK(PX(0, 0) == argb(RED) && PX(1, 0) == argb(BACKDROP) && PX(0, 1) == argb(BACKDROP), "512x1024, (300, 600): %08x", PX(0, 0));
    io32(0x28, 511 << 8); io32(0x2C, 1023 << 8);
    CHECK(PX(0, 0) == argb(GREEN), "the far corner (offset 7FFFFh): %08x", PX(0, 0));
    /* past the edge: transparent, unless the area overflow bit wraps it (BG2CNT.13) */
    io32(0x28, (u32)(-8 << 8)); io32(0x2C, 0);
    CHECK(PX(0, 0) == argb(BACKDROP), "outside without wrap");
    io16(8 + 4, 1 << 13);
    CHECK(PX(0, 0) == argb(BLUE), "wrapped to x = 504: %08x", PX(0, 0));
    /* size 1 is 1024x512, rows 1024 bytes */
    io16(8 + 4, 1 << 14);
    v8(0, 0, 400 * 1024 + 900, 7);
    io32(0x28, 900 << 8); io32(0x2C, 400 << 8);
    CHECK(PX(0, 0) == argb(RED) && PX(1, 0) == argb(BACKDROP), "1024x512, (900, 400): %08x", PX(0, 0));
    /* the screen base field is ignored */
    io16(8 + 4, 1 << 14 | 5 << 8);
    CHECK(PX(0, 0) == argb(RED), "screen base doesn't apply");
}

/* ---- extended palettes and direct-colour bitmaps ------------------------------------------- */
static void test_bg_ext_palette(void) {
    fresh();
    /* bank E as engine A's BG extended palettes: 4 slots of 8 KB. Fill it while it is LCDC memory, as a game
     * must, then allocate it. Colour of (slot k, palette 5, index 3) = 0x0100 * (k + 1) + 1. */
    wr8(&s->arm9, IO + 0x244, 0x80);
    for (u32 k = 0; k < 4; k++) wr16(&s->arm9, 0x06880000u + k * 0x2000 + (5 * 256 + 3) * 2, 0x100 * (k + 1) + 1);
    wr8(&s->arm9, IO + 0x244, 0x84);
    for (u32 i = 0; i < 64; i++) { v8(0, 0, 0x10000 + i, 3); v8(0, 0, 0x14000 + i, 3); }
    for (u32 i = 0; i < 1024; i++) { v16(0, 0, i * 2, 5 << 12); v16(0, 0, 0x800 + i * 2, 5 << 12); }
    pal(0, 3, YELLOW);
    /* BG0 uses slot 0, or slot 2 with BGxCNT.13; BG1 slot 1, or slot 3 */
    io16(8, 4u << 2 | 0x80);                                /* BG0: char block 4, map block 0 */
    io16(10, 5u << 2 | 0x80 | 1u << 8);                     /* BG1: char block 5, map block 1 */
    show(0x100 | 1u << 30);
    CHECK(PX(5, 5) == argb(0x101), "BG0, slot 0: %08x", PX(5, 5));
    io16(8, 4u << 2 | 0x80 | 0x2000);
    CHECK(PX(5, 5) == argb(0x301), "BG0 with BGxCNT.13, slot 2: %08x", PX(5, 5));
    show(0x200 | 1u << 30);
    CHECK(PX(5, 5) == argb(0x201), "BG1, slot 1: %08x", PX(5, 5));
    io16(10, 5u << 2 | 0x80 | 1u << 8 | 0x2000);
    CHECK(PX(5, 5) == argb(0x401), "BG1 with BGxCNT.13, slot 3: %08x", PX(5, 5));
    /* without DISPCNT.30 the standard palette gives the index its colour (the map's palette bits are ignored) */
    show(0x100);
    CHECK(PX(5, 5) == argb(YELLOW), "extended palettes off: %08x", PX(5, 5));
}

static void test_obj_ext_palette(void) {
    fresh();
    wr8(&s->arm9, IO + 0x245, 0x80);                        /* F as LCDC, then as engine A's OBJ extended palette */
    wr16(&s->arm9, 0x06890000u + (3 * 256 + 2) * 2, GREEN);
    wr8(&s->arm9, IO + 0x245, 0x85);
    for (u32 i = 0; i < 64; i++) v8(0, 1, i, 2);
    pal(0, 0x100 + 2, RED);
    oam(0, 0, 40 | 0x2000, 20, 3u << 12);                   /* 8x8, 256 colours, palette 3 */
    show(1u << 12 | 1u << 31);
    CHECK(PX(22, 42) == argb(GREEN), "OBJ extended palette 3, index 2: %08x", PX(22, 42));
    show(1u << 12);
    CHECK(PX(22, 42) == argb(RED), "standard OBJ palette: %08x", PX(22, 42));
}

static void test_engine_b_ext_palette(void) {
    fresh();
    /* H is engine B's BG extended palette, I its OBJ one (the engine B slots are not engine A's) */
    wr8(&s->arm9, IO + 0x248, 0x80); wr8(&s->arm9, IO + 0x249, 0x80);
    wr16(&s->arm9, 0x06898000u + 2 * 0x2000 + (1 * 256 + 4) * 2, BLUE);   /* BG0 with BGxCNT.13 uses slot 2 */
    wr16(&s->arm9, 0x068A0000u + (2 * 256 + 6) * 2, YELLOW);
    wr8(&s->arm9, IO + 0x248, 0x82); wr8(&s->arm9, IO + 0x249, 0x83);   /* VRAMCNT_H, _I: 0x247 is WRAMCNT */
    for (u32 i = 0; i < 64; i++) { v8(1, 0, 0x4000 + i, 4); v8(1, 1, i, 6); }
    for (u32 i = 0; i < 1024; i++) v16(1, 0, i * 2, 1 << 12);
    pal(1, 4, RED); pal(1, 0x100 + 6, RED);
    wr16(&s->arm9, IO + 0x1008, 1u << 2 | 0x80 | 0x2000);  /* BG0: char block 1, map block 0, slot 2 */
    oam(1, 0, 100 | 0x2000, 100, 2u << 12);
    wr32(&s->arm9, IO + 0x1000, 1u << 16 | 0x100 | 1u << 12 | 1u << 30 | 1u << 31);
    CHECK(px(1, 5, 5) == argb(BLUE), "engine B BG extended palette: %08x", px(1, 5, 5));
    CHECK(px(1, 102, 102) == argb(YELLOW), "engine B OBJ extended palette: %08x", px(1, 102, 102));
}

static void test_direct_bitmap(void) {
    fresh();
    /* BG2 in mode 5 as an extended affine bitmap (BG2CNT.7), direct colour (.2), 256x256 (size 1), 1:1 */
    io16(0x20, 0x100); io16(0x22, 0); io16(0x24, 0); io16(0x26, 0x100);
    io32(0x28, 0); io32(0x2C, 0);
    io16(8 + 4, 0x80 | 0x04 | 1 << 14);
    v16(0, 0, 0, 0x83E0); v16(0, 0, 2, 0x03E0);              /* bit 15 is the alpha: pixel 1 has none */
    v16(0, 0, 4 + 512 * 3, 0xFC00);
    show(0x400 | 5);
    CHECK(PX(0, 0) == argb(GREEN) && PX(1, 0) == argb(BACKDROP) && PX(2, 3) == argb(BLUE), "direct colour: %08x %08x %08x", PX(0, 0), PX(1, 0), PX(2, 3));
    /* the bitmap base is in 16 KB units of BGxCNT.8-12, without DISPCNT's 64 KB ones */
    io16(8 + 4, 0x80 | 0x04 | 1 << 14 | 4 << 8);
    v16(0, 0, 0x10000, 0xFC00);
    CHECK(PX(0, 0) == argb(BLUE) && PX(1, 0) == argb(BACKDROP), "bitmap base 4 * 16 KB: %08x", PX(0, 0));
    io32(0, 1u << 16 | 0x400 | 5 | 1u << 24);                /* DISPCNT.24-26: character base 64 KB steps: no effect on bitmaps */
    CHECK(PX(0, 0) == argb(BLUE), "DISPCNT character base doesn't move a bitmap");
    /* 256 colours through the palette: index 0 is transparent */
    pal(0, 1, RED);
    io32(0, 1u << 16 | 0x400 | 5);
    io16(8 + 4, 0x80 | 1 << 14);
    v8(0, 0, 0, 1); v8(0, 0, 1, 0);
    CHECK(PX(0, 0) == argb(RED) && PX(1, 0) == argb(BACKDROP), "256-colour bitmap: %08x", PX(0, 0));
    /* rotation: PA = PD = 0x80 shows the bitmap at double size */
    io16(0x20, 0x80); io16(0x26, 0x80);
    CHECK(PX(1, 0) == argb(RED) && PX(2, 0) == argb(BACKDROP), "scaled 2x: texel 0 covers two pixels: %08x %08x", PX(1, 0), PX(2, 0));
}

/* ---- display modes ------------------------------------------------------------------------- */
static void test_display_modes(void) {
    fresh();
    io32(0, 0);
    CHECK(PX(10, 10) == argb(WHITE), "display off is white: %08x", PX(10, 10));
    /* VRAM display (mode 2): the bank chosen in DISPCNT.18-19, 256 pixels a line, bits 15 ignored */
    for (u32 b = 0; b < 4; b++) {
        u16 *v = (u16 *)(s->vram + b * 0x20000 + (3 * 256 + 5) * 2);
        *v = (u16)(0x8000 | (b + 1) * 0x0421);
    }
    io32(0, 2u << 16 | 1u << 18);
    CHECK(PX(5, 3) == argb(2 * 0x0421), "VRAM display of bank B: %08x", PX(5, 3));
    io32(0, 2u << 16 | 3u << 18);
    CHECK(PX(5, 3) == argb(4 * 0x0421), "bank D");
    /* master brightness applies to VRAM display too: decrease by 8/16 takes white's 31 to 31 - 15 = 16 */
    io32(0, 2u << 16 | 0u << 18);
    io16(0x6C, 2 << 14 | 8);
    *(u16 *)(s->vram + (3 * 256 + 5) * 2) = 0x7FFF;
    CHECK(PX(5, 3) == argb(16 | 16 << 5 | 16 << 10), "brightness decrease 8/16 of white: %08x", PX(5, 3));
    io16(0x6C, 1 << 14 | 16);
    *(u16 *)(s->vram + (3 * 256 + 5) * 2) = 0;
    CHECK(PX(5, 3) == argb(WHITE), "increase by 16/16: %08x", PX(5, 3));
    /* a screen with its POWCNT1 bit clear is black */
    s->powcnt1 = 0x8003;
    CHECK(px(1, 5, 3) == 0xFF000000u, "engine B off is black");
}

/* ---- the affine reference points -------------------------------------------------------------- */
/* BG2 as a 256x256 256-colour bitmap whose row r is palette entry r % 255 + 1 (the colour is the entry
 * number), shown 1:1, so a line's colour says which row its origin was. Whole frames run with both CPUs
 * stopped; HBlank DMA writes BG2Y once a line, as a game does for a per-line effect. */
static u32 row_col(u32 r) { return argb(r % 255 + 1); }
static u32 line_col(int y) { return s->fb[0][y * SCREEN_W + 5]; }

static void row_bitmap(void) {
    fresh();
    s->arm9.stop = s->arm7.stop = 1;
    for (u32 r = 0; r < 256; r++) {
        pal(0, r % 255 + 1, (u16)(r % 255 + 1));
        for (u32 x = 0; x < 256; x++) v8(0, 0, r * 256 + x, r % 255 + 1);
    }
    io16(0x20, 0x100); io16(0x22, 0); io16(0x24, 0); io16(0x26, 0x100);
    io32(0x28, 0); io32(0x2C, 0);
    io16(8 + 4, 0x80 | 1 << 14);
    io32(0, 1u << 16 | 0x400 | 5);
}

/* DMA channel ch writes IO register `reg` = table[line] at the end of each visible line (repeating) or once */
static void reg_writer(int ch, u32 reg, const u32 *table, int repeat) {
    memcpy(s->ram + 0x100000 + ch * 0x1000, table, 192 * 4);
    io32(0xB0 + 12 * (u32)ch, 0x02100000u + (u32)ch * 0x1000); io32(0xB4 + 12 * (u32)ch, IO + reg);
    io32(0xB8 + 12 * (u32)ch, 1 | 2u << 21 | (repeat ? 1u << 25 : 0) | 1u << 26 | 2u << 27 | 1u << 31);
}
static void y_writer(const u32 *table) { reg_writer(0, 0x2C, table, 1); }     /* BG2Y = table[line] */

static int rows_wrong(u32 (*row)(int)) {
    int bad = 0;
    for (int y = 0; y < 192; y++) bad += line_col(y) != row_col(row(y));
    return bad;
}
static u32 row_identity(int y) { return (u32)y; }
static u32 row_from_40(int y) { return 40u + (u32)y; }
static u32 row_per_line(int y) { return (u32)(y * 3 + 7) % 180; }
static u32 row_after_first(int y) { return y ? 100u : 0u; }
static u32 row_after_write(int y) { return y ? 50u + (u32)y - 1 : 0u; }       /* rows stay below 256, the bitmap's end */
static u32 row_from_50(int y) { return 50u + (u32)y; }

static void test_reference_points(void) {
    /* no writes: line L starts from the reference point plus PD * L */
    row_bitmap();
    nds_run_frame(s);
    CHECK(!rows_wrong(row_identity), "reference point 0: %d lines wrong", rows_wrong(row_identity));
    /* a write in VBlank is copied at the start of the frame */
    row_bitmap();
    io32(0x2C, 40 << 8);
    nds_run_frame(s);
    CHECK(!rows_wrong(row_from_40), "written in VBlank: %d lines wrong", rows_wrong(row_from_40));
    /* a write at the end of each line is the origin of the next: every line gets its own row (the line
     * drawn from the register's last value plus PD * line would be one row 191 lines on from it) */
    row_bitmap();
    u32 table[192];
    io32(0x2C, row_per_line(0) << 8);
    for (int l = 0; l < 192; l++) table[l] = row_per_line(l + 1) << 8;
    y_writer(table);
    nds_run_frame(s);
    CHECK(!rows_wrong(row_per_line), "one write a line: %d lines wrong", rows_wrong(row_per_line));
    /* the same value written every line puts every line after the first on that row, PD or not */
    row_bitmap();
    for (int l = 0; l < 192; l++) table[l] = 100 << 8;
    y_writer(table);
    nds_run_frame(s);
    CHECK(!rows_wrong(row_after_first), "the same write each line: %d lines wrong", rows_wrong(row_after_first));
    /* one write only, at the end of line 0 (a DMA that is not repeated): line 1 starts from it and the lines
     * after it advance by PD from there; the next frame starts from the register again, with no write
     * to make it do so */
    row_bitmap();
    table[0] = 50 << 8;
    y_writer(table);
    io32(0xB8, 1 | 2u << 21 | 1u << 26 | 2u << 27 | 1u << 31);      /* once */
    nds_run_frame(s);
    CHECK(!rows_wrong(row_after_write), "one write: %d lines wrong", rows_wrong(row_after_write));
    nds_run_frame(s);
    CHECK(!rows_wrong(row_from_50), "the frame after starts from the register: %d lines wrong", rows_wrong(row_from_50));
}

/* A direct colour bitmap whose pixel (tx, ty) is its own coordinates, so a line says where both of its
 * origin's axes were: the colour is ty in bits 0-7 and tx's low 7 bits in 8-14. PA = 1.0, PC = 0. */
static u32 xy_pix(u32 tx, u32 ty) { return 0x8000u | (tx & 0x7F) << 8 | (ty & 0xFF); }
static void xy_bitmap(u32 pb, u32 pd) {
    fresh();
    s->arm9.stop = s->arm7.stop = 1;
    for (u32 ty = 0; ty < 256; ty++)
        for (u32 tx = 0; tx < 256; tx++) v16(0, 0, (ty * 256 + tx) * 2, xy_pix(tx, ty));
    io16(0x20, 0x100); io16(0x22, pb); io16(0x24, 0); io16(0x26, pd);
    io32(0x28, 0); io32(0x2C, 0);
    io16(8 + 4, 0x80 | 0x04 | 1 << 14);
    io32(0, 1u << 16 | 0x400 | 5);
}
static u32 want_tx[192], want_ty[192];                /* the texel that column 5 of each line should show */
static int xy_wrong(void) {
    int bad = 0;
    for (int y = 0; y < 192; y++) bad += line_col(y) != argb(xy_pix(want_tx[y], want_ty[y]));
    return bad;
}
static u32 x_val(int l) { return ((u32)l * 5 + 3) % 90; }
static u32 y_val(int l) { return ((u32)l * 3 + 7) % 180; }

/* BG2X and BG2Y are separate internal registers (GBATEK): a write to one copies that one and the other
 * keeps advancing by its PB or PD, so a wave done with BGxX alone doesn't stop the rows moving down. */
static void test_reference_axes(void) {
    u32 tx_w[192], ty_w[192];
    /* BG2X written every line, PB = PD = 1.0: each line starts from the value just written and, since
     * the write is at the end of the line before, has advanced by nothing; Y runs on from the register */
    xy_bitmap(0x100, 0x100);
    for (int l = 0; l < 192; l++) tx_w[l] = x_val(l) << 8;
    reg_writer(0, 0x28, tx_w, 1);
    nds_run_frame(s);
    for (int y = 0; y < 192; y++) { want_tx[y] = (y ? x_val(y - 1) : 0) + 5; want_ty[y] = (u32)y; }
    CHECK(!xy_wrong(), "BG2X every line, PD 1.0: %d lines wrong", xy_wrong());
    /* the same for BG2Y: X runs on by PB, Y is the value just written */
    xy_bitmap(0x100, 0x100);
    for (int l = 0; l < 192; l++) ty_w[l] = y_val(l) << 8;
    reg_writer(0, 0x2C, ty_w, 1);
    nds_run_frame(s);
    for (int y = 0; y < 192; y++) { want_tx[y] = (u32)y + 5; want_ty[y] = y ? y_val(y - 1) : 0; }
    CHECK(!xy_wrong(), "BG2Y every line, PB 1.0: %d lines wrong", xy_wrong());
    /* both, on two channels */
    xy_bitmap(0x100, 0x100);
    reg_writer(0, 0x28, tx_w, 1); reg_writer(1, 0x2C, ty_w, 1);
    nds_run_frame(s);
    for (int y = 0; y < 192; y++) { want_tx[y] = (y ? x_val(y - 1) : 0) + 5; want_ty[y] = y ? y_val(y - 1) : 0; }
    CHECK(!xy_wrong(), "BG2X and BG2Y every line: %d lines wrong", xy_wrong());
    /* one write of BG2X at the end of line 0: X advances by PB from there (lines 1, 2, ... are 0, 1, ...
     * on from it) while Y has run on since the top; the next frame starts from both registers */
    xy_bitmap(0x100, 0x100);
    tx_w[0] = 20 << 8;
    reg_writer(0, 0x28, tx_w, 0);
    nds_run_frame(s);
    for (int y = 0; y < 192; y++) { want_tx[y] = (y ? 20u + (u32)y - 1 : 0) + 5; want_ty[y] = (u32)y; }
    CHECK(!xy_wrong(), "one write of BG2X: %d lines wrong", xy_wrong());
    nds_run_frame(s);
    for (int y = 0; y < 192; y++) { want_tx[y] = 20u + (u32)y + 5; want_ty[y] = (u32)y; }
    CHECK(!xy_wrong(), "one write of BG2X, the frame after: %d lines wrong", xy_wrong());
    /* the same for BG2Y */
    xy_bitmap(0x100, 0x100);
    ty_w[0] = 50 << 8;
    reg_writer(0, 0x2C, ty_w, 0);
    nds_run_frame(s);
    for (int y = 0; y < 192; y++) { want_tx[y] = (u32)y + 5; want_ty[y] = y ? 50u + (u32)y - 1 : 0; }
    CHECK(!xy_wrong(), "one write of BG2Y: %d lines wrong", xy_wrong());
    nds_run_frame(s);
    for (int y = 0; y < 192; y++) { want_tx[y] = (u32)y + 5; want_ty[y] = 50u + (u32)y; }
    CHECK(!xy_wrong(), "one write of BG2Y, the frame after: %d lines wrong", xy_wrong());
}

int main(void) {
    test_window_rect();
    test_window_edges();
    test_window_priority();
    test_window_obj();
    test_window_effects();
    test_mosaic_bg();
    test_mosaic_obj();
    test_bitmap_obj();
    test_bitmap_obj_alpha();
    test_large_bitmap();
    test_bg_ext_palette();
    test_obj_ext_palette();
    test_engine_b_ext_palette();
    test_direct_bitmap();
    test_display_modes();
    test_reference_points();
    test_reference_axes();
    if (s) nds_shutdown(s);
    if (!fails) printf("2D engines: windows, mosaic, bitmap OBJs, large bitmap, extended palettes, display modes, affine reference points and axes\n");
    return fails != 0;
}
