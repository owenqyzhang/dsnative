/* dsnative runtime: the two 2D engines, one scanline at a time. Text, affine and extended
 * (affine-tiled and bitmap) backgrounds, the large 512x1024 bitmap, sprites (regular, affine and
 * bitmap, 1D and 2D mapping, extended palettes), the three windows, mosaic, priorities, colour
 * effects and master brightness, the VRAM and main memory display modes, and engine A's 3D layer
 * as BG0 (gpu3d_render.c draws it). Display capture is capture.c's: engine A's line is composed
 * here, shown, and then handed to it.
 * Reference: GBATEK, DS Video. */
#include "nds.h"

extern u8 *vram_extpal[4][4];

static inline u32 bgr555(u16 c) {
    u32 r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static inline u16 rd16v(const u8 *p) { u16 v; memcpy(&v, p, 2); return v; }

/* a layer line: colour | 0x8000 when opaque */
typedef struct { u16 px[SCREEN_W]; } Layer;

static u16 pal16(Nds *s, int engine, u32 index) { return rd16v(&s->pal[engine * 0x400 + (index & 0x1FF) * 2]); }

/* The engines' 16 KB VRAM block tables (nds.c), taken once a line: a lookup is then a mask and a load,
 * not a call. A block no bank maps reads as zero. */
static u8 *const *vtab[2][2];
static u32 vmask[2][2];

static inline u8 *vptr(int engine, int obj, u32 off) {
    u8 *b = vtab[engine][obj][(off >> 14) & vmask[engine][obj]];
    return b ? b + (off & 0x3FFF) : 0;
}

static inline u8 vbyte(Nds *s, int engine, int obj, u32 off) {
    (void)s;
    u8 *p = vptr(engine, obj, off);
    return p ? *p : 0;
}

static inline u16 vhalf(Nds *s, int engine, int obj, u32 off) {
    (void)s;
    u8 *p = vptr(engine, obj, off);
    return p ? rd16v(p) : 0;
}

static u16 ext_color(int region, int slot, u32 index) {
    u8 *p = vram_extpal[region][slot & 3];
    return p ? rd16v(p + (index & 0xFFF) * 2) : 0;
}

/* The 3D layer as BG0: BG0HOFS scrolls a region of 512 pixels, the 256 of the 3D image then
 * 256 transparent ones; vertical scroll and mosaic have no effect. alpha is the 3D pixel's. */
static void bg0_3d(Nds *s, const u8 *io, int line, Layer *L, u8 *alpha) {
    u16 px[SCREEN_W]; u8 a[SCREEN_W];
    gpu3d_bg0_line(s, line, px, a);
    u32 hofs = rd16v(io + 0x10) & 0x1FF;
    for (u32 x = 0; x < SCREEN_W; x++) {
        u32 sx = (x + hofs) & 0x1FF;
        L->px[x] = sx < SCREEN_W ? px[sx] : 0;
        alpha[x] = sx < SCREEN_W ? a[sx] : 0;
    }
}

/* One map entry and one row of the tile it names per 8 pixels, not per pixel. The row (8 bytes at 256
 * colours, 4 at 16) is aligned and so never leaves its 16 KB block. */
static void text_bg(Nds *s, int engine, const u8 *io, u32 dispcnt, int bg, int line, Layer *L) {
    u16 cnt = rd16v(io + 8 + 2 * bg);
    u32 hofs = rd16v(io + 0x10 + 4 * bg) & 0x1FF, vofs = rd16v(io + 0x12 + 4 * bg) & 0x1FF;
    u32 charbase = ((cnt >> 2) & 15) * 0x4000u, scrbase = ((cnt >> 8) & 31) * 0x800u;
    if (engine == 0) { charbase += ((dispcnt >> 24) & 7) * 0x10000u; scrbase += ((dispcnt >> 27) & 7) * 0x10000u; }
    int bpp8 = (cnt >> 7) & 1, size = cnt >> 14;
    u32 wmask = (size & 1) ? 511 : 255, hmask = (size & 2) ? 511 : 255;
    int extslot = bg < 2 && (cnt & 0x2000) ? bg + 2 : bg;
    int ext = (dispcnt >> 30) & 1;
    u32 py = ((u32)line + vofs) & hmask;
    u32 map_row = scrbase + ((py & 255) >> 3) * 64, map_blk_row = (py >> 8) * ((size & 1) ? 2 : 1);
    const u8 *palp = s->pal + engine * 0x400;
    for (u32 x = 0; x < SCREEN_W;) {
        u32 px = (x + hofs) & wmask;
        u32 eoff = map_row + ((px >> 8) + map_blk_row) * 0x800 + ((px & 255) >> 3) * 2;
        u16 e = vhalf(s, engine, 0, eoff);
        u32 ty = py & 7, t0 = px & 7, tile = e & 0x3FF;
        if (e & 0x800) ty = 7 - ty;
        u32 n = 8 - t0;
        if (x + n > SCREEN_W) n = SCREEN_W - x;
        int hf = (e & 0x400) != 0;
        if (bpp8) {
            const u8 *row = vptr(engine, 0, charbase + tile * 64 + ty * 8);
            for (u32 k = 0; k < n; k++) {
                u32 tx = hf ? 7 - (t0 + k) : t0 + k;
                u8 idx = row ? row[tx] : 0;
                if (!idx) { L->px[x + k] = 0; continue; }
                u16 col = ext ? ext_color(engine ? 2 : 0, extslot, (u32)(e >> 12) * 256 + idx) : rd16v(palp + ((u32)idx & 0x1FF) * 2);
                L->px[x + k] = col | 0x8000;
            }
        } else {
            const u8 *row = vptr(engine, 0, charbase + tile * 32 + ty * 4);
            const u8 *pal = palp + (((u32)(e >> 12) * 16) & 0x1FF) * 2;
            for (u32 k = 0; k < n; k++) {
                u32 tx = hf ? 7 - (t0 + k) : t0 + k;
                u8 b = row ? row[tx >> 1] : 0;
                u8 idx = (tx & 1) ? b >> 4 : b & 15;
                if (!idx) { L->px[x + k] = 0; continue; }
                /* the palette index is (palette * 16 + idx) & 0x1FF; palette * 16 is a multiple of 16, so adding idx never carries */
                L->px[x + k] = rd16v(pal + (u32)idx * 2) | 0x8000;
            }
        }
        x += n;
    }
}

/* The internal reference points (GBATEK, "Internal Reference Point Registers"): BG2X/Y and BG3X/Y are
 * copied at VBlank, advance by PB/PD a line, and a write outside VBlank copies at once, so the line it
 * lands in starts from the new value (the next line, for a write in HBlank: the line is drawn). X and Y
 * are separate registers: a write to one copies that one and the other keeps advancing, so each axis is
 * kept as the value its own `line` started from; a later line adds PB/PD for the lines in between, so a
 * change of PB or PD in the middle of a frame is taken as if it had held from the line of the last copy. */
int gpu2d_io_write(Nds *s, int cpu, u32 off, int size, u32 v) {
    (void)v;
    if (cpu != 0 || off >= 0x2000) return 0;
    int e = off >= 0x1000;
    u32 o = off & 0xFFF;
    for (int bg = 0; bg < 2; bg++)
        for (int a = 0; a < 2; a++) {
            u32 r = (bg ? 0x38u : 0x28u) + 4u * (u32)a;
            if (o < r + 4 && o + (u32)size > r) {
                s->bgref[e][bg].pend[a] = (s32)s->vcount + ((s->dispstat[0] & 2) ? 1 : 0);
                s->bgref[e][bg].dirty[a] = 1;
            }
        }
    return 0;
}

static void ref_sync(Nds *s, int engine, int line) {
    const u8 *io = s->io[0] + (engine ? 0x1000 : 0);
    for (int bg = 0; bg < 2; bg++) {
        BgRef *r = &s->bgref[engine][bg];
        const u8 *p = io + (bg ? 0x38 : 0x28);
        for (int a = 0; a < 2; a++) {
            if (line != 0 && !r->dirty[a]) continue;
            s32 v; memcpy(&v, p + 4 * a, 4);
            r->v[a] = (v << 4) >> 4;                /* 28-bit signed */
            r->line[a] = line ? r->pend[a] : 0;
            r->dirty[a] = 0;
        }
    }
}

/* type: 1 affine (8-bit map entries), 2 extended (16-bit entries, or a bitmap when BGxCNT.7 is set),
 * 3 the large 512x1024 / 1024x512 bitmap of mode 6 (BG2, engine A) */
static void affine_bg(Nds *s, int engine, const u8 *io, u32 dispcnt, int bg, int line, Layer *L, int type) {
    u16 cnt = rd16v(io + 8 + 2 * bg);
    const u8 *p = io + (bg == 2 ? 0x20 : 0x30);
    s32 pa = (s16)rd16v(p), pb = (s16)rd16v(p + 2), pc = (s16)rd16v(p + 4), pd = (s16)rd16v(p + 6);
    const BgRef *r = &s->bgref[engine][bg - 2];
    s32 x0 = r->v[0] + pb * (line - r->line[0]), y0 = r->v[1] + pd * (line - r->line[1]);
    int size = cnt >> 14, wrap = (cnt >> 13) & 1;
    u32 charbase = ((cnt >> 2) & 15) * 0x4000u, scrbase = ((cnt >> 8) & 31) * 0x800u;
    if (engine == 0) { charbase += ((dispcnt >> 24) & 7) * 0x10000u; scrbase += ((dispcnt >> 27) & 7) * 0x10000u; }
    int ext = (dispcnt >> 30) & 1;
    if (type == 3 || (type == 2 && (cnt & 0x80))) {     /* bitmap */
        static const int bw[4] = {128, 256, 512, 512}, bh[4] = {128, 256, 256, 512};
        int w = bw[size], hgt = bh[size];
        u32 base = ((cnt >> 8) & 31) * 0x4000u;         /* 16 KB steps, without DISPCNT's 64 KB ones */
        int direct = (cnt >> 2) & 1;
        if (type == 3) { w = (size & 1) ? 1024 : 512; hgt = (size & 1) ? 512 : 1024; base = 0; direct = 0; }
        for (int x = 0; x < SCREEN_W; x++) {
            s32 tx = (x0 + pa * x) >> 8, ty = (y0 + pc * x) >> 8;
            if (wrap) { tx &= w - 1; ty &= hgt - 1; }
            else if (tx < 0 || ty < 0 || tx >= w || ty >= hgt) { L->px[x] = 0; continue; }
            if (direct) {
                u16 c = vhalf(s, engine, 0, base + ((u32)ty * (u32)w + (u32)tx) * 2);
                L->px[x] = (c & 0x8000) ? c : 0;
            } else {
                u8 idx = vbyte(s, engine, 0, base + (u32)ty * (u32)w + (u32)tx);
                L->px[x] = idx ? (pal16(s, engine, idx) | 0x8000) : 0;
            }
        }
        return;
    }
    int dim = 128 << size;
    for (int x = 0; x < SCREEN_W; x++) {
        s32 tx = (x0 + pa * x) >> 8, ty = (y0 + pc * x) >> 8;
        if (wrap) { tx &= dim - 1; ty &= dim - 1; }
        else if (tx < 0 || ty < 0 || tx >= dim || ty >= dim) { L->px[x] = 0; continue; }
        u32 mx = (u32)tx >> 3, my = (u32)ty >> 3, ix = (u32)tx & 7, iy = (u32)ty & 7;
        u16 col; u8 idx;
        if (type == 2) {                        /* 16-bit map entries, 8bpp tiles */
            u16 e = vhalf(s, engine, 0, scrbase + (my * (u32)(dim >> 3) + mx) * 2);
            if (e & 0x400) ix = 7 - ix;
            if (e & 0x800) iy = 7 - iy;
            idx = vbyte(s, engine, 0, charbase + (e & 0x3FFu) * 64 + iy * 8 + ix);
            if (!idx) { L->px[x] = 0; continue; }
            col = ext ? ext_color(engine ? 2 : 0, bg, (u32)(e >> 12) * 256 + idx) : pal16(s, engine, idx);
        } else {
            u8 tile = vbyte(s, engine, 0, scrbase + my * (u32)(dim >> 3) + mx);
            idx = vbyte(s, engine, 0, charbase + tile * 64u + iy * 8 + ix);
            if (!idx) { L->px[x] = 0; continue; }
            col = pal16(s, engine, idx);
        }
        L->px[x] = col | 0x8000;
    }
}

/* A background line. BGxCNT.6 turns the mosaic on: the layer is drawn for the first line of
 * each block of lines and the first pixel of each block of columns is repeated (MOSAIC 0-7). */
static void bg_line(Nds *s, int engine, const u8 *io, u32 dispcnt, int bg, int type, int line, Layer *L) {
    u16 cnt = rd16v(io + 8 + 2 * bg), mos = rd16v(io + 0x4C);
    u32 mh = (mos & 15) + 1u, mv = ((mos >> 4) & 15) + 1u;
    int mosaic = (cnt & 0x40) && (mh > 1 || mv > 1);
    if (mosaic) line -= (int)((u32)line % mv);
    if (type == 0) text_bg(s, engine, io, dispcnt, bg, line, L);
    else affine_bg(s, engine, io, dispcnt, bg, line, L, type);
    if (mosaic && mh > 1) for (u32 x = 0; x < SCREEN_W; x++) L->px[x] = L->px[x - x % mh];
}

static const u8 obj_w[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
static const u8 obj_h[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};

typedef struct {
    u16 col[SCREEN_W];          /* colour | 0x8000 when a sprite is there */
    u8 prio[SCREEN_W];
    u8 semi[SCREEN_W];          /* 0 opaque, 1 semi-transparent (BLDALPHA), 2 bitmap sprite blending by its own alpha */
    u8 eva[SCREEN_W];           /* that alpha as a coefficient of 1..16 */
    u8 win[SCREEN_W];           /* inside the OBJ window */
} ObjLine;

/* Returns whether any sprite pixel is semi-transparent (blends with what is behind it). */
static int objects(Nds *s, int engine, const u8 *io, u32 dispcnt, int line, ObjLine *o) {
    const u8 *oam = s->oam + engine * 0x400;
    int map1d = (dispcnt >> 4) & 1;
    u32 bound = 32u << ((dispcnt >> 20) & 3);
    int ext = (dispcnt >> 31) & 1;
    u16 mos = rd16v(io + 0x4C);
    u32 omh = ((mos >> 8) & 15) + 1u, omv = ((mos >> 12) & 15) + 1u;
    int any_semi = 0;
    memset(o->col, 0, sizeof o->col); memset(o->prio, 4, sizeof o->prio); memset(o->semi, 0, sizeof o->semi);
    memset(o->eva, 0, sizeof o->eva); memset(o->win, 0, sizeof o->win);
    for (int i = 127; i >= 0; i--) {
        u16 a0 = rd16v(oam + i * 8), a1 = rd16v(oam + i * 8 + 2), a2 = rd16v(oam + i * 8 + 4);
        int affine = (a0 >> 8) & 1;
        if (!affine && (a0 & 0x200)) continue;
        int mode = (a0 >> 10) & 3;
        if (mode == 2 && !(dispcnt & 0x8000)) continue;     /* OBJ window sprites need DISPCNT.15 */
        int shape = a0 >> 14, sz = a1 >> 14;
        if (shape == 3) continue;
        int w = obj_w[shape][sz], hh = obj_h[shape][sz];
        int bw = w, bh = hh;
        if (affine && (a0 & 0x200)) { bw *= 2; bh *= 2; }
        /* the row within the sprite's box, modulo 256: a sprite that runs off the bottom
         * shows at the top as well (GBATEK, OBJ Vertical Wrap) */
        int ly = (line - (a0 & 0xFF)) & 0xFF;
        if (ly >= bh) continue;
        int x = a1 & 0x1FF; if (x >= 256) x -= 512;
        if (x + bw <= 0 || x >= SCREEN_W) continue;
        int mosaic = (a0 & 0x1000) != 0;
        if (mosaic) { ly -= (int)((u32)line % omv); if (ly < 0) ly = 0; }
        int bpp8 = (a0 >> 13) & 1;
        u32 tile = a2 & 0x3FF, pr = (a2 >> 10) & 3, palb = a2 >> 12;
        s32 pa = 256, pb = 0, pc = 0, pd = 256;
        if (affine) {
            int g = (a1 >> 9) & 31;
            pa = (s16)rd16v(oam + g * 32 + 6); pb = (s16)rd16v(oam + g * 32 + 14);
            pc = (s16)rd16v(oam + g * 32 + 22); pd = (s16)rd16v(oam + g * 32 + 30);
        }
        for (int sx = 0; sx < bw; sx++) {
            int px = x + sx;
            if (px < 0 || px >= SCREEN_W) continue;
            int mx = sx;
            if (mosaic) { mx -= (int)((u32)px % omh); if (mx < 0) mx = 0; }    /* blocks are aligned to the screen */
            int tx, ty;
            if (affine) {
                s32 cx = mx - bw / 2, cy = ly - bh / 2;
                tx = ((pa * cx + pb * cy) >> 8) + w / 2;
                ty = ((pc * cx + pd * cy) >> 8) + hh / 2;
                if (tx < 0 || ty < 0 || tx >= w || ty >= hh) continue;
            } else {
                tx = (a1 & 0x1000) ? w - 1 - mx : mx;
                ty = (a1 & 0x2000) ? hh - 1 - ly : ly;
            }
            u16 c;
            int semi = mode == 1, eva = 0;
            if (mode == 3) {                    /* bitmap sprite: direct colour with an alpha bit; bits 12-15 of attribute 2 are its alpha */
                u32 off;
                if (dispcnt & 0x40) {           /* 1D: rows are the sprite's width apart */
                    u32 bd = (engine == 0 && (dispcnt & 0x400000)) ? 256 : 128;
                    off = tile * bd + ((u32)ty * (u32)w + (u32)tx) * 2;
                } else {                        /* 2D: a bitmap 128 or 256 pixels wide, tile numbers split into x and y */
                    u32 maskx = (dispcnt & 0x20) ? 0x1F : 0x0F, stride = (dispcnt & 0x20) ? 512 : 256;
                    off = (tile & maskx) * 16 + (tile & ~maskx) * 0x80 + (u32)ty * stride + (u32)tx * 2;
                }
                c = vhalf(s, engine, 1, off);
                if (!(c & 0x8000)) continue;
                semi = 2; eva = (int)palb + 1;
            } else {
                u32 off;
                u32 tcol = (u32)tx >> 3, trow = (u32)ty >> 3, ix = (u32)tx & 7, iy = (u32)ty & 7;
                if (map1d) off = tile * bound + (trow * (u32)(w >> 3) + tcol) * (bpp8 ? 64 : 32);
                else off = tile * 32 + trow * 1024 + tcol * (bpp8 ? 64 : 32);   /* 2D: rows of 32 tiles */
                if (bpp8) {
                    u8 idx = vbyte(s, engine, 1, off + iy * 8 + ix);
                    if (!idx) continue;
                    c = ext ? ext_color(engine ? 3 : 1, 0, palb * 256 + idx) : pal16(s, engine, 0x100 + idx);
                } else {
                    u8 b = vbyte(s, engine, 1, off + iy * 4 + ix / 2);
                    u8 idx = (ix & 1) ? b >> 4 : b & 15;
                    if (!idx) continue;
                    c = pal16(s, engine, 0x100 + palb * 16 + idx);
                }
            }
            if (mode == 2) { o->win[px] = 1; continue; }    /* its colour and priority don't matter */
            if ((int)pr <= o->prio[px]) {
                o->col[px] = c | 0x8000; o->prio[px] = (u8)pr; o->semi[px] = (u8)semi; o->eva[px] = (u8)eva;
                any_semi |= semi;
            }
        }
    }
    return any_semi;
}

static u16 blend_alpha(u16 a, u16 b, u32 eva, u32 evb) {
    u32 r = ((a & 31) * eva + (b & 31) * evb) >> 4;
    u32 g = (((a >> 5) & 31) * eva + ((b >> 5) & 31) * evb) >> 4;
    u32 bl = (((a >> 10) & 31) * eva + ((b >> 10) & 31) * evb) >> 4;
    if (r > 31) r = 31; if (g > 31) g = 31; if (bl > 31) bl = 31;
    return (u16)(r | (g << 5) | (bl << 10));
}

static u16 brighten(u16 c, u32 y, int up) {
    u32 r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    if (up) { r += ((31 - r) * y) >> 4; g += ((31 - g) * y) >> 4; b += ((31 - b) * y) >> 4; }
    else { r -= (r * y) >> 4; g -= (g * y) >> 4; b -= (b * y) >> 4; }
    return (u16)(r | (g << 5) | (b << 10));
}

/* WIN0H/WIN1H: X1 in the high byte, X2 (exclusive) in the low. X2 = 0 is 256, and so is an X2 below
 * X1; X1 = X2 = 0 is no window at all (GBATEK, DS Window Glitches). Returns [*x1, *x2). */
static void win_columns(u16 v, int *x1, int *x2) {
    *x1 = v >> 8; *x2 = v & 0xFF;
    if (!*x1 && !*x2) return;
    if (!*x2 || *x1 > *x2) *x2 = 256;
}

/* WIN0V/WIN1V: Y1 high, Y2 (exclusive) low. A Y2 over 192 or below Y1 reaches the last line. The DS
 * compares only 8 bits of its 0..262 line counter, so Y1 of 1..6 also fires in lines 257..262 and
 * the window is already on at line 0 (until Y2). */
static int win_rows(u16 v, int line) {
    int y1 = v >> 8, y2 = v & 0xFF;
    int end = y2 > 192 ? 192 : y2;
    if (line >= y1 && line < (y1 > y2 ? 192 : end)) return 1;
    return y1 >= 1 && y1 <= 6 && line < end;
}

/* One engine's composed line (BG, 3D, OBJ, windows, effects), as 15-bit colours. */
static void compose_line(Nds *s, int engine, int line, u16 *line_px) {
    const u8 *io = s->io[0] + (engine ? 0x1000 : 0);
    u32 dispcnt; memcpy(&dispcnt, io, 4);
    int bgmode = dispcnt & 7;
    Layer bgs[4];
    u8 a3d[SCREEN_W];
    int en[4] = {0, 0, 0, 0};
    int is3d = engine == 0 && (dispcnt & 8) && (dispcnt & 0x100);
    for (int i = 0; i < 4; i++) {
        if (!(dispcnt & (0x100u << i))) continue;
        if (i == 0 && is3d) { bg0_3d(s, io, line, &bgs[0], a3d); en[0] = 1; continue; }
        int type = 0;   /* 0 text, 1 affine, 2 extended, 3 large bitmap */
        if (i == 3) type = (bgmode == 1 || bgmode == 2) ? 1 : (bgmode >= 3 && bgmode <= 5) ? 2 : 0;
        if (i == 2) type = (bgmode == 2 || bgmode == 4) ? 1 : (bgmode == 5) ? 2 : 0;
        if (bgmode == 6) { if (i != 0 && !(i == 2 && engine == 0)) continue; if (i == 2) type = 3; }
        bg_line(s, engine, io, dispcnt, i, type, line, &bgs[i]);
        en[i] = 1;
    }
    ObjLine obj;
    int objon = (dispcnt >> 12) & 1, any_semi = 0;
    if (objon) any_semi = objects(s, engine, io, dispcnt, line, &obj);
    /* windows: per pixel, which layers (bits 0-3 BG, 4 OBJ) and whether effects (5) are on there */
    u8 wmask[SCREEN_W];
    u32 winen = (dispcnt >> 13) & 7;
    if (!winen) memset(wmask, 0x3F, sizeof wmask);
    else {
        u16 winin = rd16v(io + 0x48), winout = rd16v(io + 0x4A);
        int in[2] = {0, 0}, c1[2] = {0, 0}, c2[2] = {0, 0};
        for (int w = 0; w < 2; w++) {
            if (!(winen & (1u << w))) continue;
            in[w] = win_rows(rd16v(io + 0x44 + 2 * w), line);
            win_columns(rd16v(io + 0x40 + 2 * w), &c1[w], &c2[w]);
        }
        for (int x = 0; x < SCREEN_W; x++) {
            u8 m = winout & 0x3F;
            if ((winen & 4) && objon && obj.win[x]) m = (winout >> 8) & 0x3F;
            if (in[1] && x >= c1[1] && x < c2[1]) m = (winin >> 8) & 0x3F;
            if (in[0] && x >= c1[0] && x < c2[0]) m = winin & 0x3F;
            wmask[x] = m;
        }
    }
    int bgprio[4];
    for (int i = 0; i < 4; i++) bgprio[i] = rd16v(io + 8 + 2 * i) & 3;
    u16 bldcnt = rd16v(io + 0x50), bldalpha = rd16v(io + 0x52), bldy = rd16v(io + 0x54);
    u32 eva = bldalpha & 31, evb = (bldalpha >> 8) & 31, ey = bldy & 31;
    if (eva > 16) eva = 16; if (evb > 16) evb = 16; if (ey > 16) ey = 16;
    int effect = (bldcnt >> 6) & 3;
    u16 backdrop = pal16(s, engine, 0);
    /* the layers front to back: for each priority the sprites, then the backgrounds by number */
    int seq_id[8], seq_pr[8], nseq = 0;        /* up to four BGs, and a sprite entry for each priority */
    for (int pr = 0; pr < 4; pr++) {
        if (objon) { seq_id[nseq] = 4; seq_pr[nseq++] = pr; }
        for (int i = 0; i < 4; i++) if (en[i] && bgprio[i] == pr) { seq_id[nseq] = i; seq_pr[nseq++] = pr; }
    }
    /* the layer behind the first is only looked at when a blend can use it */
    int need2 = effect == 1 || any_semi;
    if (!winen && effect == 0 && !any_semi) {
        /* nothing but the front-most opaque layer shows: paint the layers back to front, a plain pass each */
        for (int x = 0; x < SCREEN_W; x++) line_px[x] = backdrop & 0x7FFF;
        for (int k = nseq - 1; k >= 0; k--) {
            int id = seq_id[k];
            if (id == 4) {
                u8 pr = (u8)seq_pr[k];
                for (int x = 0; x < SCREEN_W; x++) if ((obj.col[x] & 0x8000) && obj.prio[x] == pr) line_px[x] = obj.col[x] & 0x7FFF;
            } else {
                const u16 *px = bgs[id].px;
                for (int x = 0; x < SCREEN_W; x++) if (px[x] & 0x8000) line_px[x] = px[x] & 0x7FFF;
            }
        }
        return;
    }
    for (int x = 0; x < SCREEN_W; x++) {
        u8 m = wmask[x];
        /* the two front-most layers: id 0-3 BG, 4 OBJ, 5 backdrop */
        int id1 = 5, id2 = 5; u16 c1 = backdrop, c2 = backdrop;
        for (int k = 0; k < nseq; k++) {
            int id = seq_id[k];
            u16 col;
            if (id == 4) {
                if (!((m & 0x10) && (obj.col[x] & 0x8000) && obj.prio[x] == seq_pr[k])) continue;
                col = obj.col[x];
            } else {
                if (!((m >> id) & 1) || !(bgs[id].px[x] & 0x8000)) continue;
                col = bgs[id].px[x];
            }
            if (id1 == 5) { id1 = id; c1 = col; if (!need2) break; }
            else { id2 = id; c2 = col; break; }
        }
        u16 c = c1 & 0x7FFF;
        if (m & 0x20) {                         /* effects are on in this window region */
            int first = (bldcnt >> id1) & 1, second = (bldcnt >> (8 + id2)) & 1;
            if (id1 == 4 && obj.semi[x] && second) {
                if (obj.semi[x] == 2) c = blend_alpha(c1, c2, obj.eva[x], 16u - obj.eva[x]);
                else c = blend_alpha(c1, c2, eva, evb);
            }
            else if (effect == 1 && first && second) {
                if (id1 != 0 || !is3d) c = blend_alpha(c1, c2, eva, evb);
                else if (a3d[x] < 31) c = blend_alpha(c1, c2, a3d[x] / 2u, 16 - a3d[x] / 2u);   /* the 3D layer blends by its own alpha, never EVA/EVB (GBATEK's guess); 31 is opaque */
            }
            else if (effect == 2 && first) c = brighten(c, ey, 1);
            else if (effect == 3 && first) c = brighten(c, ey, 0);
        }
        line_px[x] = c;
    }
}

void gpu2d_render_line(Nds *s, int engine, int line, u32 *out) {
    for (int e = 0; e < 2; e++) for (int o = 0; o < 2; o++) vtab[e][o] = vram_engine_table(e, o, &vmask[e][o]);
    ref_sync(s, engine, line);
    const u8 *io = s->io[0] + (engine ? 0x1000 : 0);
    u32 dispcnt; memcpy(&dispcnt, io, 4);
    int on = (s->powcnt1 & 1) && (s->powcnt1 & (engine ? 0x200 : 0x2));
    if (!on) { for (int x = 0; x < SCREEN_W; x++) out[x] = 0xFF000000u; return; }
    int dmode = (dispcnt >> 16) & (engine ? 1 : 3);
    int cap = engine == 0 && capture_covers(s, line);
    u16 gfx[SCREEN_W], fifo[SCREEN_W], line_px[SCREEN_W];
    /* a capture takes the composed output whatever the display shows */
    if (dmode == 1 || (cap && capture_wants_graphics(s))) compose_line(s, engine, line, gfx);
    if (engine == 0 && (dmode == 3 || (cap && capture_wants_fifo(s)))) capture_fifo_take(s, fifo);
    if (dmode == 0) {
        for (int x = 0; x < SCREEN_W; x++) line_px[x] = 0x7FFF;
    } else if (dmode == 2) {
        int bank = (dispcnt >> 18) & 3;
        const u8 *v = s->vram + bank * 0x20000 + (u32)line * SCREEN_W * 2;
        for (int x = 0; x < SCREEN_W; x++) line_px[x] = rd16v(v + x * 2);
    } else if (dmode == 3) {
        memcpy(line_px, fifo, sizeof line_px);
    } else {
        memcpy(line_px, gfx, sizeof line_px);
    }
    if (cap) capture_line(s, line, gfx, fifo, dmode == 2);     /* after the display has read its VRAM line */
    u16 mb = rd16v(io + 0x6C);
    int mmode = mb >> 14; u32 mf = mb & 31; if (mf > 16) mf = 16;
    if (mmode == 1 || mmode == 2) {
        for (int x = 0; x < SCREEN_W; x++) out[x] = bgr555(brighten(line_px[x], mf, mmode == 1));
    } else {
        for (int x = 0; x < SCREEN_W; x++) out[x] = bgr555(line_px[x]);
    }
}
