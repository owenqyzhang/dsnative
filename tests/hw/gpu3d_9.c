/* The 3D engine's test (runtime/gpu3d.c), ARM9 side only: geometry commands through the direct
 * ports, the packed GXFIFO port and geometry FIFO DMA, then what GXSTAT, the matrix readbacks
 * and the box and position tests say (tests/hw/expect_gpu3d.txt has the expected lines). No
 * polygon is drawn and DISPCNT is left alone, so the screens stay as they are. The values come
 * from GBATEK, not from a run on hardware. Prints:
 *   gxstat   GXSTAT at power-on: FIFO empty and under half full, nothing busy
 *   boxbusy  GXSTAT.0 right after a BOX_TEST, and once it has finished
 *   box      the results for a box inside the view volume, one outside, one around the whole
 *            volume (false: only the box's faces are tested), one with a face through it (sent
 *            through GXFIFO as a packed command) and one beyond the far plane
 *   clip     the clip matrix after MTX_SCALE and MTX_TRANS: m[0] m[12] m[13] m[14]
 *   postest  POS_TEST through it: x y z w
 *   stack    the position stack level after 3 pushes and after 3 pops
 *   pstack   the projection stack: level after a push, after the pop, the error flag after a second
 *            push, then the flag and level after acknowledging by writing GXSTAT.15
 *   fifo     SWAP_BUFFERS halts the engine until VBlank: 200 commands behind it fill the FIFO to
 *            about 200 entries, not under half full, busy
 *   fifodone GXSTAT once VBlank has let the queue drain
 *   gxirq    the FIFO-empty interrupt (GXSTAT.31) is taken
 *   gxdma    a command list sent by DMA3 in geometry FIFO mode: the translation it made
 * and the texture side, again with nothing drawn (DISPCNT's 3D bit stays clear, and the VRAM banks
 * it borrows, B, E, F and G, are put back):
 *   texvram  bank B in LCDC mode keeps what is written, hides it while it is mapped as texture slot
 *            1, and shows it again: the word read while hidden, then the two read back
 *   texpal   the same for the texture palette banks E, F and G: hidden, back, for each
 *   texpoly  seven quads, one in each texture format, sent as packed commands (POLYGON_ATTR,
 *            TEXIMAGE_PARAM, PLTT_BASE, BEGIN_VTXS, then TEXCOORD and VTX_16 pairs): RAM_COUNT says
 *            how many polygons and vertices the geometry engine took
 *   disp3d   DISP3DCNT reads back what was written (all the control bits) */
#include "regs.h"

#define GXSTAT R32(0x04000600)
#define GX(cmd) R32(0x04000400 + 4 * (cmd))
#define GXFIFO R32(0x04000400)
#define CLIP(i) R32(0x04000640 + 4 * (i))
#define POS_RESULT(i) R32(0x04000620 + 4 * (i))
enum { MTX_MODE = 0x10, MTX_PUSH, MTX_POP, MTX_IDENTITY = 0x15, MTX_SCALE = 0x1B, MTX_TRANS,
       SWAP_BUFFERS = 0x50, BOX_TEST = 0x70, POS_TEST };
#define REG_VRAMCNT_B R8(0x04000241)
#define REG_VRAMCNT_E R8(0x04000244)
#define REG_VRAMCNT_F R8(0x04000245)
#define REG_VRAMCNT_G R8(0x04000246)
#define BUSY (1u << 27)
#define IRQ_GXFIFO (1u << 21)

static void put_word(u32 v) { dbg(" "); dbg_hex(v); }
static void wait_idle(void) { for (u32 i = 0; i < 1000000 && (GXSTAT & BUSY); i++) ; }
static void wait_test(void) { for (u32 i = 0; i < 1000000 && (GXSTAT & 1); i++) ; }

/* the box test result (GXSTAT.1) for a box at x y z with sides w h d, 1.3.12 numbers */
static u32 box_params(s32 x, s32 y, s32 z, s32 w, s32 h, s32 d, int packed) {
    u32 p0 = (u16)x | (u32)(u16)y << 16, p1 = (u16)z | (u32)(u16)w << 16, p2 = (u16)h | (u32)(u16)d << 16;
    if (packed) { GXFIFO = BOX_TEST; GXFIFO = p0; GXFIFO = p1; GXFIFO = p2; }
    else { GX(BOX_TEST) = p0; GX(BOX_TEST) = p1; GX(BOX_TEST) = p2; }
    wait_test();
    return (GXSTAT >> 1) & 1;
}

static void trans(s32 x, s32 y, s32 z) { GX(MTX_TRANS) = (u32)x; GX(MTX_TRANS) = (u32)y; GX(MTX_TRANS) = (u32)z; }

/* a packed word of MTX_MODE, MTX_IDENTITY, MTX_TRANS and a NOP, then the parameters of those that
 * take any: mode 1 (position), and a translation of 1, 2, 3 */
static const u32 dma_list[5] = {0x001C1510u, 1, 0x1000, 0x2000, 0x3000};

/* a bank in LCDC mode at `window`: two words written, read while the bank is mapped as `texture`, read again once LCDC again */
static void bank_hidden(volatile u8 *cnt, u32 texture, u32 window, u32 a, u32 b) {
    volatile u32 *w = (volatile u32 *)window;
    *cnt = 0x80;
    w[0] = a; w[1] = b;
    *cnt = (u8)texture;
    put_word(w[0] | w[1]);
    *cnt = 0x80;
    put_word(w[0] == a && w[1] == b);
    *cnt = 0;
}

/* a textured quad at (x, y) of 1.0 with the texture format and size bits of TEXIMAGE_PARAM, as packed commands */
static void tex_quad(u32 teximage, u32 pltt) {
    static const u16 half[4][2] = {{0xF800, 0xF800}, {0x0800, 0xF800}, {0x0800, 0x0800}, {0xF800, 0x0800}};
    GXFIFO = 0x402B2A29u;               /* POLYGON_ATTR, TEXIMAGE_PARAM, PLTT_BASE, BEGIN_VTXS */
    GXFIFO = 31u << 16 | 3u << 6;       /* opaque, both sides */
    GXFIFO = teximage;
    GXFIFO = pltt;
    GXFIFO = 1;                         /* quads */
    for (int i = 0; i < 4; i += 2) {
        GXFIFO = 0x23222322u;           /* TEXCOORD, VTX_16, TEXCOORD, VTX_16 */
        for (int k = 0; k < 2; k++) {
            GXFIFO = (u32)(k == 0 ? (i + 0) * 64 : (i + 1) * 64) | (u32)(i * 32) << 16;
            GXFIFO = half[i + k][0] | (u32)half[i + k][1] << 16;
            GXFIFO = 0;
        }
    }
    GXFIFO = 0x41;                      /* END_VTXS */
}

static void tex_part(void) {
    REG_VRAMCNT_B = 0; REG_VRAMCNT_E = 0; REG_VRAMCNT_F = 0; REG_VRAMCNT_G = 0;
    dbg("texvram");
    bank_hidden(&REG_VRAMCNT_B, 0x8B, 0x06820000, 0x12345678, 0xCAFEF00D);
    dbg("\n");
    dbg("texpal");
    bank_hidden(&REG_VRAMCNT_E, 0x83, 0x06880000, 0xAAAA0001, 0x0BAD0001);
    bank_hidden(&REG_VRAMCNT_F, 0x83, 0x06890000, 0xBBBB0002, 0x0BAD0002);
    bank_hidden(&REG_VRAMCNT_G, 0x8B, 0x06894000, 0xCCCC0003, 0x0BAD0003);
    dbg("\n");

    GX(MTX_MODE) = 0; GX(MTX_IDENTITY) = 0;
    GX(MTX_MODE) = 1; GX(MTX_IDENTITY) = 0;
    wait_idle();
    u32 before = R32(0x04000604);
    for (u32 fmt = 1; fmt <= 7; fmt++)
        tex_quad(fmt << 26 | (fmt & 3) << 20 | (fmt & 3) << 23 | 1u << 16 | 1u << 17 | 0x40, 0x10 + fmt);
    wait_idle();
    u32 after = R32(0x04000604);
    dbg("texpoly"); put_word(after - before); dbg("\n");

    R16(0x04000060) = 0x49FF;
    dbg("disp3d"); put_word(R16(0x04000060)); dbg("\n");
    R16(0x04000060) = 0;
}

void gpu3d_test9(void) {
    REG_POWCNT1 |= 0x0C;                /* geometry and rendering engines on */
    dbg("gxstat"); put_word(GXSTAT); dbg("\n");

    GX(MTX_MODE) = 0; GX(MTX_IDENTITY) = 0;
    GX(MTX_MODE) = 1; GX(MTX_IDENTITY) = 0;
    wait_idle();

    GX(BOX_TEST) = 0xF800F800u; GX(BOX_TEST) = 0x1000F800u; GX(BOX_TEST) = 0x10001000u;     /* -0.5 -0.5 -0.5, sides 1 */
    u32 busy_at_once = GXSTAT & 1;
    wait_test();
    u32 inside = (GXSTAT >> 1) & 1;
    dbg("boxbusy"); put_word(busy_at_once); put_word(GXSTAT & 1); dbg("\n");
    u32 outside = box_params(0x5000, 0, 0, 0x1000, 0x1000, 0x1000, 0);         /* x 5 */
    u32 around = box_params(-0x4000, -0x4000, -0x4000, 0x7000, 0x7000, 0x7000, 0);     /* -4 to 3 */
    u32 straddle = box_params(-0x4000, -0x4000, -0x4000, 0x4800, 0x4800, 0x4800, 1);   /* -4 to 0.5 */
    u32 beyond = box_params(0, 0, 0x3000, 0x1000, 0x1000, 0x1000, 0);          /* z 3 */
    dbg("box"); put_word(inside); put_word(outside); put_word(around); put_word(straddle); put_word(beyond); dbg("\n");

    GX(MTX_SCALE) = 0x2000; GX(MTX_SCALE) = 0x2000; GX(MTX_SCALE) = 0x2000;
    trans(0x1000, 0x2000, 0x3000);      /* sent after the scale, and each command multiplies on the left: 2, 4, 6 */
    wait_idle();
    dbg("clip"); put_word(CLIP(0)); put_word(CLIP(12)); put_word(CLIP(13)); put_word(CLIP(14)); dbg("\n");
    GX(POS_TEST) = 0x08000800u; GX(POS_TEST) = 0x0800u;
    wait_test();
    dbg("postest"); put_word(POS_RESULT(0)); put_word(POS_RESULT(1)); put_word(POS_RESULT(2)); put_word(POS_RESULT(3)); dbg("\n");

    GX(MTX_MODE) = 1;
    for (int i = 0; i < 3; i++) GX(MTX_PUSH) = 0;
    wait_idle();
    u32 pushed = GXSTAT & 0x9F00;
    for (int i = 0; i < 3; i++) GX(MTX_POP) = 1;
    wait_idle();
    dbg("stack"); put_word(pushed); put_word(GXSTAT & 0x9F00); dbg("\n");

    GX(MTX_MODE) = 0;
    GX(MTX_PUSH) = 0; wait_idle();
    u32 p1 = GXSTAT & 0xA000;
    GX(MTX_POP) = 1; wait_idle();
    u32 p2 = GXSTAT & 0xA000;
    GX(MTX_PUSH) = 0; GX(MTX_PUSH) = 0; wait_idle();
    u32 p3 = GXSTAT & 0x8000;
    GXSTAT = 0x8000;                    /* acknowledge: clears the flag and the projection stack pointer */
    wait_idle();
    dbg("pstack"); put_word(p1); put_word(p2); put_word(p3); put_word(GXSTAT & 0xA000); dbg("\n");
    GX(MTX_MODE) = 1;

    GX(SWAP_BUFFERS) = 0;
    for (int i = 0; i < 200; i++) GX(MTX_IDENTITY) = 0;
    u32 st = GXSTAT, level = (st >> 16) & 0x1FF;
    if (level >= 190 && level <= 200 && !(st & (1u << 25)) && !(st & (1u << 26)) && (st & BUSY)) dbg("fifo ok\n");
    else { dbg("fifo FAIL"); put_word(st); dbg("\n"); }
    vblank_wait();
    wait_idle();
    dbg("fifodone"); put_word(GXSTAT); dbg("\n");

    REG_IE |= IRQ_GXFIFO;
    GXSTAT = 2u << 30;                  /* interrupt while the FIFO is empty */
    intr_wait(0, IRQ_GXFIFO);
    GXSTAT = 0;
    REG_IE &= ~IRQ_GXFIFO;
    dbg("gxirq ok\n");

    GX(MTX_MODE) = 0; GX(MTX_IDENTITY) = 0;
    REG_DMA3SAD = (u32)dma_list;
    REG_DMA3DAD = 0x04000400;
    REG_DMA3CNT = 5 | 2u << 21 | 1u << 26 | 7u << 27 | 1u << 31;      /* 32-bit words to a fixed address, geometry FIFO */
    for (u32 i = 0; i < 1000000 && (REG_DMA3CNT & (1u << 31)); i++) ;
    wait_idle();
    dbg("gxdma"); put_word(CLIP(12)); put_word(CLIP(13)); put_word(CLIP(14)); dbg("\n");
    tex_part();
}
