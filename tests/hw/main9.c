/* The ARM9 side of the hardware test. Built as Thumb, with the interrupt handler and one
 * ITCM function in ARM. Prints its results through the no$gba debug port. */
#include "regs.h"
#include "suite.h"

void out_str(const char *s) { dbg(s); }

volatile u32 vblanks, timer_irqs;

__attribute__((target("arm"))) void irq_handler(void) {
    u32 f = REG_IF & REG_IE;
    if (f & IRQ_VBLANK) vblanks++;
    if (f & IRQ_TIMER0) timer_irqs++;
    REG_IF = f;
    R32(0x027C3FF8) |= f;               /* tell the BIOS IntrWait */
}

/* runs from ITCM: copied there by the autoload segment */
__attribute__((section(".itcm"), target("arm"), noinline)) u32 itcm_hash(const u32 *p, int n) {
    u32 h = 0x811C9DC5u;
    for (int i = 0; i < n; i++) h = (h ^ p[i]) * 0x01000193u;
    return h;
}

static u32 swi_div(s32 n, s32 d, s32 *rem) {
    register s32 r0 __asm__("r0") = n;
    register s32 r1 __asm__("r1") = d;
    __asm__ volatile("swi 9" : "+r"(r0), "+r"(r1) :: "r3", "memory");
    *rem = r1;
    return (u32)r0;
}
static u32 swi_sqrt(u32 v) {
    register u32 r0 __asm__("r0") = v;
    __asm__ volatile("swi 13" : "+r"(r0) :: "r1", "r3", "memory");
    return r0;
}

static u32 isqrt(u32 v) { u32 r = 0; for (u32 b = 1u << 30; b; b >>= 2) { if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1; } return r; }

static u32 seed = 99;
static u32 rnd(void) { seed = seed * 1103515245u + 12345u; return seed ^ (seed >> 15); }

static void test_hw_math(void) {
    int bad = 0;
    for (int i = 0; i < 100; i++) {
        s32 n = (s32)rnd(), d = (s32)(rnd() >> (rnd() & 31)) | 1;
        REG_DIVCNT = 0;
        *(volatile s32 *)0x04000290 = n;
        *(volatile s32 *)0x04000298 = d;
        if ((s32)REG_DIV_RESULT != n / d || (s32)REG_DIVREM != n % d) bad++;
        s64 n64 = ((s64)rnd() << 32) | rnd();
        REG_DIVCNT = 1;
        REG_DIV_NUMER = n64;
        *(volatile s32 *)0x04000298 = d;
        if (REG_DIV_RESULT != n64 / d) bad++;
        u32 v = rnd();
        REG_SQRTCNT = 0;
        REG_SQRT_PARAM = v;
        if (REG_SQRT_RESULT != isqrt(v)) bad++;
        s32 rem;
        if (swi_div(n, d, &rem) != (u32)(n / d) || rem != n % d) bad++;
        if (swi_sqrt(v) != isqrt(v)) bad++;
    }
    dbg("hwmath "); dbg(bad ? "FAIL " : "ok "); dbg_hex((u32)bad); dbg("\n");
}

static void test_ipc(void) {
    REG_IPCFIFOCNT = FIFO_ENABLE | FIFO_SEND_CLEAR;
    while ((REG_IPCSYNC & 0xF) != 0xA) ;    /* the ARM7 is up */
    dbg("ipc");
    for (u32 v = 1; v <= 5; v++) {
        REG_IPCFIFOSEND = v;
        while (REG_IPCFIFOCNT & FIFO_RECV_EMPTY) ;
        dbg(" "); dbg_hex(REG_IPCFIFORECV);
    }
    dbg("\n");
}

static void test_card(void) {
    u32 fat = *(volatile u32 *)0x027FFE48;      /* the header copy the firmware leaves */
    u32 start = 0;
    /* read the FAT block, then the block holding file 0 */
    u32 block[128];
    for (int pass = 0; pass < 2; pass++) {
        u32 a = pass ? (start & ~0x1FFu) : (fat & ~0x1FFu);
        REG_AUXSPICNT = 0x8000;
        REG_CARDCMD[0] = 0xB7; REG_CARDCMD[1] = (u8)(a >> 24); REG_CARDCMD[2] = (u8)(a >> 16);
        REG_CARDCMD[3] = (u8)(a >> 8); REG_CARDCMD[4] = (u8)a; REG_CARDCMD[5] = REG_CARDCMD[6] = REG_CARDCMD[7] = 0;
        REG_ROMCTRL = 0xA1586000u;              /* start, 0x200 bytes */
        int n = 0;
        while (REG_ROMCTRL & 0x80000000u) if (REG_ROMCTRL & 0x00800000u) block[n++ & 127] = REG_CARDDATA;
        if (!pass) start = block[(fat & 0x1FF) / 4];
    }
    dbg("card ");
    const char *t = (const char *)block + (start & 0x1FF);
    char line[40]; int i = 0;
    while (i < 39 && t[i] && t[i] != '\n') { line[i] = t[i]; i++; }
    line[i] = 0;
    dbg(line); dbg("\n");
}

/* screens: engine A shows a picture drawn into VRAM bank A, engine B a tiled background
 * and a sprite that the d-pad moves */
static void draw_top(void) {
    REG_VRAMCNT_A = 0x80;                       /* bank A: LCDC */
    REG_DISPCNT = 0x00020000;                   /* VRAM display, bank A */
    volatile u16 *fb = (volatile u16 *)0x06800000;
    for (int y = 0; y < 192; y++)
        for (int x = 0; x < 256; x++) {
            s32 cr = (x - 170) * 12, ci = (y - 96) * 12;    /* 10-bit fixed point, ~3 units wide */
            s32 zr = 0, zi = 0; int i = 0;
            while (i < 16) {
                s32 zr2 = (zr * zr) >> 10, zi2 = (zi * zi) >> 10;
                if (zr2 + zi2 > 4 << 10) break;
                zi = ((2 * zr * zi) >> 10) + ci;
                zr = zr2 - zi2 + cr;
                i++;
            }
            u16 c = i == 16 ? 0 : (u16)(((i * 4) & 31) | (((i * 6 + 8) & 31) << 5) | (((31 - i * 2) & 31) << 10));
            fb[y * 256 + x] = c;
        }
}

static const u16 bg_pal[4] = {0x0000, 0x7C1F, 0x03FF, 0x7FE0};
static void setup_bottom(void) {
    REG_VRAMCNT_C = 0x84;                       /* bank C: engine B BG */
    REG_VRAMCNT_D = 0x84;                       /* bank D: engine B OBJ */
    REG_DISPCNT_B = 0x00011110;                 /* mode 1 display, BG0, OBJ, 1D OBJ tiles */
    REG_BG0CNT_B = (31 << 8);                   /* tiles at 0, map at 0xF800, 4bpp */
    volatile u16 *pal = (volatile u16 *)0x05000400;
    for (int i = 0; i < 4; i++) pal[i] = bg_pal[i];
    pal[0] = 0x2108;                            /* backdrop */
    /* tiles 1-3: stripes, a checker and a frame, built in RAM then copied by DMA */
    static u32 tiles[4 * 8];
    for (int r = 0; r < 8; r++) {
        tiles[8 + r] = (r & 2) ? 0x11111111u : 0x00000000u;
        tiles[16 + r] = (r & 1) ? 0x22002200u : 0x00220022u;
        tiles[24 + r] = (r == 0 || r == 7) ? 0x33333333u : 0x30000003u;
    }
    REG_DMA3SAD = (u32)tiles; REG_DMA3DAD = 0x06200000; REG_DMA3CNT = 0x84000000u | 32;
    /* the map: DMA fill with tile 0, then a pattern */
    REG_DMAFILL3 = 0;
    REG_DMA3SAD = 0x040000EC; REG_DMA3DAD = 0x0620F800; REG_DMA3CNT = 0x85000000u | 256;
    volatile u16 *map = (volatile u16 *)0x0620F800;
    for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) map[y * 32 + x] = (u16)(((x + y) % 4));
    /* sprite: a 16x16 diamond in palette 0 */
    volatile u16 *opal = (volatile u16 *)0x05000600;
    opal[1] = 0x7FFF; opal[2] = 0x001F;
    volatile u32 *ot = (volatile u32 *)0x06600000;
    for (int ty = 0; ty < 2; ty++) for (int tx = 0; tx < 2; tx++) for (int r = 0; r < 8; r++) {
        u32 row = 0;
        for (int c = 0; c < 8; c++) {
            int px = tx * 8 + c - 8, py = ty * 8 + r - 8;
            int d = (px < 0 ? -px - 1 : px) + (py < 0 ? -py - 1 : py);
            u32 v = d < 6 ? 2 : d < 8 ? 1 : 0;
            row |= v << (c * 4);
        }
        ot[(ty * 2 + tx) * 8 + r] = row;
    }
    volatile u16 *oam = (volatile u16 *)0x07000400;
    for (int i = 0; i < 128; i++) oam[i * 4] = 0x0200;    /* hide all */
}

static void place_sprite(int x, int y) {
    volatile u16 *oam = (volatile u16 *)0x07000400;
    oam[0] = (u16)(y & 0xFF);
    oam[1] = (u16)((x & 0x1FF) | (1 << 14));    /* 16x16 */
    oam[2] = 0;
}

int main(void) {
    dbg("dsnative hardware test\n");
    run_suite();
    static u32 words[64];
    for (int i = 0; i < 64; i++) words[i] = (u32)i * 0x9E3779B9u;
    dbg("itcm "); dbg_hex(itcm_hash(words, 64)); dbg("\n");
    test_hw_math();
    test_card();
    REG_POWCNT1 = 0x820F;
    draw_top();
    setup_bottom();
    REG_IE = IRQ_VBLANK | IRQ_TIMER0;
    REG_IF = ~0u;
    REG_DISPSTAT = 1 << 3;                      /* VBlank interrupt */
    REG_TM0CNT_L = (u16)(0x10000 - 1000);
    REG_TM0CNT_H = 0xC1;                        /* on, interrupt, F/64 */
    REG_IME = 1;
    test_ipc();
    int x = 120, y = 88;
    for (int frame = 0; ; frame++) {
        vblank_wait();
        u16 k = (u16)~REG_KEYINPUT;
        if (k & (1 << 4)) x += 2;
        if (k & (1 << 5)) x -= 2;
        if (k & (1 << 6)) y -= 2;
        if (k & (1 << 7)) y += 2;
        place_sprite(x, y);
        REG_BG0HOFS_B = (u16)frame;
        REG_BG0VOFS_B = (u16)(frame / 2);
        if (frame == 30) {
            u32 t = timer_irqs;
            dbg("frames "); dbg_hex(vblanks); dbg("\n");
            dbg("timer "); dbg(t > 230 && t < 300 ? "ok" : "FAIL"); dbg(" "); dbg_hex(t); dbg("\n");
        }
        if (frame == 60) { dbg("sprite "); dbg_hex((u32)x); dbg(" "); dbg_hex((u32)y); dbg("\n"); dbg("DONE\n"); }
    }
}
