/* The 3D engine's textures, polygon modes and effects (gpu3d.c, gpu3d_render.c), driven through the
 * ARM9's bus like a game: VRAM banks mapped as texture image and palette memory, the seven texture
 * formats, their sizes and addressing, the texture matrix modes and perspective, then modulation,
 * decal, toon, highlight and shadow polygons, lighting, fog, edge marking, the rear-plane bitmap
 * and anti-aliasing. Every expected number comes from GBATEK's formulas written out by hand here,
 * not from the renderer; the places where GBATEK is silent and the renderer guesses are checked
 * only where a guess cannot change the result, and listed in docs/design.md. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nds.h"

static Nds *s;
static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("%s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define IO 0x04000000u
enum { DISP3DCNT = 0x60, EDGE_COLOR = 0x330, ALPHA_TEST_REF = 0x340, CLEAR_COLOR = 0x350, CLEAR_DEPTH = 0x354,
       CLRIMAGE_OFFSET = 0x356, FOG_COLOR = 0x358, FOG_OFFSET = 0x35C, FOG_TABLE = 0x360, TOON_TABLE = 0x380, GXFIFO = 0x400 };
enum { MTX_MODE = 0x10, MTX_PUSH, MTX_POP, MTX_STORE, MTX_RESTORE, MTX_IDENTITY, MTX_LOAD_4x4, MTX_LOAD_4x3, MTX_MULT_4x4,
       MTX_MULT_4x3, MTX_MULT_3x3, MTX_SCALE, MTX_TRANS,
       COLOR = 0x20, NORMAL, TEXCOORD, VTX_16, VTX_10, VTX_XY, VTX_XZ, VTX_YZ, VTX_DIFF, POLYGON_ATTR, TEXIMAGE_PARAM, PLTT_BASE,
       DIF_AMB = 0x30, SPE_EMI, LIGHT_VECTOR, LIGHT_COLOR, SHININESS,
       BEGIN_VTXS = 0x40, END_VTXS, SWAP_BUFFERS = 0x50, VIEWPORT = 0x60 };

static void reg(u32 off, u32 v) { wr32(&s->arm9, IO + off, v); }
static void reg16(u32 off, u32 v) { wr16(&s->arm9, IO + off, v); }
static void cmd(int id, u32 p) { wr32(&s->arm9, IO + GXFIFO + (u32)id * 4, p); }
static void cmd0(int id) { cmd(id, 0); }
static s32 F(double v) { return (s32)lrint(v * 4096); }           /* 20.12 */
static u32 xy(double x, double y) { return (u32)(u16)(s16)F(x) | (u32)(u16)(s16)F(y) << 16; }
static u32 rgb5(u32 r, u32 g, u32 b) { return r | g << 5 | b << 10; }
static u32 edge_color(u32 k) { return rgb5(31 - 3 * k, 4 * k, 7 + k); }       /* EDGE_COLOR entry k, distinct from the colours the tests draw */

enum { ATTR_FRONT = 1 << 7, ATTR_BACK = 1 << 6, ATTR_DEPTH = 1 << 11, ATTR_FOG = 1 << 15 };
static u32 attr(u32 alpha, u32 id, u32 flags) { return flags | alpha << 16 | id << 24; }
#define SOLID attr(31, 0, ATTR_FRONT | ATTR_BACK)
#define OPAQUE 0x8000u

static void fresh(void) {
    static u8 rom[0x1000];
    if (s) nds_shutdown(s);
    s = nds_create(rom, sizeof rom);
    cmd(VIEWPORT, 0xBFFF0000u);
}

/* the default scene: identity matrices, full viewport, opaque black rear plane, textures and
 * alpha blending on. NDC x is pixel (x + 1) * 128, NDC y is row 192 - (y + 1) * 96. */
static void scene(u32 disp) {
    fresh();
    cmd(MTX_MODE, 0); cmd0(MTX_IDENTITY);
    cmd(MTX_MODE, 1); cmd0(MTX_IDENTITY);
    reg(CLEAR_COLOR, 31u << 16);
    reg(CLEAR_DEPTH, 0x7FFF);
    reg(DISP3DCNT, disp);
}
static void color(u32 r, u32 g, u32 b) { cmd(COLOR, rgb5(r, g, b)); }
static void vtx(double x, double y, double z) { cmd(VTX_16, xy(x, y)); cmd(VTX_16, (u32)(u16)(s16)F(z)); }
static void quad(double x0, double y0, double x1, double y1, double z) {    /* counter-clockwise from the lower left */
    cmd(BEGIN_VTXS, 1); vtx(x0, y0, z); vtx(x1, y0, z); vtx(x1, y1, z); vtx(x0, y1, z); cmd0(END_VTXS);
}
static void show(u32 swap_bits) {
    cmd(SWAP_BUFFERS, swap_bits);
    gpu3d_line(s, 191);
    gpu3d_render(s);
}
static u32 px(int x, int y, u32 *a) {
    u16 line[256]; u8 al[256];
    gpu3d_bg0_line(s, y, line, al);
    if (a) *a = al[x];
    return line[x];
}
static u32 pixel(int x, int y) { return px(x, y, NULL); }

/* ---- texture memory ------------------------------------------------------------------------- */
/* Banks A-D as texture slots 0-3 and bank E as the first four palette slots, set through VRAMCNT
 * on the bus; the test pokes the bytes where the banks keep them. */
static const u32 bank_off[9] = {0x00000, 0x20000, 0x40000, 0x60000, 0x80000, 0x90000, 0x94000, 0x98000, 0xA0000};
static void vramcnt(int bank, u32 v) { wr8(&s->arm9, IO + 0x240 + (u32)bank, v); }
static void texture_vram(void) {
    vramcnt(0, 0x83); vramcnt(1, 0x8B); vramcnt(2, 0x93); vramcnt(3, 0x9B);     /* MST 3, offset (bits 3-4) = the slot */
    vramcnt(4, 0x83);
}
static u8 *tex_mem(u32 addr) { return s->vram + bank_off[(addr >> 17) & 3] + (addr & 0x1FFFF); }
static void put8(u32 addr, u32 v) { *tex_mem(addr) = (u8)v; }
static void put16(u32 addr, u32 v) { put8(addr, v & 255); put8(addr + 1, v >> 8); }
static void pal(u32 addr, u32 c) { s->vram[bank_off[4] + addr] = (u8)c; s->vram[bank_off[4] + addr + 1] = (u8)(c >> 8); }

/* palette entry k's colour: 15 bits that differ for every k below 1024 (the multiplier is odd) */
static u32 C(u32 k) { return (k * 0x9E5 + 0x123) & 0x7FFF; }
static void fill_palette(u32 addr, int n) { for (int k = 0; k < n; k++) pal(addr + 2 * (u32)k, C((u32)k)); }

typedef struct {
    int fmt, wlog, hlog;                /* format 1..7, size 8 << wlog by 8 << hlog texels */
    u32 base, pltt;                     /* image address (bytes), PLTT_BASE register */
    int rep_s, rep_t, flip_s, flip_t, trans0, tcmode;
} Tex;
static u32 teximage(const Tex *t) {
    return t->base / 8 | (u32)t->rep_s << 16 | (u32)t->rep_t << 17 | (u32)t->flip_s << 18 | (u32)t->flip_t << 19 |
           (u32)t->wlog << 20 | (u32)t->hlog << 23 | (u32)t->fmt << 26 | (u32)t->trans0 << 29 | (u32)t->tcmode << 30;
}

/* A quad over the pixels [x0, x1) x [y0, y1) with the texture mapped from (s0, t0) at its top left
 * to (s1, t1) at its bottom right (texels), white vertices, drawn with POLYGON_ATTR a, in the
 * scene's identity space with a viewport 256 x 128 whose pixel steps are exact in 4.12. */
static void tex_quad(const Tex *t, u32 a, int x0, int y0, int x1, int y1, double s0, double t0, double s1, double t1) {
    cmd(VIEWPORT, 64u << 8 | 255u << 16 | 191u << 24);
    cmd(POLYGON_ATTR, a);
    cmd(TEXIMAGE_PARAM, teximage(t));
    cmd(PLTT_BASE, t->pltt);
    color(31, 31, 31);
    cmd(BEGIN_VTXS, 1);
    int px4[4] = {x0, x1, x1, x0}, py4[4] = {y1, y1, y0, y0};
    double ss[4] = {s0, s1, s1, s0}, tt[4] = {t1, t1, t0, t0};
    for (int i = 0; i < 4; i++) {
        cmd(TEXCOORD, (u32)(u16)(s16)lrint(ss[i] * 16) | (u32)(u16)(s16)lrint(tt[i] * 16) << 16);
        cmd(VTX_16, (u32)(u16)(s16)(32 * px4[i] - 4096) | (u32)(u16)(s16)(4096 - 64 * py4[i]) << 16);
        cmd(VTX_16, 0);
    }
    cmd0(END_VTXS);
}

/* the rear plane colour tests see where a texel is transparent */
#define REAR rgb5(3, 5, 7)
static void tex_scene(void) {
    scene(1 | 8);
    reg(CLEAR_COLOR, REAR | 31u << 16);
    texture_vram();
}

/* An 8 x 8 texture magnified 8 times: texel (x, y) is at pixels [8x, 8x + 8) x [8y, 8y + 8) */
static u32 texel_px(int x, int y) { return pixel(8 * x + 4, 8 * y + 4); }

/* ---- the mapping ---------------------------------------------------------------------------- */
static void test_memory(void) {
    /* 256-colour texture read from each of the four slots, each by the bank mapped there: the
     * bank's offset bits pick the slot whichever bank it is */
    for (int slot = 0; slot < 4; slot++) {
        for (int order = 0; order < 2; order++) {
            tex_scene();
            if (order) {                /* banks A-D mapped to the slots in the opposite order */
                for (int b = 0; b < 4; b++) vramcnt(b, 0x83 | (u32)(3 - b) << 3);
            }
            /* which bank holds slot `slot`: bank b with (OFS = slot) */
            int bank = order ? 3 - slot : slot;
            u32 base = (u32)slot * 0x20000 + 0x1000;
            s->vram[bank_off[bank] + 0x1000] = 9;
            pal(18, C(9));
            Tex t = {.fmt = 4, .wlog = 0, .hlog = 0, .base = base, .pltt = 0};
            tex_quad(&t, SOLID, 0, 0, 64, 64, 0, 0, 8, 8);
            show(0);
            u32 want = OPAQUE | C(9);
            /* texel 0 of an 8x8 texture is byte 0: index 9 -> palette entry 9 */
            CHECK(texel_px(0, 0) == want, "slot %d (%s order): texel is %04x, wanted %04x", slot, order ? "reversed" : "straight", texel_px(0, 0), want);
        }
    }
    /* a slot with no bank reads zero: index 0, palette entry 0 */
    tex_scene();
    vramcnt(2, 0);
    pal(0, C(1));
    Tex t = {.fmt = 4, .wlog = 0, .hlog = 0, .base = 0x40000};
    tex_quad(&t, SOLID, 0, 0, 64, 64, 0, 0, 8, 8);
    show(0);
    CHECK(texel_px(2, 2) == (OPAQUE | C(1)), "unmapped slot reads as zero: %04x", texel_px(2, 2));

    /* palette memory: bank E is slots 0-3, banks F and G take slot (OFS & 1) + 4 * (OFS >> 1) */
    static const struct { int bank, ofs; u32 slot; } pm[] = {
        {5, 0, 0}, {5, 1, 1}, {5, 2, 4}, {5, 3, 5}, {6, 0, 0}, {6, 1, 1}, {6, 2, 4}, {6, 3, 5},
    };
    for (unsigned i = 0; i < sizeof pm / sizeof *pm; i++) {
        tex_scene();
        vramcnt(4, 0);                  /* bank E out of the way */
        vramcnt(pm[i].bank, 0x83 | (u32)pm[i].ofs << 3);
        s->vram[bank_off[pm[i].bank] + 6] = (u8)(C(77) & 255);          /* entry 3 of the bank's 16 KB */
        s->vram[bank_off[pm[i].bank] + 7] = (u8)(C(77) >> 8);
        memset(s->vram + bank_off[0], 3, 64);
        Tex t = {.fmt = 4, .wlog = 0, .hlog = 0, .base = 0, .pltt = pm[i].slot * 0x4000 / 16};
        tex_quad(&t, SOLID, 0, 0, 64, 64, 0, 0, 8, 8);
        show(0);
        CHECK(texel_px(1, 1) == (OPAQUE | C(77)), "palette bank %c ofs %d (slot %u): %04x", 'A' + pm[i].bank, pm[i].ofs, pm[i].slot, texel_px(1, 1));
    }
    /* bank E's four slots follow one another: PLTT_BASE steps of 16 bytes reach 0x10000 */
    tex_scene();
    pal(0xC000 + 2 * 5, C(21));
    memset(s->vram + bank_off[0], 5, 64);
    Tex t2 = {.fmt = 4, .base = 0, .pltt = 0xC000 / 16};
    tex_quad(&t2, SOLID, 0, 0, 64, 64, 0, 0, 8, 8);
    show(0);
    CHECK(texel_px(3, 3) == (OPAQUE | C(21)), "bank E slot 3: %04x", texel_px(3, 3));
    /* a mapping that changes between frames is followed: with bank A back in LCDC mode its slot
     * is gone, so every texel is index 0, palette entry 0 (black) */
    vramcnt(0, 0x80);
    tex_quad(&t2, SOLID, 0, 0, 64, 64, 0, 0, 8, 8);
    show(0);
    CHECK(texel_px(3, 3) == OPAQUE, "unmapped again: %04x", texel_px(3, 3));
    vramcnt(0, 0x83);
    tex_quad(&t2, SOLID, 0, 0, 64, 64, 0, 0, 8, 8);
    show(0);
    CHECK(texel_px(3, 3) == (OPAQUE | C(21)), "mapped again: %04x", texel_px(3, 3));
}

/* ---- the seven formats ---------------------------------------------------------------------- */
static void test_formats(void) {
    /* format 4, 256 colours, one byte per texel; colour 0 transparent when bit 29 is set */
    for (int trans0 = 0; trans0 < 2; trans0++) {
        tex_scene();
        fill_palette(0x20, 256);
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) put8(0x100 + (u32)(y * 8 + x), (u32)(x * 29 + y * 17 + 1) & 255);
        put8(0x100 + 2 * 8 + 3, 0);
        Tex t = {.fmt = 4, .base = 0x100, .pltt = 2, .trans0 = trans0};
        tex_quad(&t, SOLID, 0, 0, 64, 64, 0, 0, 8, 8);
        show(0);
        int bad = 0;
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
            u32 idx = (y == 2 && x == 3) ? 0 : (u32)(x * 29 + y * 17 + 1) & 255;
            u32 want = (idx == 0 && trans0) ? REAR : C(idx);
            if (texel_px(x, y) != (OPAQUE | want)) { bad++; if (bad < 4) printf("  fmt4 trans0=%d texel %d,%d: %04x want %04x\n", trans0, x, y, texel_px(x, y), want); }
        }
        CHECK(!bad, "format 4 (trans0 %d): %d texels wrong", trans0, bad);
    }

    /* format 3, 16 colours, 4 bits a texel, the first in the low bits */
    for (int trans0 = 0; trans0 < 2; trans0++) {
        tex_scene();
        fill_palette(0x40, 16);
        for (int i = 0; i < 32; i++) {
            u32 lo = (u32)((2 * i) % 8 + 2 * ((2 * i) / 8)) & 15, hi = (u32)((2 * i + 1) % 8 + 2 * ((2 * i + 1) / 8)) & 15;
            put8(0x800 + (u32)i, lo | hi << 4);
        }
        Tex t = {.fmt = 3, .base = 0x800, .pltt = 4, .trans0 = trans0};
        tex_quad(&t, SOLID, 0, 0, 64, 64, 0, 0, 8, 8);
        show(0);
        int bad = 0;
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
            u32 idx = (u32)(x + 2 * y) & 15, want = (idx == 0 && trans0) ? REAR : C(idx);
            if (texel_px(x, y) != (OPAQUE | want)) bad++;
        }
        CHECK(!bad, "format 3 (trans0 %d): %d texels wrong", trans0, bad);
    }

    /* format 2, 4 colours, 2 bits a texel; PLTT_BASE steps are 8 bytes for this format only */
    for (int trans0 = 0; trans0 < 2; trans0++) {
        tex_scene();
        fill_palette(0x18, 4);          /* PLTT_BASE 3 * 8 */
        for (int i = 0; i < 16; i++) {
            u32 b = 0;
            for (int k = 0; k < 4; k++) { int n = 4 * i + k; b |= (u32)(n % 8 + n / 8) % 4 << (2 * k); }
            put8(0x200 + (u32)i, b);
        }
        Tex t = {.fmt = 2, .base = 0x200, .pltt = 3, .trans0 = trans0};
        tex_quad(&t, SOLID, 0, 0, 64, 64, 0, 0, 8, 8);
        show(0);
        int bad = 0;
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
            u32 idx = (u32)(x + y) % 4, want = (idx == 0 && trans0) ? REAR : C(idx);
            if (texel_px(x, y) != (OPAQUE | want)) bad++;
        }
        CHECK(!bad, "format 2 (trans0 %d): %d texels wrong", trans0, bad);
    }

    /* format 7, direct colour: bit 15 is alpha, and trans0 does not apply */
    for (int trans0 = 0; trans0 < 2; trans0++) {
        tex_scene();
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++)
            put16(0x1000 + 2 * (u32)(y * 8 + x), rgb5((u32)(x * 4) & 31, (u32)(y * 4) & 31, (u32)((x + y) * 2) & 31) | ((x + y) & 1 ? 0x8000u : 0));
        put16(0x1000, 0x8000);          /* texel 0 is opaque black: colour 0 of nothing */
        Tex t = {.fmt = 7, .base = 0x1000, .trans0 = trans0};
        tex_quad(&t, SOLID, 0, 0, 64, 64, 0, 0, 8, 8);
        show(0);
        int bad = 0;
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
            u32 want = ((x + y) & 1 || (!x && !y)) ? rgb5((u32)(x * 4) & 31, (u32)(y * 4) & 31, (u32)((x + y) * 2) & 31) : REAR;
            if (!x && !y) want = 0;
            if (texel_px(x, y) != (OPAQUE | want)) bad++;
        }
        CHECK(!bad, "format 7 (trans0 %d): %d texels wrong", trans0, bad);
    }

    /* format 1, A3I5: alpha 0..7 becomes 0, 4, 9, 13, 18, 22, 27, 31 of 31 (a * 4 + a / 2) */
    static const u32 a3to5[8] = {0, 4, 9, 13, 18, 22, 27, 31};
    tex_scene();
    for (int k = 0; k < 32; k++) pal(2 * (u32)k, 0x7FFF);       /* white: the colour is the alpha's alone */
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) put8(0x300 + (u32)(y * 8 + x), (u32)x << 5 | (u32)((y * 3 + x) & 31));
    Tex t1 = {.fmt = 1, .base = 0x300, .pltt = 0};
    tex_quad(&t1, SOLID, 0, 0, 64, 64, 0, 0, 8, 8);
    show(0);
    for (int x = 0; x < 8; x++) {
        u32 a = 0;
        u32 got = px(8 * x + 4, 28, &a);
        if (x == 0) CHECK(got == (OPAQUE | REAR), "A3I5 alpha 0 is transparent: %04x", got);
        else {
            /* over the rear plane (3, 5, 7), not black: (c6 * (A + 1) + rear6 * (31 - A)) >> 5, in 6 bits */
            u32 w[3] = {0}, g5[3] = {got & 31, (got >> 5) & 31, (got >> 10) & 31}, r5[3] = {3, 5, 7};
            for (int k = 0; k < 3; k++) w[k] = ((63 * (a3to5[x] + 1) + (r5[k] * 2 + 1) * (31 - a3to5[x])) >> 5) >> 1;
            CHECK(g5[0] == w[0] && g5[1] == w[1] && g5[2] == w[2], "A3I5 alpha %d (%u): pixel %04x, wanted %u %u %u", x, a3to5[x], got, w[0], w[1], w[2]);
            if (x == 7) CHECK(got == (OPAQUE | 0x7FFF), "A3I5 alpha 7 is solid white: %04x", got);
        }
    }
    /* the palette index is the low 5 bits: texel (1, 2) has index 7 */
    tex_scene();
    fill_palette(0, 32);
    for (int i = 0; i < 64; i++) put8(0x300 + (u32)i, 7u << 5 | (u32)(i & 31));
    tex_quad(&t1, SOLID, 0, 0, 64, 64, 0, 0, 8, 8);
    show(0);
    CHECK(texel_px(1, 2) == (OPAQUE | C(((1 + 2 * 8) & 31))), "A3I5 index %04x", texel_px(1, 2));

    /* format 6, A5I3: alpha is the top 5 bits, the index the low 3 */
    tex_scene();
    for (int k = 0; k < 8; k++) pal(2 * (u32)k, 0x7FFF);
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) put8(0x300 + (u32)(y * 8 + x), (u32)(y * 4 + x / 2) << 3 | (u32)(x & 7));
    Tex t6 = {.fmt = 6, .base = 0x300};
    tex_quad(&t6, SOLID, 0, 0, 64, 64, 0, 0, 8, 8);
    show(0);
    for (int y = 0; y < 8; y++) {
        u32 a5 = (u32)(y * 4);          /* x / 2 of 0 */
        u32 got = texel_px(0, y);
        if (a5 == 0) { CHECK(got == (OPAQUE | REAR), "A5I3 alpha 0: %04x", got); continue; }
        u32 w[3], r5[3] = {3, 5, 7};
        for (int k = 0; k < 3; k++) w[k] = ((63 * (a5 + 1) + (r5[k] * 2 + 1) * (31 - a5)) >> 5) >> 1;
        CHECK((got & 31) == w[0] && ((got >> 5) & 31) == w[1] && ((got >> 10) & 31) == w[2], "A5I3 alpha %u: %04x", a5, got);
    }
    tex_scene();
    fill_palette(0, 8);
    for (int i = 0; i < 64; i++) put8(0x300 + (u32)i, 31u << 3 | (u32)(i % 8));
    tex_quad(&t6, SOLID, 0, 0, 64, 64, 0, 0, 8, 8);
    show(0);
    CHECK(texel_px(5, 3) == (OPAQUE | C(5)), "A5I3 index: %04x", texel_px(5, 3));
}

/* ---- format 5: 4x4 compressed --------------------------------------------------------------- */
static u32 mix5(u32 c0, u32 c1, u32 w0, u32 w1, u32 div) {      /* GBATEK's (Color0 * w0 + Color1 * w1) / div per 5-bit component */
    u32 o = 0;
    for (int k = 0; k < 15; k += 5) o |= ((((c0 >> k) & 31) * w0 + ((c1 >> k) & 31) * w1) / div) << k;
    return o;
}

static void test_compressed(void) {
    /* an 8 x 8 texture of four blocks, one per palette index mode; texel (x, y) of a block holds
     * the 2-bit value (x + y) & 3 (block-relative). base in slot 0 and slot 2 (a different place
     * for its palette index halfwords: slot 1's first 64 KB and its second) */
    static const u32 bases[2] = {0x400, 0x40000 + 0x80};
    for (int which = 0; which < 2; which++) {
        tex_scene();
        u32 base = bases[which];
        /* palette: PLTT_BASE 1 is 16 bytes in; block b reads its colours at pltt * 16 + offset * 4 */
        u32 pltt = 1, off[4] = {0, 2, 4, 6};          /* in 4-byte steps: palette entries 0, 4, 8 and 12 */
        for (u32 k = 0; k < 32; k++) pal(pltt * 16 + 2 * k, C(k + 3));
        /* indices: mode in bits 14-15 */
        u32 idx_base = which ? 0x20000 + 0x10000 + (base - 0x40000) / 2 : 0x20000 + base / 2;
        for (u32 b = 0; b < 4; b++) {
            put16(idx_base + 2 * b, off[b] | b << 14);
            for (u32 row = 0; row < 4; row++) {
                u32 byte = 0;
                for (u32 col = 0; col < 4; col++) byte |= ((col + row) & 3) << (2 * col);
                put8(base + 4 * b + row, byte);
            }
        }
        Tex t = {.fmt = 5, .base = base, .pltt = pltt};
        tex_quad(&t, SOLID, 0, 0, 64, 64, 0, 0, 8, 8);
        show(0);
        int bad = 0;
        for (u32 b = 0; b < 4; b++) {
            u32 pa = off[b] * 2;        /* the block's colours: palette entries 3 + pa + n */
            u32 c[4] = {C(3 + pa), C(4 + pa), C(5 + pa), C(6 + pa)};
            for (u32 ty = 0; ty < 4; ty++) for (u32 tx = 0; tx < 4; tx++) {
                u32 v = (tx + ty) & 3, want;
                switch (b) {            /* GBATEK's table of the four modes */
                case 0: want = v == 3 ? REAR : c[v]; break;
                case 1: want = v == 3 ? REAR : v == 2 ? mix5(c[0], c[1], 1, 1, 2) : c[v]; break;
                case 2: want = c[v]; break;
                default: want = v == 2 ? mix5(c[0], c[1], 5, 3, 8) : v == 3 ? mix5(c[0], c[1], 3, 5, 8) : c[v]; break;
                }
                int x = (int)((b & 1) * 4 + tx), y = (int)((b >> 1) * 4 + ty);
                if (texel_px(x, y) != (OPAQUE | want)) {
                    bad++;
                    if (bad < 5) printf("  base %#x block %u texel %u,%u: %04x, wanted %04x\n", base, b, tx, ty, texel_px(x, y), want);
                }
            }
        }
        CHECK(!bad, "compressed texture at %#x: %d texels wrong", base, bad);
    }
}

/* ---- sizes and addressing ------------------------------------------------------------------- */
/* a wide, tall or big texture: texel (x, y) is byte y * w + x with colour index (7x + 3y + 1) % 255 + 1 */
static u32 big_idx(int x, int y) { return (u32)((7 * x + 3 * y + 1) % 255) + 1; }
static void test_sizes(void) {
    static const int sizes[][2] = {{0, 0}, {1, 0}, {0, 1}, {2, 1}, {1, 3}, {3, 2}, {5, 0}, {7, 0}, {0, 5}};
    for (unsigned k = 0; k < sizeof sizes / sizeof *sizes; k++) {
        int w = 8 << sizes[k][0], h = 8 << sizes[k][1];
        tex_scene();
        fill_palette(0, 256);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) put8(0x200 + (u32)(y * w + x), big_idx(x, y));
        Tex t = {.fmt = 4, .wlog = sizes[k][0], .hlog = sizes[k][1], .base = 0x200};
        /* a window of up to 32 x 16 texels at 4 pixels a texel, from texel (w - 32, h - 16) when
         * the texture is bigger than that, so the far corner of each size is read too */
        int ww = w < 32 ? w : 32, wh = h < 16 ? h : 16, x0 = w - ww, y0 = h - wh;
        tex_quad(&t, SOLID, 0, 0, 4 * ww, 4 * wh, x0, y0, x0 + ww, y0 + wh);
        show(0);
        int bad = 0;
        for (int y = 0; y < wh; y++) for (int x = 0; x < ww; x++)
            if (pixel(4 * x + 2, 4 * y + 2) != (OPAQUE | C(big_idx(x0 + x, y0 + y)))) bad++;
        CHECK(!bad, "texture %d x %d: %d of %d texels wrong", w, h, bad, ww * wh);
    }
}

/* the reference of GBATEK's picture: clamp holds the edge texels, repeat repeats, flip mirrors each second copy */
static int ref_wrap(int i, int size, int repeat, int flip) {
    if (!repeat) return i < 0 ? 0 : i >= size ? size - 1 : i;
    int m = ((i % (2 * size)) + 2 * size) % (2 * size);
    if (flip) return m >= size ? 2 * size - 1 - m : m;
    return m % size;
}

static void test_addressing(void) {
    for (int mode = 0; mode < 16; mode++) {
        int rs = mode & 1, rt = (mode >> 1) & 1, fs = (mode >> 2) & 1, ft = (mode >> 3) & 1;
        if ((fs && !rs) || (ft && !rt)) continue;       /* flip needs repeat; without it the flip bit does nothing, checked below */
        tex_scene();
        fill_palette(0, 256);
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) put8(0x200 + (u32)(y * 8 + x), big_idx(x, y));
        Tex t = {.fmt = 4, .base = 0x200, .rep_s = rs, .rep_t = rt, .flip_s = fs, .flip_t = ft};
        /* 24 x 24 texels from (-8, -8) to (16, 16), 4 pixels a texel */
        tex_quad(&t, SOLID, 0, 0, 96, 96, -8, -8, 16, 16);
        show(0);
        int bad = 0;
        for (int y = 0; y < 24; y++) for (int x = 0; x < 24; x++) {
            u32 want = C(big_idx(ref_wrap(x - 8, 8, rs, fs), ref_wrap(y - 8, 8, rt, ft)));
            if (pixel(4 * x + 2, 4 * y + 2) != (OPAQUE | want)) bad++;
        }
        CHECK(!bad, "addressing repeat %d%d flip %d%d: %d texels wrong", rs, rt, fs, ft, bad);
    }
    /* flip without repeat does nothing: the edge texels are held */
    tex_scene();
    fill_palette(0, 256);
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) put8(0x200 + (u32)(y * 8 + x), big_idx(x, y));
    Tex t = {.fmt = 4, .base = 0x200, .flip_s = 1, .flip_t = 1};
    tex_quad(&t, SOLID, 0, 0, 96, 96, -8, -8, 16, 16);
    show(0);
    CHECK(pixel(4 * 20 + 2, 4 * 2 + 2) == (OPAQUE | C(big_idx(7, 0))), "flip without repeat: %04x", pixel(82, 10));
    /* addressing at fractions: the texel is the integer part (floor, so -0.5 is texel -1) */
    tex_scene();
    fill_palette(0, 256);
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) put8(0x200 + (u32)(y * 8 + x), big_idx(x, y));
    Tex tr = {.fmt = 4, .base = 0x200, .rep_s = 1, .rep_t = 1};
    tex_quad(&tr, SOLID, 0, 0, 64, 64, -0.5, 2.25, 7.5, 10.25);    /* 8 pixels a texel, shifted by a fraction */
    show(0);
    /* pixel 4 is at texel -0.5 + 0.5 / 8 * ... : s = -0.5 + (4.5 / 8) = 0.0625 -> texel 0; pixel 0: s = -0.4375 -> texel -1 = 7 */
    CHECK(pixel(0, 4) == (OPAQUE | C(big_idx(7, 2))) && pixel(4, 4) == (OPAQUE | C(big_idx(0, 2))), "fractional coordinates: %04x %04x", pixel(0, 4), pixel(4, 4));
}

/* ---- the texture matrix --------------------------------------------------------------------- */
/* the vertex RAM entry of the last polygon's i-th vertex */
static const G3dVertex *vertex_of(int i) {
    const G3dList *L = &s->g3d.lists[s->g3d.cur];
    return &L->vtx[L->poly[L->npoly - 1].v[i]];
}

static void tex_matrix(const s32 *m) {
    cmd(MTX_MODE, 3);
    for (int i = 0; i < 16; i++) cmd(MTX_LOAD_4x4, (u32)m[i]);
    cmd(MTX_MODE, 1);
}

static void test_texcoord_modes(void) {
    s32 m[16];
    /* mode 0: S and T as sent, whatever the matrix says */
    scene(1);
    memset(m, 0, sizeof m);
    m[0] = m[5] = F(3); m[15] = F(1);
    tex_matrix(m);
    cmd(POLYGON_ATTR, SOLID);
    cmd(TEXIMAGE_PARAM, 4u << 26 | 0u << 30);
    cmd(BEGIN_VTXS, 0);
    for (int i = 0; i < 3; i++) { cmd(TEXCOORD, (u32)(u16)(s16)(16 * (i + 1)) | (u32)(u16)(s16)(-32 * i) << 16); vtx(0.1 * i, 0.1 * (i & 1), 0); }
    CHECK(vertex_of(1)->s == 32 && vertex_of(1)->t == -32 && vertex_of(2)->s == 48, "mode 0 passes S T: %d %d", vertex_of(1)->s, vertex_of(1)->t);

    /* mode 1: (S T 1/16 1/16) * matrix, at TEXCOORD: scaling by 2 and 3 on the diagonal, a skew
     * of T into S, and translations of m[12] (S) and m[13] (T) of 16.0, which is one texel in
     * the 12.4 result */
    scene(1);
    memset(m, 0, sizeof m);
    m[0] = F(2); m[1] = F(0.5); m[4] = F(1.5); m[5] = F(3); m[12] = F(16); m[13] = F(-32); m[15] = F(1);
    tex_matrix(m);
    cmd(POLYGON_ATTR, SOLID);
    cmd(TEXIMAGE_PARAM, 4u << 26 | 1u << 30);
    cmd(BEGIN_VTXS, 0);
    for (int i = 0; i < 3; i++) { cmd(TEXCOORD, (u32)(u16)(s16)(16 * (i + 1)) | (u32)(u16)(s16)(16 * (2 * i + 1)) << 16); vtx(0.1 * i, 0.1 * (i & 1), 0); }
    /* S = 16, T = 16: S' = 16*2 + 16*1.5 + 16 = 72, T' = 16*0.5 + 16*3 - 32 = 24 */
    CHECK(vertex_of(0)->s == 72 && vertex_of(0)->t == 24, "mode 1: %d %d, wanted 72 24", vertex_of(0)->s, vertex_of(0)->t);
    /* S = 32, T = 48: 64 + 72 + 16 = 152 and 16 + 144 - 32 = 128 */
    CHECK(vertex_of(1)->s == 152 && vertex_of(1)->t == 128, "mode 1: %d %d, wanted 152 128", vertex_of(1)->s, vertex_of(1)->t);

    /* mode 1 and the 16-bit result: it wraps rather than saturates (S' is 1 + 11 + 4 bits) */
    scene(1);
    memset(m, 0, sizeof m);
    m[0] = F(8); m[5] = F(1); m[15] = F(1);
    tex_matrix(m);
    cmd(POLYGON_ATTR, SOLID);
    cmd(TEXIMAGE_PARAM, 4u << 26 | 1u << 30);
    cmd(BEGIN_VTXS, 0);
    for (int i = 0; i < 3; i++) { cmd(TEXCOORD, (u32)(u16)(s16)(0x1000) | 0); vtx(0.1 * i, 0.1 * (i & 1), 0); }
    CHECK(vertex_of(0)->s == (s16)(0x1000 * 8), "mode 1 wraps to 16 bits: %d", vertex_of(0)->s);

    /* mode 2: (Nx Ny Nz 1.0) * matrix with S T as the last row, at NORMAL. A unit normal along x
     * times 8.0 in m[0] is 8 texels away from S: texel 8 = 128 */
    scene(1);
    memset(m, 0, sizeof m);
    m[0] = F(16); m[1] = F(-4); m[4] = F(2); m[5] = F(4); m[8] = F(1); m[9] = F(1); m[15] = F(1);
    tex_matrix(m);
    cmd(POLYGON_ATTR, SOLID);
    cmd(TEXIMAGE_PARAM, 4u << 26 | 2u << 30);
    cmd(BEGIN_VTXS, 0);
    for (int i = 0; i < 3; i++) {
        cmd(TEXCOORD, (u32)(u16)(s16)(16 * (i + 1)) | (u32)(u16)(s16)(16 * i) << 16);
        cmd(NORMAL, (u32)(256) | (u32)(128) << 10);          /* (0.5, 0.25, 0) */
        vtx(0.1 * i, 0.1 * (i & 1), 0);
    }
    /* S' = 0.5 * 16 + 0.25 * 2 texels + S = 8.5 texels = 136 plus S (16, 32, 48): 152, 168, 184;
     * T' = 0.5 * -4 + 0.25 * 4 = -1 texel = -16 plus T (0, 16, 32) */
    CHECK(vertex_of(0)->s == 152 && vertex_of(0)->t == -16, "mode 2 first vertex: %d %d", vertex_of(0)->s, vertex_of(0)->t);
    CHECK(vertex_of(2)->s == 184 && vertex_of(2)->t == 16, "mode 2 last vertex: %d %d", vertex_of(2)->s, vertex_of(2)->t);

    /* mode 3: (Vx Vy Vz 1.0) * matrix with S T last, at the vertex. The projection shrinks the
     * vertices (1, 2, 0.5) to a quarter so they stay in view */
    scene(1);
    cmd(MTX_MODE, 0);
    for (int i = 0; i < 3; i++) cmd(MTX_SCALE, (u32)F(0.25));
    memset(m, 0, sizeof m);
    m[0] = F(1); m[1] = F(0.25); m[4] = F(1); m[5] = F(0.5); m[8] = F(2); m[9] = F(-1); m[15] = F(1);
    tex_matrix(m);
    cmd(POLYGON_ATTR, SOLID);
    cmd(TEXIMAGE_PARAM, 4u << 26 | 3u << 30);
    cmd(BEGIN_VTXS, 0);
    cmd(TEXCOORD, 16u | 32u << 16);
    cmd(VTX_16, (u32)(u16)(s16)F(1) | (u32)(u16)(s16)F(2) << 16); cmd(VTX_16, (u32)(u16)(s16)F(0.5));
    cmd(VTX_16, (u32)(u16)(s16)F(0.3) | (u32)(u16)(s16)F(0.1) << 16); cmd(VTX_16, 0);
    cmd(VTX_16, (u32)(u16)(s16)F(0.1) | (u32)(u16)(s16)F(0.5) << 16); cmd(VTX_16, 0);
    /* S' = 1 * 1 + 2 * 1 + 0.5 * 2 = 4 texels = 64, plus S = 16: 80; T' = 1 * 0.25 + 2 * 0.5 - 0.5 * 1 = 0.75 texel = 12, plus T = 32: 44 */
    CHECK(vertex_of(0)->s == 80 && vertex_of(0)->t == 44, "mode 3 first vertex: %d %d, wanted 80 44", vertex_of(0)->s, vertex_of(0)->t);

    /* mode 1 on a drawn quad: scale by 1/2 magnifies, a translation of one texel shifts the picture */
    tex_scene();
    fill_palette(0, 256);
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) put8(0x200 + (u32)(y * 8 + x), big_idx(x, y));
    memset(m, 0, sizeof m);
    m[0] = F(1); m[5] = F(1); m[12] = F(16); m[13] = F(32); m[15] = F(1);      /* + (1, 2) texels */
    tex_matrix(m);
    Tex t = {.fmt = 4, .base = 0x200, .rep_s = 1, .rep_t = 1, .tcmode = 1};
    tex_quad(&t, SOLID, 0, 0, 64, 64, 0, 0, 8, 8);
    show(0);
    CHECK(texel_px(0, 0) == (OPAQUE | C(big_idx(1, 2))) && texel_px(7, 7) == (OPAQUE | C(big_idx(0, 1))),
          "translated texture: %04x %04x", texel_px(0, 0), texel_px(7, 7));
}

/* ---- perspective ---------------------------------------------------------------------------- */
/* A wall running away from the camera: clip w = 1 + z, the left edge at w = 1, the right at w = 4
 * (z = 3), both on screen pixels 64 and 192, with the texture 0..8 texels across. The texture
 * coordinate at a pixel of screen fraction f is the perspective-correct (f / 4) / ((1 - f) + f / 4)
 * of the way; linear interpolation would say f. */
static void test_perspective(void) {
    tex_scene();
    fill_palette(0, 256);
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) put8(0x200 + (u32)(y * 8 + x), big_idx(x, y));
    cmd(MTX_MODE, 0);
    s32 p[16] = {0};
    p[0] = p[5] = F(1); p[11] = F(1); p[15] = F(1);     /* w = 1 + z, z' = 0 */
    for (int i = 0; i < 16; i++) cmd(MTX_LOAD_4x4, (u32)p[i]);
    cmd(MTX_MODE, 1);
    Tex t = {.fmt = 4, .base = 0x200};
    cmd(POLYGON_ATTR, SOLID); cmd(TEXIMAGE_PARAM, teximage(&t)); cmd(PLTT_BASE, 0);
    color(31, 31, 31);
    cmd(BEGIN_VTXS, 1);
    /* x = screen x * w: -0.5 and +0.5 on the screen are -0.5 * 1 and 0.5 * 4; y = +-0.5 * w too */
    double xs[4] = {-0.5, 2.0, 2.0, -0.5}, ys[4] = {-0.5, -2.0, 2.0, 0.5}, zs[4] = {0, 3, 3, 0}, ss[4] = {0, 8, 8, 0}, tt[4] = {8, 8, 0, 0};
    for (int i = 0; i < 4; i++) {
        cmd(TEXCOORD, (u32)(u16)(s16)lrint(ss[i] * 16) | (u32)(u16)(s16)lrint(tt[i] * 16) << 16);
        vtx(xs[i], ys[i], zs[i]);
    }
    cmd0(END_VTXS);
    show(0);
    int checked = 0, bad = 0, linear_differs = 0;
    for (int x = 70; x < 190; x += 3) {
        double f = (x + 0.5 - 64) / 128.0, u = (f / 4) / ((1 - f) + f / 4) * 8;
        if (fabs(u - floor(u)) < 0.08 || fabs(u - floor(u)) > 0.92) continue;       /* near a texel boundary: skip */
        int tx = (int)floor(u);
        /* T is linear down a column (w is constant there): row 96 is 48.5 / 96 of the way, texel 4 */
        if (pixel(x, 96) != (OPAQUE | C(big_idx(tx, 4)))) {
            bad++;
            if (bad < 5) printf("  x %d: u %.3f texel %d: %04x\n", x, u, tx, pixel(x, 96));
        }
        if ((int)floor(f * 8) != tx) linear_differs++;
        checked++;
    }
    CHECK(checked > 30 && !bad, "perspective-correct texture: %d of %d samples wrong", bad, checked);
    CHECK(linear_differs > 20, "the test would not tell linear from perspective-correct (%d)", linear_differs);
}

/* ---- polygon modes -------------------------------------------------------------------------- */
static u32 e6(u32 c5) { return c5 ? c5 * 2 + 1 : 0; }          /* GBATEK: X * 2 + 1, zero stays zero */
static u32 modulate6(u32 t, u32 v) { return ((t + 1) * (v + 1) - 1) / 64; }
static u32 c5(u32 c, int k) { return (c >> (5 * k)) & 31; }

/* a direct-colour 8 x 8 texture, every texel the colour c with alpha bit 15 set; the quad shows it whole */
static Tex solid_texture(u32 c) {
    for (int i = 0; i < 64; i++) put16(0x1000 + 2 * (u32)i, c | 0x8000);
    Tex t = {.fmt = 7, .base = 0x1000};
    return t;
}

/* a 64 x 64 pixel quad at the top left with vertex colour (r, g, b), for a texture of one colour all over */
static void flat_quad(const Tex *t, u32 a, u32 r, u32 g, u32 b) {
    cmd(VIEWPORT, 64u << 8 | 255u << 16 | 191u << 24);
    cmd(POLYGON_ATTR, a);
    cmd(TEXIMAGE_PARAM, teximage(t));
    cmd(PLTT_BASE, t->pltt);
    color(r, g, b);
    cmd(BEGIN_VTXS, 1);
    for (int i = 0; i < 4; i++) {
        cmd(TEXCOORD, 0);
        cmd(VTX_16, (u32)(u16)(s16)(32 * (i == 1 || i == 2 ? 64 : 0) - 4096) | (u32)(u16)(s16)(4096 - 64 * (i < 2 ? 64 : 0)) << 16);
        cmd(VTX_16, 0);
    }
    cmd0(END_VTXS);
}

static void test_modulation(void) {
    /* R = ((Rt + 1) * (Rv + 1) - 1) / 64 on 6-bit values: (31, 16, 0) by (16, 31, 8) is (33, 33, 0) of 63 */
    tex_scene();
    Tex tt = solid_texture(rgb5(31, 16, 0));
    flat_quad(&tt, SOLID, 16, 31, 8);
    show(0);
    CHECK(pixel(30, 30) == (OPAQUE | rgb5(16, 16, 0)), "modulation: %04x, wanted %04x", pixel(30, 30), rgb5(16, 16, 0));
    /* a grid of texel and vertex colours against the formula */
    int bad = 0;
    for (u32 tr = 0; tr < 32; tr += 7) for (u32 vg = 0; vg < 32; vg += 5) {
        tex_scene();
        Tex tg = solid_texture(rgb5(tr, 31 - tr, tr / 2));
        flat_quad(&tg, SOLID, vg, (vg * 3) & 31, 31 - vg);
        show(0);
        u32 tc[3] = {tr, 31 - tr, tr / 2}, vc[3] = {vg, (vg * 3) & 31, 31 - vg}, want = 0;
        for (int k = 0; k < 3; k++) want |= (modulate6(e6(tc[k]), e6(vc[k])) >> 1) << (5 * k);
        if (pixel(30, 30) != (OPAQUE | want)) bad++;
    }
    CHECK(!bad, "modulation grid: %d mismatches", bad);

    /* the alpha: A = ((At + 1) * (Av + 1) - 1) / 64 with both expanded to 6 bits. A texel of alpha 31 under a
     * polygon of 16 gives 33, 16 of 31; a texel of alpha 16 under a solid polygon gives 33 too. Over the rear
     * plane (3, 5, 7) the blend is (c6 * (A + 1) + rear6 * (31 - A)) >> 5 */
    tex_scene();
    for (int k = 0; k < 8; k++) pal(2 * (u32)k, 0x7FFF);
    for (int i = 0; i < 64; i++) put8(0x300 + (u32)i, 16u << 3);          /* A5I3, alpha 16 */
    Tex ta = {.fmt = 6, .base = 0x300};
    tex_quad(&ta, SOLID, 0, 0, 64, 64, 0, 0, 8, 8);
    show(0);
    u32 got = pixel(30, 30), w6[3] = {7, 11, 15};
    u32 want = 0;
    for (int k = 0; k < 3; k++) want |= (((63 * 17 + w6[k] * 15) >> 5) >> 1) << (5 * k);
    CHECK(got == (OPAQUE | want), "texel alpha 16, polygon solid: %04x, wanted %04x", got, want);
    tex_scene();
    for (int k = 0; k < 8; k++) pal(2 * (u32)k, 0x7FFF);
    for (int i = 0; i < 64; i++) put8(0x300 + (u32)i, 31u << 3);
    tex_quad(&ta, attr(16, 1, ATTR_FRONT | ATTR_BACK), 0, 0, 64, 64, 0, 0, 8, 8);
    show(0);
    got = pixel(30, 30);
    CHECK(got == (OPAQUE | want), "polygon alpha 16, texel solid: %04x, wanted %04x", got, want);
}

static void test_decal(void) {
    /* R = (Rt * At + Rv * (63 - At)) / 64 with At as 6 bits, except At = 0 gives the vertex colour and
     * At = 31 (63) the texel's: the texture's alpha is only the mix, the polygon's stays Av */
    for (u32 a5 = 0; a5 < 32; a5 += 3) {
        tex_scene();
        for (int k = 0; k < 8; k++) pal(2 * (u32)k, rgb5(31, 8, 0));
        for (int i = 0; i < 64; i++) put8(0x300 + (u32)i, a5 << 3);
        Tex t = {.fmt = 6, .base = 0x300};
        flat_quad(&t, SOLID | 1u << 4, 10, 20, 5);
        show(0);
        u32 tc[3] = {31, 8, 0}, vc[3] = {10, 20, 5}, want = 0, at = e6(a5);
        for (int k = 0; k < 3; k++) {
            u32 r = a5 == 0 ? e6(vc[k]) : a5 == 31 ? e6(tc[k]) : (e6(tc[k]) * at + e6(vc[k]) * (63 - at)) / 64;
            want |= (r >> 1) << (5 * k);
        }
        u32 a;
        u32 got = px(30, 30, &a);
        CHECK(got == (OPAQUE | want) && a == 31, "decal with texel alpha %u: %04x alpha %u, wanted %04x", a5, got, a, want);
    }
    /* A = Av: a translucent decal polygon keeps its own alpha whatever the texel's */
    tex_scene();
    for (int k = 0; k < 8; k++) pal(2 * (u32)k, 0x7FFF);
    for (int i = 0; i < 64; i++) put8(0x300 + (u32)i, 31u << 3);
    Tex t = {.fmt = 6, .base = 0x300};
    tex_quad(&t, attr(8, 1, ATTR_FRONT | ATTR_BACK) | 1u << 4, 0, 0, 64, 64, 0, 0, 8, 8);
    show(0);
    u32 w6[3] = {7, 11, 15}, want = 0;
    for (int k = 0; k < 3; k++) want |= (((63 * 9 + w6[k] * 23) >> 5) >> 1) << (5 * k);
    CHECK(pixel(30, 30) == (OPAQUE | want), "decal keeps the polygon's alpha: %04x, wanted %04x", pixel(30, 30), want);
}

static void toon_table(u32 (*f)(u32)) { for (u32 i = 0; i < 32; i += 2) reg(TOON_TABLE + 2 * i, f(i) | f(i + 1) << 16); }
static u32 toon_color(u32 k) { return rgb5(k, 31 - k, (k * 3) & 31); }

static void test_toon_highlight(void) {
    /* toon (DISP3DCNT.1 clear): the red of the vertex colour indexes the table; untextured, the polygon
     * is the table's colour. The vertex red of 5 bits is 6 bits inside, the table is 32 entries */
    int bad = 0;
    for (u32 r = 0; r < 32; r++) {
        scene(1 | 8);
        toon_table(toon_color);
        cmd(POLYGON_ATTR, SOLID | 2u << 4);
        color(r, 17, 3);
        quad(-0.5, -0.5, 0.5, 0.5, 0);
        show(0);
        if (pixel(128, 96) != (OPAQUE | toon_color(r))) { bad++; if (bad < 4) printf("  toon %u: %04x, wanted %04x\n", r, pixel(128, 96), toon_color(r)); }
    }
    CHECK(!bad, "toon table index: %d of 32 wrong", bad);
    /* the green and blue of the vertex colour are not used */
    scene(1 | 8);
    toon_table(toon_color);
    cmd(POLYGON_ATTR, SOLID | 2u << 4);
    cmd(BEGIN_VTXS, 1);
    color(9, 0, 0); vtx(-0.5, -0.5, 0); color(9, 31, 31); vtx(0.5, -0.5, 0); color(9, 5, 20); vtx(0.5, 0.5, 0); color(9, 31, 0); vtx(-0.5, 0.5, 0);
    cmd0(END_VTXS);
    show(0);
    CHECK(pixel(100, 120) == (OPAQUE | toon_color(9)) && pixel(150, 60) == (OPAQUE | toon_color(9)), "toon ignores green and blue: %04x", pixel(100, 120));
    /* the index follows the interpolated red: a ramp from 0 to 31 across the quad */
    scene(1 | 8);
    toon_table(toon_color);
    cmd(POLYGON_ATTR, SOLID | 2u << 4);
    cmd(BEGIN_VTXS, 1);
    color(0, 0, 0); vtx(-0.5, -0.5, 0); color(31, 0, 0); vtx(0.5, -0.5, 0); color(31, 0, 0); vtx(0.5, 0.5, 0); color(0, 0, 0); vtx(-0.5, 0.5, 0);
    cmd0(END_VTXS);
    show(0);
    u32 left = pixel(70, 96), right = pixel(185, 96);
    CHECK((left & 31) < 6 && (right & 31) > 26 && (pixel(128, 96) & 31) > 12 && (pixel(128, 96) & 31) < 19, "toon ramp: %u %u %u", left & 31, pixel(128, 96) & 31, right & 31);

    /* textured toon is modulation by the table colour; highlight adds the table colour, to 63 at most */
    const u32 texel[3] = {rgb5(16, 16, 16), rgb5(31, 8, 0), rgb5(2, 30, 9)};
    for (int hl = 0; hl < 2; hl++) for (int ti = 0; ti < 3; ti++) {
        scene(1 | 8 | (hl ? 2u : 0));
        reg(CLEAR_COLOR, 31u << 16);
        texture_vram();
        toon_table(toon_color);
        Tex t = solid_texture(texel[ti]);
        flat_quad(&t, SOLID | 2u << 4, 20, 0, 0);
        show(0);
        u32 want = 0, sc = toon_color(20);
        for (int k = 0; k < 3; k++) {
            u32 m = modulate6(e6(c5(texel[ti], k)), e6(c5(sc, k)));
            if (hl) { m += e6(c5(sc, k)); if (m > 63) m = 63; }
            want |= (m >> 1) << (5 * k);
        }
        CHECK(pixel(30, 30) == (OPAQUE | want), "%s with texel %d: %04x, wanted %04x", hl ? "highlight" : "toon", ti, pixel(30, 30), want);
    }
}

/* ---- shadow polygons ------------------------------------------------------------------------ */
static void quad_at(double x0, double y0, double x1, double y1, double z, int cw) {
    cmd(BEGIN_VTXS, 1);
    if (cw) { vtx(x0, y0, z); vtx(x0, y1, z); vtx(x1, y1, z); vtx(x1, y0, z); }
    else { vtx(x0, y0, z); vtx(x1, y0, z); vtx(x1, y1, z); vtx(x0, y1, z); }
    cmd0(END_VTXS);
}
/* a shadow volume over [x0, x1] x [y0, y1]: its rear face (clockwise) at z_back, its front face at z_front */
static void volume(double x0, double y0, double x1, double y1, double z_front, double z_back) {
    quad_at(x0, y0, x1, y1, z_back, 1);
    quad_at(x0, y0, x1, y1, z_front, 0);
}

static void shadow_scene(u32 ground_id, int mask, int render) {
    scene(1 | 8);
    cmd(SWAP_BUFFERS, 1); gpu3d_line(s, 191);               /* sent order for the list: GBATEK wants the mask first */
    cmd(POLYGON_ATTR, attr(31, ground_id, ATTR_FRONT)); color(0, 31, 0);
    quad(-0.9, -0.9, 0.9, 0.9, 0);
    if (mask) {                                             /* step 1: rear faces only, ID 0, no colour */
        cmd(POLYGON_ATTR, attr(16, 0, ATTR_BACK | 3u << 4)); color(0, 0, 0);
        volume(-0.5, -0.5, 0.0, 0.5, -0.5, 0.5);            /* the ground is inside this one */
        volume(0.2, -0.5, 0.7, 0.5, -0.8, -0.6);            /* this one is wholly in front of the ground */
    }
    if (render) {                                           /* step 2: front faces, a new ID */
        cmd(POLYGON_ATTR, attr(16, 2, ATTR_FRONT | 3u << 4)); color(0, 0, 0);
        volume(-0.5, -0.5, 0.0, 0.5, -0.5, 0.5);
        volume(0.2, -0.5, 0.7, 0.5, -0.8, -0.6);
    }
    show(0);
}

static void test_shadow(void) {
    u32 green = OPAQUE | rgb5(0, 31, 0), shaded = OPAQUE | rgb5(0, 14, 0);   /* black at alpha 16 over green: (63 * 15) >> 5 = 29 */
    /* mask and render: shaded where the volume holds the ground, none where it lies before it, none outside */
    shadow_scene(1, 1, 1);
    CHECK(pixel(100, 96) == shaded, "inside the volume: %04x, wanted %04x", pixel(100, 96), shaded);
    CHECK(pixel(180, 96) == green, "a volume in front of the ground casts nothing: %04x", pixel(180, 96));
    CHECK(pixel(40, 96) == green && pixel(128, 20) == green, "outside the volumes");
    /* the mask alone draws nothing, not even into the depth buffer: the ground is intact */
    shadow_scene(1, 1, 0);
    CHECK(pixel(100, 96) == green && pixel(180, 96) == green, "the mask polygons draw: %04x %04x", pixel(100, 96), pixel(180, 96));
    /* the render volume without a mask shades wherever its front face is before the ground, both places */
    shadow_scene(1, 0, 1);
    CHECK(pixel(100, 96) == shaded && pixel(180, 96) == shaded, "without a mask: %04x %04x", pixel(100, 96), pixel(180, 96));
    /* a render volume checks the stencil and clears it: a second one over the region the first found
     * masked is no longer held back (ID 3, so the first one's translucent ID does not stop it) */
    scene(1 | 8);
    cmd(SWAP_BUFFERS, 1); gpu3d_line(s, 191);
    cmd(POLYGON_ATTR, attr(31, 1, ATTR_FRONT)); color(0, 31, 0);
    quad(-0.9, -0.9, 0.9, 0.9, 0);
    cmd(POLYGON_ATTR, attr(16, 0, ATTR_BACK | 3u << 4)); color(0, 0, 0);
    volume(0.2, -0.5, 0.7, 0.5, -0.8, -0.6);
    cmd(POLYGON_ATTR, attr(16, 2, ATTR_FRONT | 3u << 4));
    volume(0.2, -0.5, 0.7, 0.5, -0.8, -0.6);
    cmd(POLYGON_ATTR, attr(16, 3, ATTR_FRONT | 3u << 4));
    volume(0.2, -0.5, 0.7, 0.5, -0.8, -0.6);
    show(0);
    CHECK(pixel(180, 96) == shaded, "the stencil is cleared once a render volume has met it: %04x", pixel(180, 96));
    /* the render volume's ID is that of the ground: no shadow on the object that casts it */
    shadow_scene(2, 1, 1);
    CHECK(pixel(100, 96) == green, "same polygon ID: %04x", pixel(100, 96));
}

/* ---- translucent textures and the order polygons are drawn in ------------------------------- */
/* A quad over the pixels [x0, x1) x [y0, y1) of tex_quad's 256 x 128 viewport at depth z (NDC: the
 * smaller, the nearer), textured with t (NULL: untextured) mapped whole, vertex colour c, drawn with
 * POLYGON_ATTR a. */
static void zquad(const Tex *t, u32 a, u32 c, int x0, int y0, int x1, int y1, double z) {
    cmd(VIEWPORT, 64u << 8 | 255u << 16 | 191u << 24);
    cmd(POLYGON_ATTR, a);
    cmd(TEXIMAGE_PARAM, t ? teximage(t) : 0);
    cmd(PLTT_BASE, 0);
    cmd(COLOR, c);
    cmd(BEGIN_VTXS, 1);
    int px4[4] = {x0, x1, x1, x0}, py4[4] = {y1, y1, y0, y0}, ss[4] = {0, 8, 8, 0}, tt[4] = {8, 8, 0, 0};
    for (int i = 0; i < 4; i++) {
        cmd(TEXCOORD, (u32)(ss[i] * 16) | (u32)(tt[i] * 16) << 16);
        cmd(VTX_16, (u32)(u16)(s16)(32 * px4[i] - 4096) | (u32)(u16)(s16)(4096 - 64 * py4[i]) << 16);
        cmd(VTX_16, (u32)(u16)(s16)F(z));
    }
    cmd0(END_VTXS);
}

/* A texture of one A5I3 or A3I5 texel, white, of the alpha (of 31) the format can hold nearest to 16 */
static Tex alpha_texture(int fmt, u32 *at) {
    for (int k = 0; k < 32; k++) pal(2 * (u32)k, 0x7FFF);
    u32 texel = fmt == 6 ? 16u << 3 : 4u << 5;                 /* A5I3 alpha 16; A3I5 alpha 4 of 7 is 4 * 4 + 4 / 2 = 18 */
    *at = fmt == 6 ? 16 : 18;
    for (int i = 0; i < 64; i++) put8(0x300 + (u32)i, texel);
    Tex t = {.fmt = fmt, .base = 0x300};
    return t;
}

/* GBATEK's alpha blending on 6-bit colour: (new * (A + 1) + old * (31 - A)) / 32, a channel at a time, as 5 bits */
static u32 blend5(u32 new6[3], u32 old6[3], u32 a) {
    u32 o = 0;
    for (int k = 0; k < 3; k++) o |= (((new6[k] * (a + 1) + old6[k] * (31 - a)) >> 5) >> 1) << (5 * k);
    return o;
}

static void test_translucent_texture_order(void) {
    /* A white quad of polygon alpha 31 with a translucent texture, nearer, and a green opaque one behind it:
     * the texels of alpha < 31 are translucent pixels, which do not write depth, so the polygon has to be drawn
     * after the opaque ones or what is behind it wipes it out. Either send order, auto or manual sort. */
    static const char *names[2] = {"A5I3", "A3I5"};
    for (int f = 0; f < 2; f++) for (int order = 0; order < 4; order++) {
        int fmt = f ? 1 : 6, green_first = order & 1, manual = order >> 1;
        tex_scene();
        u32 at;
        Tex t = alpha_texture(fmt, &at);
        if (manual) { cmd(SWAP_BUFFERS, 1); gpu3d_line(s, 191); }
        if (green_first) zquad(NULL, SOLID, rgb5(0, 31, 0), 0, 0, 64, 64, 0.5);
        zquad(&t, SOLID, rgb5(31, 31, 31), 0, 0, 64, 64, -0.5);
        if (!green_first) zquad(NULL, SOLID, rgb5(0, 31, 0), 0, 0, 64, 64, 0.5);
        show(0);
        u32 white[3] = {modulate6(63, 63), modulate6(63, 63), modulate6(63, 63)}, green[3] = {0, 63, 0};
        u32 a = modulate6(e6(at), e6(31)) >> 1, want = OPAQUE | blend5(white, green, a);
        CHECK(pixel(30, 30) == want, "%s texel alpha %u, %s first, %s sort: %04x, wanted %04x", names[f], at,
              green_first ? "green" : "textured", manual ? "manual" : "auto", pixel(30, 30), want);
    }

    /* a wire frame is made transparent the same way (GBATEK): the outline blends over the green behind, the
     * inside shows the green */
    tex_scene();
    u32 at;
    Tex tw = alpha_texture(6, &at);
    zquad(&tw, attr(0, 0, ATTR_FRONT | ATTR_BACK), rgb5(31, 31, 31), 8, 0, 72, 64, -0.5);
    zquad(NULL, SOLID, rgb5(0, 31, 0), 0, 0, 100, 64, 0.5);
    show(0);
    u32 white[3] = {63, 63, 63}, green[3] = {0, 63, 0};
    u32 want = OPAQUE | blend5(white, green, modulate6(e6(at), e6(31)) >> 1);
    CHECK(pixel(8, 30) == want, "wire frame, translucent texture: edge %04x, wanted %04x", pixel(8, 30), want);
    CHECK(pixel(40, 30) == (OPAQUE | rgb5(0, 31, 0)), "wire frame: inside %04x", pixel(40, 30));

    /* a decal polygon's alpha is its own (A = Av): a texel's alpha only mixes the colours, so a decal of alpha 31
     * never makes a translucent pixel and stays among the opaque polygons. A nearer translucent quad that the
     * Y-sort would put first (it ends higher up) has to blend over it, not be wiped out by it */
    tex_scene();
    for (int k = 0; k < 8; k++) pal(2 * (u32)k, rgb5(31, 0, 0));
    for (int i = 0; i < 64; i++) put8(0x300 + (u32)i, 16u << 3);
    Tex td = {.fmt = 6, .base = 0x300};
    zquad(&td, SOLID | 1u << 4, rgb5(0, 31, 0), 0, 0, 64, 64, 0.5);
    zquad(NULL, attr(16, 1, ATTR_FRONT | ATTR_BACK), rgb5(0, 0, 31), 0, 0, 64, 32, -0.5);
    show(0);
    u32 tex6[3] = {63, 0, 0}, vtx6[3] = {0, 63, 0}, decal[3], blue[3] = {0, 0, 63}, at6 = e6(16);
    for (int k = 0; k < 3; k++) decal[k] = (tex6[k] * at6 + vtx6[k] * (63 - at6)) / 64;
    want = OPAQUE | blend5(blue, decal, 16);
    CHECK(pixel(30, 10) == want, "translucent quad over a decal of alpha 31: %04x, wanted %04x", pixel(30, 10), want);

    /* with textures off in DISP3DCNT the A5I3 polygon is an untextured one of alpha 31, an opaque one in the order
     * sent: at equal depth the first drawn stays */
    scene(8);
    reg(CLEAR_COLOR, REAR | 31u << 16);
    Tex tn = {.fmt = 6, .base = 0x300};
    zquad(&tn, SOLID, rgb5(31, 31, 31), 0, 0, 64, 64, 0);
    zquad(NULL, SOLID, rgb5(0, 31, 0), 0, 0, 64, 64, 0);
    show(0);
    CHECK(pixel(30, 30) == (OPAQUE | 0x7FFF), "textures off, equal depth: %04x, the first sent should stay", pixel(30, 30));

    /* a shadow mask draws no colour, so a textured one of alpha 31 stays among the opaque polygons, before the
     * render volume that the sort would otherwise put ahead of it (that one ends higher up). The volume lies wholly
     * in front of the ground, so the mask marks it and the render volume must find the mark: no shadow */
    scene(1 | 8);
    cmd(POLYGON_ATTR, attr(31, 1, ATTR_FRONT)); color(0, 31, 0);
    quad(-0.9, -0.9, 0.9, 0.9, 0);
    cmd(POLYGON_ATTR, attr(31, 0, ATTR_BACK | 3u << 4)); cmd(TEXIMAGE_PARAM, teximage(&tn)); color(0, 0, 0);
    volume(0.2, -0.5, 0.7, 0.5, -0.8, -0.6);
    cmd(POLYGON_ATTR, attr(16, 2, ATTR_FRONT | 3u << 4)); cmd(TEXIMAGE_PARAM, 0);
    volume(0.2, -0.25, 0.7, 0.25, -0.8, -0.6);
    show(0);
    CHECK(pixel(180, 96) == (OPAQUE | rgb5(0, 31, 0)), "textured mask of alpha 31: %04x, wanted the ground", pixel(180, 96));
}

/* ---- lighting ------------------------------------------------------------------------------- */
static u32 vec3(int x, int y, int z) { return (u32)(x & 0x3FF) | (u32)(y & 0x3FF) << 10 | (u32)(z & 0x3FF) << 20; }   /* 1.9 numbers */
static void light_vector(u32 i, int x, int y, int z) { cmd(LIGHT_VECTOR, vec3(x, y, z) | i << 30); }
static void light_color(u32 i, u32 c) { cmd(LIGHT_COLOR, c | i << 30); }
/* NORMAL inside a polygon with the lights in `lights` enabled; the vertex colour it makes, as three 5-bit numbers */
static void lit(u32 lights, int x, int y, int z, int out[3]) {
    cmd(POLYGON_ATTR, SOLID | lights);
    cmd(BEGIN_VTXS, 0);
    cmd(NORMAL, vec3(x, y, z));
    for (int k = 0; k < 3; k++) {
        u32 v6 = k == 0 ? s->g3d.r : k == 1 ? s->g3d.g : s->g3d.b;
        out[k] = (int)(v6 >> 1);
        if (v6) CHECK(v6 & 1, "a colour component of 6 bits is %u: not X * 2 + 1", v6);
    }
}
static void lighting_scene(void) {
    scene(0);
    cmd(MTX_MODE, 2); cmd0(MTX_IDENTITY);                  /* the vector matrix, which the light and normal vectors pass through */
    for (u32 i = 0; i < 4; i++) { light_color(i, 0); light_vector(i, 0, 0, 0); }
}
static void close_to(const int *got, int r, int g, int b, const char *what) {
    CHECK(abs(got[0] - r) <= 1 && abs(got[1] - g) <= 1 && abs(got[2] - b) <= 1, "%s: %d %d %d, wanted %d %d %d (+-1)", what, got[0], got[1], got[2], r, g, b);
}
static void exactly(const int *got, int r, int g, int b, const char *what) {
    CHECK(got[0] == r && got[1] == g && got[2] == b, "%s: %d %d %d, wanted %d %d %d", what, got[0], got[1], got[2], r, g, b);
}

static void test_lighting(void) {
    int c[3];
    /* with no light enabled the colour is the emission */
    lighting_scene();
    cmd(SPE_EMI, 5u << 16 | 10u << 21 | 15u << 26);
    cmd(DIF_AMB, 31 | 31u << 5 | 31u << 10 | 31u << 16 | 31u << 21 | 31u << 26);
    light_color(0, 0x7FFF);
    lit(0, 0, 0, 511, c);
    exactly(c, 5, 10, 15, "emission alone");

    /* a light shining straight at a face: 511 / 512 either way, level (511 / 512)^2 = 0.9961.
     * Diffuse 31 * light 31 * level / 31 = 30.88, 16 * 0.9961 = 15.9; ambient adds amb * light / 31 */
    lighting_scene();
    cmd(SPE_EMI, 0);
    cmd(DIF_AMB, 31u | 16u << 5 | 0u << 10 | 0u << 16 | 4u << 21 | 8u << 26);
    light_color(0, 0x7FFF);
    light_vector(0, 0, 0, -511);
    lit(1, 0, 0, 511, c);
    close_to(c, 31, 16 + 4, 8, "diffuse and ambient");
    CHECK(c[0] >= 30 && c[1] >= 19, "the level is just under 1: %d %d", c[0], c[1]);

    /* ambient needs no angle at all, and a light behind the face gives no diffuse: ambient alone, exactly */
    lighting_scene();
    cmd(SPE_EMI, 0);
    cmd(DIF_AMB, 31u | 31u << 5 | 31u << 10 | 3u << 16 | 4u << 21 | 5u << 26);
    light_color(0, 0x7FFF);
    light_vector(0, 0, 0, 511);
    lit(1, 0, 0, 511, c);
    exactly(c, 3, 4, 5, "a light behind the face");
    /* a light colour scales ambient: amb * light / 31 */
    lighting_scene();
    cmd(DIF_AMB, 0 | 31u << 16 | 31u << 21 | 31u << 26);
    light_color(0, 31u | 15u << 5 | 0u << 10);
    lit(1, 0, 0, 511, c);
    exactly(c, 31, 15, 0, "ambient through a coloured light");

    /* an angle: a light 60 degrees off the normal gives half the diffuse (cos 60 = 0.5) */
    lighting_scene();
    cmd(DIF_AMB, 31u | 31u << 5 | 31u << 10);
    light_color(0, 0x7FFF);
    light_vector(0, 0, -442, -256);          /* (0, -0.863, -0.5): length 1 */
    lit(1, 0, 0, 511, c);
    close_to(c, 15, 15, 15, "60 degrees");

    /* specular: ((-H) . N)^2 with H = (L + (0, 0, -1)) / 2, so a light at the camera and a face toward it is (0.9990 * 0.9980)^2 = 0.994 */
    lighting_scene();
    cmd(SPE_EMI, 31u | 8u << 5 | 0u << 10);
    cmd(DIF_AMB, 0);
    light_color(0, 0x7FFF);
    light_vector(0, 0, 0, -511);
    lit(1, 0, 0, 511, c);
    close_to(c, 31, 8, 0, "specular");
    CHECK(c[0] >= 30, "specular level: %d", c[0]);
    /* ... and none when the face is turned a long way from the half vector (normal at 90 degrees) */
    lit(1, 511, 0, 0, c);
    exactly(c, 0, 0, 0, "specular at a right angle");
    /* the shininess table (SPE_EMI.15): every entry 64 of 256 is a quarter of the level */
    lighting_scene();
    cmd(SPE_EMI, 31u | 31u << 5 | 31u << 10 | 0x8000u);
    cmd(DIF_AMB, 0);
    light_color(0, 0x7FFF);
    light_vector(0, 0, 0, -511);
    for (int i = 0; i < 32; i++) cmd(SHININESS, 0x40404040u);
    lit(1, 0, 0, 511, c);
    close_to(c, 8, 8, 8, "shininess table of one quarter");
    /* a table that rises with the level: entry i is 2 * i, which is the level over 128 steps: nearly what no table gives */
    for (int i = 0; i < 32; i++) cmd(SHININESS, (u32)(8 * i) | (u32)(8 * i + 2) << 8 | (u32)(8 * i + 4) << 16 | (u32)(8 * i + 6) << 24);
    lit(1, 0, 0, 511, c);
    close_to(c, 31, 31, 31, "a rising shininess table");
    /* a normal at 45 degrees to the half vector: level cos^2 = 0.5, then a table entry of 128 * 0.5.. use the linear
     * level: 0.5 of 31 */
    lighting_scene();
    cmd(SPE_EMI, 31u | 31u << 5 | 31u << 10);
    cmd(DIF_AMB, 0);
    light_color(0, 0x7FFF);
    light_vector(0, 0, 0, -511);
    lit(1, 362, 0, 362, c);                 /* (0.707, 0, 0.707): -H . N is 0.707 * 0.999 */
    close_to(c, 15, 15, 15, "specular at 45 degrees");

    /* four lights add and the sum stops at 31; a light the polygon does not enable adds nothing */
    lighting_scene();
    cmd(SPE_EMI, 0);
    cmd(DIF_AMB, 31u | 31u << 5 | 31u << 10);
    for (u32 i = 0; i < 4; i++) { light_color(i, 0x7FFF); light_vector(i, 0, 0, -511); }
    lit(1 | 2, 0, 0, 511, c);
    close_to(c, 31, 31, 31, "two lights clamp");
    lighting_scene();
    cmd(DIF_AMB, 31u | 31u << 5 | 31u << 10);
    light_color(0, 4u | 4u << 5 | 4u << 10); light_vector(0, 0, 0, -511);
    light_color(1, 8u | 8u << 5 | 8u << 10); light_vector(1, 0, 0, -511);
    light_color(2, 16u | 16u << 5 | 16u << 10); light_vector(2, 0, 0, -511);
    lit(1 | 2, 0, 0, 511, c);
    close_to(c, 12, 12, 12, "two of three lights add");
    lit(4, 0, 0, 511, c);
    close_to(c, 16, 16, 16, "only the enabled light");
    lit(0, 0, 0, 511, c);
    exactly(c, 0, 0, 0, "no light enabled");

    /* the vector matrix turns the normal (and the light vector when it is set): a quarter turn takes
     * (1, 0, 0) to (0, 0, 1) */
    lighting_scene();
    cmd(MTX_MODE, 2);
    s32 m[16] = {0};
    m[2] = F(1); m[5] = F(1); m[8] = F(-1); m[15] = F(1);
    for (int i = 0; i < 16; i++) cmd(MTX_LOAD_4x4, (u32)m[i]);
    cmd(DIF_AMB, 31u | 31u << 5 | 31u << 10);
    light_color(0, 0x7FFF);
    cmd(MTX_MODE, 2); cmd0(MTX_IDENTITY);
    light_vector(0, 0, 0, -511);             /* set with the identity: unrotated */
    cmd(MTX_MODE, 2);
    for (int i = 0; i < 16; i++) cmd(MTX_LOAD_4x4, (u32)m[i]);
    lit(1, 511, 0, 0, c);
    close_to(c, 31, 31, 31, "a normal turned toward the light by the matrix");
    lit(1, 0, 0, 511, c);                    /* turned to (-1, 0, 0): at a right angle to the light */
    exactly(c, 0, 0, 0, "a normal turned away");

    /* DIF_AMB.15 takes the diffuse colour as the vertex colour, like COLOR */
    lighting_scene();
    cmd(DIF_AMB, rgb5(31, 4, 10) | 0x8000u);
    cmd(POLYGON_ATTR, SOLID);
    CHECK(s->g3d.r == 63 && s->g3d.g == 9 && s->g3d.b == 21, "DIF_AMB sets the vertex colour: %u %u %u", s->g3d.r, s->g3d.g, s->g3d.b);
    cmd(DIF_AMB, rgb5(1, 2, 3));
    CHECK(s->g3d.r == 63, "without bit 15 the colour stays");

    /* the colour NORMAL made is what the next vertices carry, and what is drawn */
    lighting_scene();
    cmd(DIF_AMB, 31u << 5);                  /* diffuse green only */
    light_color(0, 0x7FFF);
    light_vector(0, 0, 0, -511);
    cmd(POLYGON_ATTR, SOLID | 1);
    cmd(BEGIN_VTXS, 1);
    cmd(NORMAL, vec3(0, 0, 511));
    vtx(-0.5, -0.5, 0); vtx(0.5, -0.5, 0); vtx(0.5, 0.5, 0); vtx(-0.5, 0.5, 0);
    cmd0(END_VTXS);
    show(0);
    u32 got = pixel(128, 96);
    CHECK((got & 31) == 0 && ((got >> 5) & 31) >= 30 && ((got >> 10) & 31) == 0, "lit green quad: %04x", got);
}

/* ---- fog ------------------------------------------------------------------------------------ */
/* the 15-bit depth a quad at clip z (w = 1) has in the buffer: (z * 0x4000 + 0x3FFF) * 0x200 is 24 bits, 15 above the low 9 */
static u32 depth15(double z) { return (u32)(F(z) * 4 + 0x3FFF); }
/* GBATEK: FogDepthBoundary[n] = FOG_OFFSET + FOG_STEP * (n + 1), the density running linearly from table[n] at boundary n to
 * table[n + 1] at the next, table[0] before the first boundary and table[31] beyond the last; 127 is 128 */
static u32 ref_density(u32 d, u32 offset, u32 shift, const u8 *tbl) {
    u32 step = 0x400 >> shift, dens;
    if (d < offset + step) dens = tbl[0];
    else if (!step || d >= offset + step * 32) dens = tbl[31];
    else {
        u32 n = 0;
        while (d >= offset + step * (n + 2)) n++;
        u32 f = d - (offset + step * (n + 1));
        dens = (tbl[n] * (step - f) + tbl[n + 1] * f) / step;
    }
    return dens >= 127 ? 128 : dens;
}
static void fog_table(const u8 *tbl) { for (int i = 0; i < 32; i += 4) reg(FOG_TABLE + (u32)i, tbl[i] | tbl[i + 1] << 8 | tbl[i + 2] << 16 | (u32)tbl[i + 3] << 24); }

/* a black quad over the whole screen at depth z, fog colour white with alpha fa; returns the pixel and its alpha */
static u32 fog_pixel(u32 disp, u32 poly_flags, double z, u32 offset, const u8 *tbl, u32 fa, u32 *alpha) {
    scene(1 | 8 | disp);
    reg(FOG_COLOR, 0x7FFFu | fa << 16);
    reg(FOG_OFFSET, offset);
    fog_table(tbl);
    cmd(POLYGON_ATTR, attr(31, 1, ATTR_FRONT | poly_flags)); color(0, 0, 0);
    quad(-0.9, -0.9, 0.9, 0.9, z);
    show(0);
    return px(128, 96, alpha);
}

static void test_fog(void) {
    u8 tbl[32], full[32];
    for (int i = 0; i < 32; i++) { tbl[i] = (u8)(4 * i); full[i] = (u8)(i == 31 ? 127 : 4 * i); }
    u32 a, c, d;
    /* off in DISP3DCNT, or off on the polygon: untouched */
    c = fog_pixel(0, ATTR_FOG, 0.5, 0x4000, tbl, 0, &a);
    CHECK(c == OPAQUE && a == 31, "fog disabled: %04x alpha %u", c, a);
    c = fog_pixel(0x80, 0, 0.5, 0x4000, tbl, 0, &a);
    CHECK(c == OPAQUE && a == 31, "fog off on the polygon: %04x alpha %u", c, a);
    /* colour and alpha: FrameBuffer = (Fog * density + FrameBuffer * (128 - density)) / 128 on the 6-bit colour, the alpha too */
    struct { double z; u32 offset, shift; const u8 *t; } cases[] = {
        {0.5, 0x4000, 0, tbl}, {0.9, 0x4000, 0, tbl}, {-0.5, 0x4000, 0, tbl}, {0.0, 0, 0, tbl}, {0.5, 0x2000, 1, tbl},
        {0.9, 0x4000, 4, tbl},              /* beyond the last boundary: table[31] */
        {0.9, 0x4000, 4, full},             /* 127 counts as 128: all fog */
        {0.5, 0x4000, 11, tbl},             /* a step of zero: table[0] before the offset, table[31] after */
        {-0.5, 0x4000, 11, tbl},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        u32 fa = 5;
        c = fog_pixel(0x80 | cases[i].shift << 8, ATTR_FOG, cases[i].z, cases[i].offset, cases[i].t, fa, &a);
        u32 d = ref_density(depth15(cases[i].z), cases[i].offset, cases[i].shift, cases[i].t);
        u32 r6 = (63 * d) / 128;
        /* the alpha: GBATEK's glitch reads the fog alpha as 31 up to the first boundary */
        u32 fa_used = depth15(cases[i].z) < cases[i].offset + (0x400u >> cases[i].shift) ? 31 : fa;
        u32 want_a = (fa_used * d + 31 * (128 - d)) / 128;
        CHECK(c == (OPAQUE | rgb5(r6 >> 1, r6 >> 1, r6 >> 1)) && a == want_a, "fog case %u (density %u): %04x alpha %u, wanted colour %u alpha %u", i, d, c, a, r6 >> 1, want_a);
    }
    /* the glitch is only seen when table[0] is not zero: a quad before the first boundary, the fog alpha 5 read as 31 */
    u8 start[32];
    for (int i = 0; i < 32; i++) start[i] = (u8)(40 + 2 * i);
    c = fog_pixel(0x80, ATTR_FOG, -0.5, 0x4000, start, 5, &a);
    CHECK(a == 31, "fog alpha before the first boundary: %u, the glitch makes it 31", a);
    c = fog_pixel(0x80, ATTR_FOG, 0.5, 0x4000, start, 5, &a);
    d = ref_density(depth15(0.5), 0x4000, 0, start);
    CHECK(a == (5 * d + 31 * (128 - d)) / 128, "fog alpha past it: %u", a);
    /* alpha only (DISP3DCNT.6): the colour is left alone */
    c = fog_pixel(0x80 | 0x40, ATTR_FOG, 0.5, 0x4000, tbl, 0, &a);
    d = ref_density(depth15(0.5), 0x4000, 0, tbl);
    CHECK(c == OPAQUE && a == (31 * (128 - d)) / 128, "fog alpha only: %04x alpha %u, wanted %u", c, a, (31 * (128 - d)) / 128);

    /* the fog flag: an opaque polygon replaces it, a translucent one ANDs it, the rear plane has its own (CLEAR_COLOR.15) */
    scene(1 | 8 | 0x80);
    reg(FOG_COLOR, 0x7FFFu); reg(FOG_OFFSET, 0x4000); fog_table(tbl);
    cmd(POLYGON_ATTR, attr(31, 1, ATTR_FRONT | ATTR_FOG)); color(0, 0, 0);
    quad(-0.9, -0.9, 0.9, 0.9, 0.5);
    cmd(POLYGON_ATTR, attr(31, 2, ATTR_FRONT)); color(0, 0, 0);
    quad(-0.5, -0.5, 0.0, 0.0, 0.4);            /* nearer, without fog: replaces the flag */
    cmd(POLYGON_ATTR, attr(16, 3, ATTR_FRONT)); color(0, 0, 0);
    quad(0.2, 0.2, 0.7, 0.7, 0.3);              /* translucent, without fog, over fogged: ANDed away */
    cmd(POLYGON_ATTR, attr(16, 4, ATTR_FRONT | ATTR_FOG)); color(0, 0, 0);
    quad(-0.7, 0.3, -0.3, 0.7, 0.3);            /* translucent with fog over fogged: stays */
    show(0);
    u32 fogged = ref_density(depth15(0.5), 0x4000, 0, tbl) * 63 / 128 >> 1;
    CHECK(pixel(30, 150) == (OPAQUE | rgb5(fogged, fogged, fogged)), "fogged polygon: %04x, wanted %u", pixel(30, 150), fogged);
    CHECK(pixel(96, 120) == OPAQUE, "an opaque polygon without fog over a fogged one: %04x", pixel(96, 120));
    c = pixel(160, 60);
    CHECK((c & 31) < 2, "a translucent polygon without fog over fogged: %04x", c);     /* black at alpha 16 over fogged (not fogged again) over black */
    u32 over = pixel(50, 60);
    CHECK(over != OPAQUE, "a translucent fogged polygon keeps its fog: %04x", over);
    /* a translucent fogged polygon over an unfogged pixel does not fog it: the old flag is ANDed with the polygon's */
    scene(1 | 8 | 0x80);
    reg(FOG_COLOR, 0x7FFFu); reg(FOG_OFFSET, 0x4000); fog_table(full);
    cmd(POLYGON_ATTR, attr(31, 1, ATTR_FRONT)); color(0, 0, 0);
    quad(-0.9, -0.9, 0.9, 0.9, 0.9);
    cmd(POLYGON_ATTR, attr(16, 2, ATTR_FRONT | ATTR_FOG)); color(0, 0, 0);
    quad(-0.5, -0.5, 0.5, 0.5, 0.8);
    show(0);
    CHECK(pixel(128, 96) == OPAQUE, "a fogged translucent polygon over an unfogged one: %04x", pixel(128, 96));
    /* the rear plane: fogged by its flag and its depth (0x7FFF) */
    scene(1 | 8 | 0x80);
    reg(CLEAR_COLOR, 31u << 16 | 0x8000u);
    reg(FOG_COLOR, 0x7FFFu); reg(FOG_OFFSET, 0x4000); fog_table(tbl);
    show(0);
    d = ref_density(0x7FFF, 0x4000, 0, tbl);
    u32 r6 = 63 * d / 128;
    CHECK(pixel(10, 10) == (OPAQUE | rgb5(r6 >> 1, r6 >> 1, r6 >> 1)), "fogged rear plane: %04x, wanted %u", pixel(10, 10), r6 >> 1);
    scene(1 | 8 | 0x80);
    reg(FOG_COLOR, 0x7FFFu); reg(FOG_OFFSET, 0x4000); fog_table(tbl);
    show(0);
    CHECK(pixel(10, 10) == OPAQUE, "rear plane without its fog flag: %04x", pixel(10, 10));
}

/* ---- edge marking --------------------------------------------------------------------------- */
static void edge_colors(void) { for (u32 k = 0; k < 8; k += 2) reg(EDGE_COLOR + 2 * k, edge_color(k) | edge_color(k + 1) << 16); }

static void test_edge_marking(void) {
    /* a polygon of ID 8 over the rear plane (ID 0): its outline, one pixel wide, takes edge colour 1 (IDs 8-15) */
    scene(1 | 8 | 0x20);
    edge_colors();
    cmd(POLYGON_ATTR, attr(31, 8, ATTR_FRONT)); color(31, 0, 0);
    quad(-0.5, -0.5, 0.5, 0.5, 0);
    show(0);
    u32 red = OPAQUE | rgb5(31, 0, 0), edge = OPAQUE | edge_color(1);
    CHECK(pixel(64, 96) == edge && pixel(191, 96) == edge && pixel(100, 48) == edge && pixel(100, 143) == edge,
          "the outline: %04x %04x %04x %04x, wanted %04x", pixel(64, 96), pixel(191, 96), pixel(100, 48), pixel(100, 143), edge);
    CHECK(pixel(65, 96) == red && pixel(190, 96) == red && pixel(100, 49) == red && pixel(100, 142) == red && pixel(128, 96) == red,
          "the inside is not marked: %04x %04x %04x %04x", pixel(65, 96), pixel(190, 96), pixel(100, 49), pixel(100, 142));
    CHECK(pixel(63, 96) == OPAQUE && pixel(192, 96) == OPAQUE && pixel(100, 47) == OPAQUE && pixel(100, 144) == OPAQUE, "nothing outside the polygon");
    /* edge marking off: no outline */
    scene(1 | 8);
    edge_colors();
    cmd(POLYGON_ATTR, attr(31, 8, ATTR_FRONT)); color(31, 0, 0);
    quad(-0.5, -0.5, 0.5, 0.5, 0);
    show(0);
    CHECK(pixel(64, 96) == red && pixel(100, 48) == red, "edge marking off: %04x", pixel(64, 96));
    /* the rear plane's polygon ID matters: a polygon with the clear ID has no outline there */
    scene(1 | 8 | 0x20);
    edge_colors();
    reg(CLEAR_COLOR, 31u << 16 | 8u << 24);
    cmd(POLYGON_ATTR, attr(31, 8, ATTR_FRONT)); color(31, 0, 0);
    quad(-0.5, -0.5, 0.5, 0.5, 0);
    show(0);
    CHECK(pixel(64, 96) == red, "same ID as the rear plane: %04x", pixel(64, 96));
    /* touching the screen's edge the neighbour is the rear plane too */
    scene(1 | 8 | 0x20);
    edge_colors();
    cmd(POLYGON_ATTR, attr(31, 8, ATTR_FRONT)); color(31, 0, 0);
    quad(-1, -1, 1, 1, 0);
    show(0);
    CHECK(pixel(0, 96) == edge && pixel(255, 96) == edge && pixel(100, 0) == edge && pixel(100, 191) == edge && pixel(100, 96) == red,
          "at the screen edge: %04x %04x %04x %04x", pixel(0, 96), pixel(255, 96), pixel(100, 0), pixel(100, 191));
    /* two polygons of one ID side by side: no edge between them, the outer outline stays; of two IDs at one depth: none between
     * either (the edge's depth must be less than the neighbour's) */
    for (int ids = 1; ids <= 2; ids++) {
        scene(1 | 8 | 0x20);
        edge_colors();
        cmd(POLYGON_ATTR, attr(31, 8, ATTR_FRONT)); color(31, 0, 0);
        quad(-0.5, -0.5, 0.0, 0.5, 0);
        cmd(POLYGON_ATTR, attr(31, ids == 1 ? 8 : 16, ATTR_FRONT)); color(0, 0, 31);
        quad(0.0, -0.5, 0.5, 0.5, 0);
        show(0);
        CHECK(pixel(127, 96) == red && pixel(128, 96) == (OPAQUE | rgb5(0, 0, 31)), "%d ID(s): between them %04x %04x", ids, pixel(127, 96), pixel(128, 96));
        CHECK(pixel(64, 96) == edge, "%d ID(s): the outer outline: %04x", ids, pixel(64, 96));
    }
    /* a nearer polygon of another ID over a farther one (x 32-160, rows 72-168; x 96-224, rows 24-120): the nearer outline
     * shows over the farther polygon; the farther one's outline where it is covered does not */
    scene(1 | 8 | 0x20);
    edge_colors();
    cmd(POLYGON_ATTR, attr(31, 8, ATTR_FRONT)); color(31, 0, 0);
    quad(-0.75, -0.75, 0.25, 0.25, 0.5);
    cmd(POLYGON_ATTR, attr(31, 16, ATTR_FRONT)); color(0, 0, 31);
    quad(-0.25, -0.25, 0.75, 0.75, -0.5);
    show(0);
    u32 blue = OPAQUE | rgb5(0, 0, 31);
    CHECK(pixel(96, 100) == (OPAQUE | edge_color(2)) && pixel(120, 119) == (OPAQUE | edge_color(2)),
          "the nearer outline over a farther polygon: %04x %04x", pixel(96, 100), pixel(120, 119));
    CHECK(pixel(32, 150) == (OPAQUE | edge_color(1)) && pixel(159, 150) == (OPAQUE | edge_color(1)),
          "the farther polygon's own outline: %04x %04x", pixel(32, 150), pixel(159, 150));
    CHECK(pixel(159, 100) == blue && pixel(120, 72) == blue, "a covered outline stays hidden: %04x %04x", pixel(159, 100), pixel(120, 72));
    /* translucent polygons are not marked */
    scene(1 | 8 | 0x20);
    edge_colors();
    cmd(POLYGON_ATTR, attr(16, 8, ATTR_FRONT)); color(31, 0, 0);
    quad(-0.5, -0.5, 0.5, 0.5, 0);
    show(0);
    CHECK(pixel(64, 96) == pixel(100, 96) && pixel(100, 48) == pixel(100, 96), "a translucent polygon has no outline: %04x %04x", pixel(64, 96), pixel(100, 96));
    /* with anti-aliasing on, the edge colour is translucent: GBATEK's "alpha 16 or so" */
    scene(1 | 8 | 0x20 | 0x10);
    edge_colors();
    cmd(POLYGON_ATTR, attr(31, 8, ATTR_FRONT)); color(31, 0, 0);
    quad(-0.5, -0.5, 0.5, 0.5, 0);
    show(0);
    u32 a;
    px(100, 48, &a);
    CHECK(a == 16, "edge marking with anti-aliasing: alpha %u", a);
}

/* ---- the rear-plane bitmap ------------------------------------------------------------------ */
static void test_rear_bitmap(void) {
    tex_scene();
    reg(DISP3DCNT, 1 | 8 | 0x4000);
    reg(CLEAR_COLOR, 0);                        /* clear polygon ID 0; the bitmap decides the rest */
    /* colour bitmap in slot 2: texel (x, y) is C(x + 3y), solid where (x + y) is even; depth in slot 3: 0x7FFF but 0 for x >= 200 */
    for (u32 y = 0; y < 256; y++) for (u32 x = 0; x < 256; x++) {
        put16(0x40000 + 2 * (y * 256 + x), C(x + 3 * y) | ((x + y) % 2 ? 0 : 0x8000));
        put16(0x60000 + 2 * (y * 256 + x), (x >= 200 ? 0 : 0x7FFF) | (y == 100 ? 0x8000 : 0));
    }
    show(0);
    u32 a;
    u32 c = px(10, 10, &a);
    CHECK(c == (OPAQUE | C(10 + 30)) && a == 31, "bitmap pixel (10, 10): %04x alpha %u", c, a);
    c = px(11, 10, &a);
    CHECK(c == 0 && a == 0, "alpha bit clear is transparent: %04x alpha %u", c, a);
    /* the offset scrolls: the bitmap wraps at 256 */
    reg16(CLRIMAGE_OFFSET, 10 | 20 << 8);
    show(0);
    c = px(0, 0, &a);
    CHECK(c == (OPAQUE | C(10 + 3 * 20)) && a == 31, "scrolled (0, 0): %04x", c);
    c = px(250, 0, &a);
    CHECK(c == (OPAQUE | C(4 + 3 * 20)) && a == 31, "wrapped in x: %04x", c);
    c = px(0, 180, &a);
    CHECK(c == (OPAQUE | C(10 + 3 * 200)) && a == 31, "scrolled in y: %04x", c);
    /* the Y offset takes 8 bits */
    reg16(CLRIMAGE_OFFSET, 3 | 131 << 8);
    show(0);
    c = px(0, 0, &a);
    CHECK(c == (OPAQUE | C(3 + 3 * 131)) && a == 31, "scrolled by 131 in y: %04x", c);
    /* the depth bitmap: a polygon at the farthest depth but one shows where the bitmap is 0x7FFF and hides where it is 0 */
    reg16(CLRIMAGE_OFFSET, 0);
    cmd(POLYGON_ATTR, SOLID); color(0, 31, 0);
    cmd(VIEWPORT, 0xBFFF0000u);
    quad(-0.9, -0.9, 0.9, 0.9, 0.9);
    show(0);
    /* x 100 is under the polygon with bitmap depth 0x7FFF: drawn; x 220 has depth 0, nearer than the polygon: the bitmap shows */
    CHECK(pixel(100, 96) == (OPAQUE | rgb5(0, 31, 0)), "polygon in front of the bitmap depth: %04x", pixel(100, 96));
    c = pixel(220, 96);
    CHECK(c != (OPAQUE | rgb5(0, 31, 0)), "polygon behind the bitmap depth: %04x", c);
}

/* ---- anti-aliasing -------------------------------------------------------------------------- */
static void test_anti_aliasing(void) {
    u32 red = OPAQUE | rgb5(31, 0, 0);
    /* a triangle's slanted edge over the black rear plane: pixels along it come out between red and black; its inside stays red */
    for (int aa = 0; aa < 2; aa++) {
        scene(1 | 8 | (aa ? 0x10 : 0));
        cmd(POLYGON_ATTR, SOLID); color(31, 0, 0);
        cmd(BEGIN_VTXS, 0); vtx(-0.5, -0.5, 0); vtx(0.5, -0.5, 0); vtx(0.5, 0.5, 0); cmd0(END_VTXS);
        show(0);
        int mixed = 0;
        for (int y = 60; y < 140; y++)
            for (int x = 64; x < 192; x++) {
                u32 c = pixel(x, y), r = c & 31;
                if (r > 0 && r < 31) mixed++;
            }
        if (aa) CHECK(mixed > 40, "anti-aliasing smooths a slanted edge: %d mixed pixels", mixed);
        else CHECK(mixed == 0, "no anti-aliasing: %d mixed pixels", mixed);
        CHECK(pixel(150, 130) == red, "the inside of the triangle stays solid: %04x", pixel(150, 130));
    }
    /* the same on a right edge: the mirror triangle's slanted side is the right one */
    scene(1 | 8 | 0x10);
    cmd(POLYGON_ATTR, SOLID); color(31, 0, 0);
    cmd(BEGIN_VTXS, 0); vtx(-0.5, -0.5, 0); vtx(0.5, -0.5, 0); vtx(-0.5, 0.5, 0); cmd0(END_VTXS);
    show(0);
    int mixed = 0;
    for (int y = 60; y < 140; y++) for (int x = 64; x < 192; x++) { u32 r = pixel(x, y) & 31; if (r > 0 && r < 31) mixed++; }
    CHECK(mixed > 40, "anti-aliasing smooths a slanted right edge: %d mixed pixels", mixed);
    /* two triangles that make a quad: no seam along their shared diagonal, nowhere inside, with anti-aliasing on */
    scene(1 | 8 | 0x10);
    cmd(POLYGON_ATTR, SOLID); color(31, 0, 0);
    cmd(BEGIN_VTXS, 0);
    vtx(-0.5, -0.5, 0); vtx(0.5, -0.5, 0); vtx(0.5, 0.5, 0);
    vtx(-0.5, -0.5, 0); vtx(0.5, 0.5, 0); vtx(-0.5, 0.5, 0);
    cmd0(END_VTXS);
    show(0);
    int bad = 0;
    for (int y = 50; y < 142; y++) for (int x = 66; x < 190; x++) if (pixel(x, y) != red) bad++;
    CHECK(!bad, "a seam through a polygon pair: %d pixels not solid", bad);
    /* over a rear plane with alpha 0 the edge pixels turn translucent by their coverage, for the 2D layers behind to show */
    scene(1 | 8 | 0x10);
    reg(CLEAR_COLOR, 0);
    cmd(POLYGON_ATTR, SOLID); color(31, 0, 0);
    cmd(BEGIN_VTXS, 0); vtx(-0.5, -0.5, 0); vtx(0.5, -0.5, 0); vtx(0.5, 0.5, 0); cmd0(END_VTXS);
    show(0);
    int part = 0;
    for (int y = 60; y < 140; y++) for (int x = 64; x < 192; x++) { u32 a; px(x, y, &a); if (a > 0 && a < 31) part++; }
    CHECK(part > 40, "edge pixels translucent over a transparent rear plane: %d", part);
}

int main(void) {
    for (u32 k = 0; k < 1024; k++) if (C(k) == REAR || C(k) == 0x7FFF) { printf("palette colour %u is the rear plane's or white\n", k); return 1; }
    test_memory();
    test_formats();
    test_compressed();
    test_sizes();
    test_addressing();
    test_texcoord_modes();
    test_perspective();
    test_modulation();
    test_decal();
    test_toon_highlight();
    test_shadow();
    test_translucent_texture_order();
    test_lighting();
    test_fog();
    test_edge_marking();
    test_rear_bitmap();
    test_anti_aliasing();
    if (!fails) printf("tex3d: all checks passed\n");
    return fails != 0;
}
