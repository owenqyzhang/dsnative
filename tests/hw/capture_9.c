/* Display capture's test (runtime/capture.c), ARM9 side only: engine A's output captured into VRAM bank
 * B, which neither screen uses (tests/hw/expect_capture.txt has the expected lines). DISPCNT is left
 * alone, so the screens stay as they are: bank A keeps showing the Mandelbrot set, and the captures
 * read it as source B. Each image is checked against what GBATEK's formulas give, computed here, before
 * its checksum (FNV-1a over the halfwords) is printed; the values are GBATEK's, not measured on hardware.
 *   capture_reg    DISPCAPCNT after writing 6FFFFFFFh: only the defined bits read back
 *   capture_busy   the enable bit before a capture is armed, right after arming it in VBlank, in the middle
 *                  of the next frame, and after line 192
 *   capture_a      source A, the graphics screen: with no layers on, engine A's backdrop (palette entry 0),
 *                  256x192, opaque
 *   capture_b      source B alone: a copy of bank A, the Mandelbrot set as it is on the screen
 *   capture_blend  A at 8/16 plus B at 8/16 where B's pixels have no alpha bit (the Mandelbrot's): only A's half
 *   capture_fifo   A at 8/16 plus the main memory display FIFO at 8/16, fed by DMA in main memory display
 *                  timing from a picture in RAM
 *   capture_size   128x128 at write offset 0x8000: packed 128 pixels a line, and nothing else written */
#include "regs.h"

#define DISPCAPCNT R32(0x04000064)
#define DMA0SAD R32(0x040000B0)
#define DMA0DAD R32(0x040000B4)
#define DMA0CNT R32(0x040000B8)
#define VRAMCNT_B R8(0x04000241)
#define PALETTE_A R16(0x05000000)
#define BANK_A ((volatile u16 *)0x06800000)
#define BANK_B ((volatile u16 *)0x06820000)
#define BUSY (1u << 31)

#define RGB(r, g, b) ((u32)(r) | (u32)(g) << 5 | (u32)(b) << 10)
/* DISPCAPCNT: EVA, EVB, write bank (B), write offset, size, source A, source B, capture source, enable */
#define CAP(eva, evb, woff, size, srcb, src) \
    ((u32)(eva) | (u32)(evb) << 8 | 1u << 16 | (u32)(woff) << 18 | (u32)(size) << 20 | (u32)(srcb) << 25 | (u32)(src) << 29 | BUSY)

static const u32 A_COL = RGB(20, 10, 30);
#define PIXELS (256 * 192)

static u16 picture[PIXELS];             /* the main memory display picture, 96 KB in .bss */

static void put_word(u32 v) { dbg(" "); dbg_hex(v); }
static u32 busy(void) { return DISPCAPCNT >> 31; }

static u32 fnv(volatile const u16 *p, u32 n) {
    u32 h = 0x811C9DC5u;
    for (u32 i = 0; i < n; i++) h = (h ^ p[i]) * 0x01000193u;
    return h;
}
static void fill_b(u16 v) { for (u32 i = 0; i < 0x10000; i++) BANK_B[i] = v; }

/* arms a capture in VBlank and waits until its frame has ended (line 192) */
static void capture_frame(u32 cap) {
    vblank_wait();
    DISPCAPCNT = cap;
    vblank_wait();                      /* the capture runs in the frame that starts at the next line 0, and is done at its line 192 */
}

static u32 blend5(u32 a, u32 b, u32 eva, u32 evb) {
    u32 v = (a * eva + b * evb) >> 4;
    return v > 31 ? 31 : v;
}

static void report(const char *name, int bad, u32 h) {
    dbg(name);
    if (bad) { dbg(" FAIL"); put_word((u32)bad); }
    else { dbg(" ok"); put_word(h); }
    dbg("\n");
}

void capture_test9(void) {
    VRAMCNT_B = 0x80;                   /* bank B: LCDC, where the captures go */
    DISPCAPCNT = 0x6FFFFFFFu;
    dbg("capture_reg"); put_word(DISPCAPCNT); dbg("\n");
    DISPCAPCNT = 0;

    PALETTE_A = (u16)A_COL;             /* engine A's backdrop: the graphics screen with no layers on */
    fill_b(0x1357);

    /* the busy bit: clear when idle, set by arming it, still set in the middle of the frame, clear after line 192 */
    vblank_wait();
    while (REG_VCOUNT < 100 || REG_VCOUNT >= 192) ;
    u32 idle = busy();
    vblank_wait();
    DISPCAPCNT = CAP(0, 0, 0, 3, 0, 0);
    u32 armed = busy();
    while (REG_VCOUNT >= 192 || REG_VCOUNT < 100) ;       /* into the next frame, whose lines the capture takes */
    u32 mid = busy();
    vblank_wait();
    u32 done = busy();
    dbg("capture_busy"); put_word(idle); put_word(armed); put_word(mid); put_word(done); dbg("\n");

    /* source A: the backdrop, opaque */
    int bad = 0;
    for (u32 i = 0; i < PIXELS; i++) bad += BANK_B[i] != (u16)(0x8000u | A_COL);
    bad += BANK_B[PIXELS] != 0x1357;
    report("capture_a", bad, fnv(BANK_B, 0x10000));

    /* source B alone: bank A as the screen shows it, alpha bit and all */
    fill_b(0x1357);
    capture_frame(CAP(0, 0, 0, 3, 0, 1));
    bad = 0;
    for (u32 i = 0; i < PIXELS; i++) bad += BANK_B[i] != BANK_A[i];
    report("capture_b", bad, fnv(BANK_B, PIXELS));

    /* A and B blended: B's pixels (bit 15 clear) weigh nothing and give no alpha, so A's half is left */
    fill_b(0x1357);
    capture_frame(CAP(8, 8, 0, 3, 0, 2));
    bad = 0;
    u16 want = (u16)(0x8000u | RGB(blend5(20, 0, 8, 8), blend5(10, 0, 8, 8), blend5(30, 0, 8, 8)));
    for (u32 i = 0; i < PIXELS; i++) bad += BANK_B[i] != want;
    report("capture_blend", bad, fnv(BANK_B, PIXELS));

    /* the main memory display FIFO as source B: DMA 4 words a burst, destination fixed, in main memory
     * display timing (4), started in VBlank so it runs from the next frame's line 0 */
    for (u32 i = 0; i < PIXELS; i++) picture[i] = (u16)((i * 5 + 1) & 0x7FFF);
    fill_b(0x1357);
    vblank_wait();
    DMA0SAD = (u32)picture;
    DMA0DAD = 0x04000068;
    DMA0CNT = 4 | 2u << 21 | 1u << 25 | 1u << 26 | 4u << 27 | 1u << 31;
    DISPCAPCNT = CAP(8, 8, 0, 3, 1, 2);
    vblank_wait();
    DMA0CNT = 0;
    bad = 0;
    for (u32 i = 0; i < PIXELS; i++) {
        u32 b = picture[i];
        u16 w = (u16)(0x8000u | RGB(blend5(20, b & 31, 8, 8), blend5(10, (b >> 5) & 31, 8, 8), blend5(30, (b >> 10) & 31, 8, 8)));
        bad += BANK_B[i] != w;
    }
    report("capture_fifo", bad, fnv(BANK_B, PIXELS));

    /* 128x128 at offset 0x8000 (field 1): the pixels are packed 128 to a line and fill 0x8000..0xFFFF of the
     * bank; the rest of it is as it was */
    fill_b(0x1357);
    capture_frame(CAP(0, 0, 1, 0, 0, 0));
    bad = 0;
    for (u32 i = 0; i < 0x10000; i++) {
        u16 w = (i >= 0x4000 && i < 0x8000) ? (u16)(0x8000u | A_COL) : 0x1357;
        bad += BANK_B[i] != w;
    }
    report("capture_size", bad, fnv(BANK_B, 0x10000));

    PALETTE_A = 0;
}
