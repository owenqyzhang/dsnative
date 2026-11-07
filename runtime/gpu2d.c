/* dsnative runtime: the two 2D engines, one scanline at a time. Text, affine and extended
 * (affine-tiled and bitmap) backgrounds, sprites (regular and affine, 1D and 2D tile
 * mapping, extended palettes), priorities, colour effects and master brightness, plus the
 * VRAM display mode. Not yet: the 3D layer, windows, mosaic, bitmap sprites, the large
 * bitmap mode and main-memory display. Reference: GBATEK, DS Video. */
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

static u8 vbyte(Nds *s, int engine, int obj, u32 off) {
    u8 *p = vram_engine_ptr(s, engine, obj, off);
    return p ? *p : 0;
}

static u16 vhalf(Nds *s, int engine, int obj, u32 off) {
    u8 *p = vram_engine_ptr(s, engine, obj, off);
    return p ? rd16v(p) : 0;
}

static u16 ext_color(int region, int slot, u32 index) {
    u8 *p = vram_extpal[region][slot & 3];
    return p ? rd16v(p + (index & 0xFFF) * 2) : 0;
}

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
    for (int x = 0; x < SCREEN_W; x++) {
        u32 px = ((u32)x + hofs) & wmask;
        u32 blk = (px >> 8) + ((py >> 8) * ((size & 1) ? 2 : 1));
        u32 eoff = scrbase + blk * 0x800 + ((py & 255) >> 3) * 64 + ((px & 255) >> 3) * 2;
        u16 e = vhalf(s, engine, 0, eoff);
        u32 tx = px & 7, ty = py & 7;
        if (e & 0x400) tx = 7 - tx;
        if (e & 0x800) ty = 7 - ty;
        u32 tile = e & 0x3FF;
        u16 col;
        if (bpp8) {
            u8 idx = vbyte(s, engine, 0, charbase + tile * 64 + ty * 8 + tx);
            if (!idx) { L->px[x] = 0; continue; }
            col = ext ? ext_color(engine ? 2 : 0, extslot, (u32)(e >> 12) * 256 + idx) : pal16(s, engine, idx);
        } else {
            u8 b = vbyte(s, engine, 0, charbase + tile * 32 + ty * 4 + tx / 2);
            u8 idx = (tx & 1) ? b >> 4 : b & 15;
            if (!idx) { L->px[x] = 0; continue; }
            col = pal16(s, engine, (u32)(e >> 12) * 16 + idx);
        }
        L->px[x] = col | 0x8000;
    }
}

static void affine_bg(Nds *s, int engine, const u8 *io, u32 dispcnt, int bg, int line, Layer *L, int extended) {
    u16 cnt = rd16v(io + 8 + 2 * bg);
    const u8 *p = io + (bg == 2 ? 0x20 : 0x30);
    s32 pa = (s16)rd16v(p), pb = (s16)rd16v(p + 2), pc = (s16)rd16v(p + 4), pd = (s16)rd16v(p + 6);
    s32 rx, ry; memcpy(&rx, p + 8, 4); memcpy(&ry, p + 12, 4);
    rx = (rx << 4) >> 4; ry = (ry << 4) >> 4;       /* 28-bit signed */
    s32 x0 = rx + pb * line, y0 = ry + pd * line;
    int size = cnt >> 14, wrap = (cnt >> 13) & 1;
    u32 charbase = ((cnt >> 2) & 15) * 0x4000u, scrbase = ((cnt >> 8) & 31) * 0x800u;
    if (engine == 0) { charbase += ((dispcnt >> 24) & 7) * 0x10000u; scrbase += ((dispcnt >> 27) & 7) * 0x10000u; }
    int ext = (dispcnt >> 30) & 1;
    if (extended && (cnt & 0x80)) {             /* bitmap */
        static const int bw[4] = {128, 256, 512, 512}, bh[4] = {128, 256, 256, 512};
        int w = bw[size], hgt = bh[size];
        u32 base = ((cnt >> 8) & 31) * 0x4000u;
        int direct = (cnt >> 2) & 1;
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
    int dim = extended ? (128 << size) : (128 << size);
    for (int x = 0; x < SCREEN_W; x++) {
        s32 tx = (x0 + pa * x) >> 8, ty = (y0 + pc * x) >> 8;
        if (wrap) { tx &= dim - 1; ty &= dim - 1; }
        else if (tx < 0 || ty < 0 || tx >= dim || ty >= dim) { L->px[x] = 0; continue; }
        u32 mx = (u32)tx >> 3, my = (u32)ty >> 3, ix = (u32)tx & 7, iy = (u32)ty & 7;
        u16 col; u8 idx;
        if (extended) {                         /* 16-bit map entries, 8bpp tiles */
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

static const u8 obj_w[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
static const u8 obj_h[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};

static void objects(Nds *s, int engine, u32 dispcnt, int line, u16 *col, u8 *prio, u8 *semi) {
    const u8 *oam = s->oam + engine * 0x400;
    int map1d = (dispcnt >> 4) & 1;
    u32 bound = 32u << ((dispcnt >> 20) & 3);
    int ext = (dispcnt >> 31) & 1;
    for (int x = 0; x < SCREEN_W; x++) { col[x] = 0; prio[x] = 4; semi[x] = 0; }
    for (int i = 127; i >= 0; i--) {
        u16 a0 = rd16v(oam + i * 8), a1 = rd16v(oam + i * 8 + 2), a2 = rd16v(oam + i * 8 + 4);
        int affine = (a0 >> 8) & 1;
        if (!affine && (a0 & 0x200)) continue;
        int mode = (a0 >> 10) & 3;
        if (mode >= 2) continue;            /* window and bitmap sprites: not yet */
        int shape = a0 >> 14, sz = a1 >> 14;
        if (shape == 3) continue;
        int w = obj_w[shape][sz], hh = obj_h[shape][sz];
        int bw = w, bh = hh;
        if (affine && (a0 & 0x200)) { bw *= 2; bh *= 2; }
        int y = a0 & 0xFF; if (y >= 192) y -= 256;
        int x = a1 & 0x1FF; if (x >= 256) x -= 512;
        if (line < y || line >= y + bh) continue;
        int bpp8 = (a0 >> 13) & 1;
        u32 tile = a2 & 0x3FF, pr = (a2 >> 10) & 3, palb = a2 >> 12;
        s32 pa = 256, pb = 0, pc = 0, pd = 256;
        if (affine) {
            int g = (a1 >> 9) & 31;
            pa = (s16)rd16v(oam + g * 32 + 6); pb = (s16)rd16v(oam + g * 32 + 14);
            pc = (s16)rd16v(oam + g * 32 + 22); pd = (s16)rd16v(oam + g * 32 + 30);
        }
        int ly = line - y;
        for (int sx = 0; sx < bw; sx++) {
            int px = x + sx;
            if (px < 0 || px >= SCREEN_W) continue;
            int tx, ty;
            if (affine) {
                s32 cx = sx - bw / 2, cy = ly - bh / 2;
                tx = ((pa * cx + pb * cy) >> 8) + w / 2;
                ty = ((pc * cx + pd * cy) >> 8) + hh / 2;
                if (tx < 0 || ty < 0 || tx >= w || ty >= hh) continue;
            } else {
                tx = (a1 & 0x1000) ? w - 1 - sx : sx;
                ty = (a1 & 0x2000) ? hh - 1 - ly : ly;
            }
            u32 off;
            u32 tcol = (u32)tx >> 3, trow = (u32)ty >> 3, ix = (u32)tx & 7, iy = (u32)ty & 7;
            if (map1d) off = tile * bound + (trow * (u32)(w >> 3) + tcol) * (bpp8 ? 64 : 32);
            else off = tile * 32 + trow * 1024 + tcol * (bpp8 ? 64 : 32);   /* 2D: rows of 32 tiles */
            u16 c;
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
            if (pr <= prio[px]) { col[px] = c | 0x8000; prio[px] = (u8)pr; semi[px] = mode == 1; }
        }
    }
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

void gpu2d_render_line(Nds *s, int engine, int line, u32 *out) {
    const u8 *io = s->io[0] + (engine ? 0x1000 : 0);
    u32 dispcnt; memcpy(&dispcnt, io, 4);
    int on = (s->powcnt1 & 1) && (s->powcnt1 & (engine ? 0x200 : 0x2));
    if (!on) { for (int x = 0; x < SCREEN_W; x++) out[x] = 0xFF000000u; return; }
    int dmode = (dispcnt >> 16) & (engine ? 1 : 3);
    u16 line_px[SCREEN_W];
    if (dmode == 0) {
        for (int x = 0; x < SCREEN_W; x++) line_px[x] = 0x7FFF;
    } else if (dmode == 2) {
        int bank = (dispcnt >> 18) & 3;
        const u8 *v = s->vram + bank * 0x20000 + (u32)line * SCREEN_W * 2;
        for (int x = 0; x < SCREEN_W; x++) line_px[x] = rd16v(v + x * 2);
    } else if (dmode == 3) {
        for (int x = 0; x < SCREEN_W; x++) line_px[x] = 0;
    } else {
        int bgmode = dispcnt & 7;
        static Layer bgs[4];
        int en[4] = {0, 0, 0, 0};
        for (int i = 0; i < 4; i++) {
            if (!(dispcnt & (0x100u << i))) continue;
            if (i == 0 && engine == 0 && (dispcnt & 8)) continue;     /* 3D layer: not yet */
            int type = 0;   /* 0 text, 1 affine, 2 extended */
            if (i == 3) type = (bgmode == 1 || bgmode == 2) ? 1 : (bgmode >= 3 && bgmode <= 5) ? 2 : 0;
            if (i == 2) type = (bgmode == 2 || bgmode == 4) ? 1 : (bgmode == 5) ? 2 : 0;
            if (bgmode == 6 && i != 0) continue;
            if (type == 0) text_bg(s, engine, io, dispcnt, i, line, &bgs[i]);
            else affine_bg(s, engine, io, dispcnt, i, line, &bgs[i], type == 2);
            en[i] = 1;
        }
        static u16 ocol[SCREEN_W]; static u8 oprio[SCREEN_W], osemi[SCREEN_W];
        int objon = (dispcnt >> 12) & 1;
        if (objon) objects(s, engine, dispcnt, line, ocol, oprio, osemi);
        u16 bldcnt = rd16v(io + 0x50), bldalpha = rd16v(io + 0x52), bldy = rd16v(io + 0x54);
        u32 eva = bldalpha & 31, evb = (bldalpha >> 8) & 31, ey = bldy & 31;
        if (eva > 16) eva = 16; if (evb > 16) evb = 16; if (ey > 16) ey = 16;
        int effect = (bldcnt >> 6) & 3;
        u16 backdrop = pal16(s, engine, 0);
        for (int x = 0; x < SCREEN_W; x++) {
            /* the two front-most layers: id 0-3 BG, 4 OBJ, 5 backdrop */
            int id1 = 5, id2 = 5; u16 c1 = backdrop, c2 = backdrop; int p1 = 4, p2 = 4;
            for (int pr = 0; pr < 4; pr++) {
                if (objon && (ocol[x] & 0x8000) && oprio[x] == pr) {
                    if (id1 == 5) { id1 = 4; c1 = ocol[x]; p1 = pr; } else if (id2 == 5) { id2 = 4; c2 = ocol[x]; p2 = pr; }
                }
                for (int i = 0; i < 4; i++) {
                    if (!en[i] || (rd16v(io + 8 + 2 * i) & 3) != pr || !(bgs[i].px[x] & 0x8000)) continue;
                    if (id1 == 5) { id1 = i; c1 = bgs[i].px[x]; p1 = pr; } else if (id2 == 5) { id2 = i; c2 = bgs[i].px[x]; p2 = pr; }
                }
            }
            (void)p1; (void)p2;
            u16 c = c1 & 0x7FFF;
            int first = (bldcnt >> id1) & 1, second = (bldcnt >> (8 + id2)) & 1;
            if (id1 == 4 && osemi[x] && second) c = blend_alpha(c1, c2, eva, evb);
            else if (effect == 1 && first && second) c = blend_alpha(c1, c2, eva, evb);
            else if (effect == 2 && first) c = brighten(c, ey, 1);
            else if (effect == 3 && first) c = brighten(c, ey, 0);
            line_px[x] = c;
        }
    }
    u16 mb = rd16v(io + 0x6C);
    int mmode = mb >> 14; u32 mf = mb & 31; if (mf > 16) mf = 16;
    for (int x = 0; x < SCREEN_W; x++) {
        u16 c = line_px[x];
        if (mmode == 1) c = brighten(c, mf, 1); else if (mmode == 2) c = brighten(c, mf, 0);
        out[x] = bgr555(c);
    }
}
