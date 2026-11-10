/* dsnative runtime: the 3D rasterizer. Draws the polygon list SWAP_BUFFERS handed over into a
 * 256x192 colour + alpha frame with a depth buffer (Z or W): the rear plane, then the opaque
 * polygons in the order they were sent, then the translucent ones (sorted by Y unless the swap
 * before the list asked for manual order; a polygon with a translucent texture counts as one,
 * see late_pass), each scanline by scanline; then edge marking, fog and anti-aliasing over the
 * whole frame. Reference: GBATEK, DS 3D Video.
 *
 * Vertices sit on whole pixels, as on the DS. A pixel is drawn when its centre is inside the
 * polygon, so the right and bottom edges are left out and polygons that share an edge never
 * both draw its pixels (GBATEK: small polygons exclude those edges; with edge marking or
 * anti-aliasing on, opaque polygons are full size, and so are translucent ones when alpha
 * blending is on: their whole outline is drawn). A polygon with no area on screen (a line or a
 * dot, which a game makes with repeated vertices) draws every pixel along its run, at least one
 * per row; a polygon no bigger than 1x1 is one pixel (GBATEK rounds width and height up to 1).
 * Colour, W and the texture coordinates are interpolated perspective-correctly (w scaled to 16
 * bits), Z linearly in screen space.
 *
 * Per pixel: depth test, texture fetch (one texel, no filtering) and blend with the vertex
 * colour by polygon mode, alpha test, then the opaque or translucent write. Shadow polygons
 * set and test a stencil bit instead of (or as well as) drawing. */
#include <stdlib.h>
#include "nds.h"
#include "workers.h"

extern u8 *vram_texslot[4], *vram_texpal[8];    /* nds.c: the VRAM banks mapped as texture image and palette */

/* attribute buffer, per pixel: bits 0-5 the ID of the opaque polygon there, 6-11 that of the last
 * translucent one (A_TRANS says one is), bit 13 the fog flag, 14 a possible edge for edge marking,
 * 15 the shadow stencil, 16-20 and 21 the anti-aliasing coverage of an edge pixel and its side */
enum { A_TRANS = 1 << 12, A_FOG = 1 << 13, A_EDGE = 1 << 14, A_STENCIL = 1 << 15, A_COV = 16, A_RIGHT = 1 << 21 };

#define PIX(r, g, b, a) ((u32)(r) | (u32)(g) << 6 | (u32)(b) << 12 | (u32)(a) << 18)

static inline u32 x6(u32 c) { return c ? c * 2 + 1 : 0; }      /* 5-bit to 6-bit */
static inline u16 rd16r(const u8 *p) { u16 v; memcpy(&v, p, 2); return v; }
static inline u32 rd32r(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }
static inline u32 rgb6(u16 c) { return PIX(x6(c & 31), x6((c >> 5) & 31), x6((c >> 10) & 31), 0); }
/* the 15-bit clear depth as 24 bits: 0x7FFF is the farthest, 0xFFFFFF */
static inline u32 depth24(u32 d) { d &= 0x7FFF; return d * 0x200 + ((d + 1) / 0x8000) * 0x1FF; }

/* Texture image memory is the four 128 KB slots (VRAM banks A-D mapped as texture, whatever
 * order); texture palette memory is eight 16 KB slots (E, F and G). A slot no bank is mapped to
 * reads as zero: GBATEK does not say, a guess. */
static inline u32 tex8(u32 a) { const u8 *p = vram_texslot[(a >> 17) & 3]; return p ? p[a & 0x1FFFF] : 0; }
static inline u32 tex16(u32 a) { return tex8(a) | tex8(a + 1) << 8; }
static inline u32 pal16(u32 a) { const u8 *p = vram_texpal[(a >> 14) & 7]; return p ? rd16r(p + (a & 0x3FFE)) : 0; }

typedef struct {
    Gpu3d *g;
    const u32 *toon;                    /* TOON_TABLE as 6-bit colours */
    u32 attr, disp;                     /* POLYGON_ATTR, DISP3DCNT */
    u32 alpha, id;
    int wire, wbuf, full, mode;         /* mode: POLYGON_ATTR bits 4-5; full: draw the whole outline */
    int mask;                           /* a shadow mask polygon: no colour, only the stencil */
    int aa;                             /* give an opaque polygon's side pixels a coverage */
    int edge;                           /* flag an opaque polygon's outline for edge marking */
    int tex, rep_s, rep_t, flip_s, flip_t, trans0;     /* tex: the format (0..7), 0 when the polygon draws untextured */
    u32 sw, sh, base, pbase;            /* texture size in texels, image address, palette address */
} Poly;

/* where a polygon edge crosses a scanline's centre */
typedef struct {
    s64 x, lo, hi;                      /* 16.16: the crossing, and the edge's run within the row */
    s64 w;                              /* clip w */
    s32 z, wn;                          /* Z depth, w scaled to 16 bits */
    s32 r, g, b;                        /* 6-bit colour * 256 */
    s32 s, t;                           /* texture coordinates, 12.4 texels * 256 */
} Cross;

/* ---- textures ------------------------------------------------------------------------------ */
typedef struct { u32 r, g, b, a; } Texel;       /* 6-bit colour, 5-bit alpha */

static inline s32 wrap(s32 v, u32 size, int repeat, int flip) {
    if (!repeat) return v < 0 ? 0 : v >= (s32)size ? (s32)size - 1 : v;
    if (flip && (v & (s32)size)) return (s32)size - 1 - (v & ((s32)size - 1));
    return v & ((s32)size - 1);
}

static inline void texel_rgb(u32 c, Texel *t) { t->r = x6(c & 31); t->g = x6((c >> 5) & 31); t->b = x6((c >> 10) & 31); }

/* a colour between two 15-bit ones, per 5-bit component: (a * wa + b * wb) / div */
static u32 mix15(u32 a, u32 b, u32 wa, u32 wb, u32 div) {
    u32 o = 0;
    for (int k = 0; k < 15; k += 5) o |= ((((a >> k) & 31) * wa + ((b >> k) & 31) * wb) / div) << k;
    return o;
}

/* The texel at (s, t) in 12.12 texels. */
static void fetch(const Poly *P, s32 s, s32 t, Texel *o) {
    s32 x = wrap(s >> 12, P->sw, P->rep_s, P->flip_s), y = wrap(t >> 12, P->sh, P->rep_t, P->flip_t);
    u32 idx = (u32)y * P->sw + (u32)x, v;
    o->a = 31;
    switch (P->tex) {
    case 1:                             /* A3I5: alpha 0..7 becomes 0..31 as a * 4 + a / 2 */
        v = tex8(P->base + idx);
        texel_rgb(pal16(P->pbase + (v & 31) * 2), o);
        o->a = (v >> 5) * 4 + (v >> 6);
        break;
    case 2:
        v = tex8(P->base + (idx >> 2)) >> ((idx & 3) * 2) & 3;
        texel_rgb(pal16(P->pbase + v * 2), o);
        if (!v && P->trans0) o->a = 0;
        break;
    case 3:
        v = tex8(P->base + (idx >> 1)) >> ((idx & 1) * 4) & 15;
        texel_rgb(pal16(P->pbase + v * 2), o);
        if (!v && P->trans0) o->a = 0;
        break;
    case 4:
        v = tex8(P->base + idx);
        texel_rgb(pal16(P->pbase + v * 2), o);
        if (!v && P->trans0) o->a = 0;
        break;
    case 5: {                           /* 4x4 blocks in raster order, 4 bytes (a row each) per block; palette index halfwords in slot 1 */
        u32 blk = (u32)(y >> 2) * (P->sw >> 2) + (u32)(x >> 2);
        u32 tv = tex8(P->base + blk * 4 + ((u32)y & 3)) >> (((u32)x & 3) * 2) & 3;
        u32 slot = (P->base >> 17) & 3;
        u32 pi = tex16(0x20000 + ((slot & 2) ? 0x10000 : 0) + ((P->base & 0x1FFFF) >> 1) + blk * 2);
        u32 pa = P->pbase + (pi & 0x3FFF) * 4, mode = pi >> 14;
        u32 c0 = pal16(pa), c1 = pal16(pa + 2), c;
        if (mode == 1 || mode == 3) {   /* two colours and what lies between them */
            if (tv == 0) c = c0;
            else if (tv == 1) c = c1;
            else if (mode == 1) { if (tv == 3) { o->a = 0; c = 0; } else c = mix15(c0, c1, 1, 1, 2); }
            else c = tv == 2 ? mix15(c0, c1, 5, 3, 8) : mix15(c0, c1, 3, 5, 8);
        } else {
            c = pal16(pa + tv * 2);
            if (mode == 0 && tv == 3) { o->a = 0; c = 0; }
        }
        texel_rgb(c, o);
        break;
    }
    case 6:                             /* A5I3 */
        v = tex8(P->base + idx);
        texel_rgb(pal16(P->pbase + (v & 7) * 2), o);
        o->a = v >> 3;
        break;
    default:                            /* 7: direct colour, bit 15 is alpha */
        v = tex16(P->base + idx * 2);
        texel_rgb(v, o);
        o->a = (v & 0x8000) ? 31 : 0;
        break;
    }
}

/* ---- the pixel ----------------------------------------------------------------------------- */
/* The polygon's colour at a pixel from the vertex colour and the texel by polygon mode. GBATEK
 * writes the blend formulas for 6-bit colour and alpha, 5-bit values expanded as X * 2 + 1 (zero
 * stays zero), so alpha is expanded here too, and the result is cut back to 5 bits. That makes
 * decal's "Rt * At + Rv * (63 - At)" exact at both ends of At (the exceptions the text lists).
 * Returns the alpha (5 bits); 0 means the pixel is not drawn. Untextured, a polygon's colour is
 * its vertex colour; in toon and highlight mode that is the toon table colour (a guess for
 * highlight, where the formula with a white texture would double it). */
static u32 shade(const Poly *P, u32 *r, u32 *g, u32 *b, s32 s, s32 t) {
    u32 av = P->wire ? 31 : P->alpha;
    if (P->mode == 2) {                 /* the toon table, indexed by the red of the vertex colour (6 bits down to 5) */
        u32 c = P->toon[*r >> 1];
        *r = c & 63; *g = (c >> 6) & 63; *b = (c >> 12) & 63;
    }
    if (!P->tex) return av;
    Texel T;
    fetch(P, s, t, &T);
    if (!T.a && P->mode != 1) return 0;           /* decal ignores the texel's alpha but for the mix: a clear texel shows the vertex colour */
    if (P->mode == 1) {
        u32 a6 = x6(T.a);
        if (T.a == 31) { *r = T.r; *g = T.g; *b = T.b; }
        else if (T.a) {
            *r = (T.r * a6 + *r * (63 - a6)) / 64;
            *g = (T.g * a6 + *g * (63 - a6)) / 64;
            *b = (T.b * a6 + *b * (63 - a6)) / 64;
        }
        return av;
    }
    u32 sr = *r, sg = *g, sb = *b;
    *r = ((T.r + 1) * (sr + 1) - 1) / 64; *g = ((T.g + 1) * (sg + 1) - 1) / 64; *b = ((T.b + 1) * (sb + 1) - 1) / 64;
    if (P->mode == 2 && (P->disp & 2)) {        /* highlight: add the shading colour back */
        *r += sr; *g += sg; *b += sb;
        if (*r > 63) *r = 63;
        if (*g > 63) *g = 63;
        if (*b > 63) *b = 63;
    }
    return (((x6(T.a) + 1) * (x6(av) + 1) - 1) / 64) >> 1;
}

static int depth_pass(const Poly *P, u32 d, u32 old) {
    if (P->attr & (1u << 14)) return d + 0x200 >= old && d <= old + 0x200;   /* "equal", with GBATEK's tolerance */
    return d < old;
}

/* A translucent pixel blends with what is there, unless that is a translucent pixel of the same
 * polygon ID (overlapping translucent polygons with one ID blend once; GBATEK states this for
 * shadow volumes, where it picks whether overlaps shade once or twice). The fog flag is the old
 * one ANDed with the polygon's. */
static void translucent(const Poly *P, u32 i, u32 r, u32 g, u32 b, u32 a, u32 depth) {
    Gpu3d *G = P->g;
    u32 pa = G->pattr[i];
    if ((pa & A_TRANS) && ((pa >> 6) & 63) == P->id) return;
    u32 old = G->color[i], oa = (old >> 18) & 31;
    if ((P->disp & 8) && oa) {          /* blending on, and not over a fully transparent rear plane */
        r = (r * (a + 1) + (old & 63) * (31 - a)) >> 5;
        g = (g * (a + 1) + ((old >> 6) & 63) * (31 - a)) >> 5;
        b = (b * (a + 1) + ((old >> 12) & 63) * (31 - a)) >> 5;
        if (oa > a) a = oa;
    }
    G->color[i] = PIX(r, g, b, a);
    if (P->attr & (1u << 11)) G->depth[i] = depth;
    pa = (pa & (63 | A_EDGE | A_STENCIL)) | (P->id << 6) | A_TRANS;
    if ((G->pattr[i] & A_FOG) && (P->attr & (1u << 15))) pa |= A_FOG;
    G->pattr[i] = pa;
}

/* cov: 1..31 how much of an edge pixel the polygon covers (31 inside), right: it is a right edge's;
 * edge: the pixel is on the outline */
static void fragment(const Poly *P, int x, int y, u32 depth, u32 r, u32 g, u32 b, s32 s, s32 t, u32 cov, int right, int edge) {
    Gpu3d *G = P->g;
    u32 i = (u32)y * 256 + (u32)x;
    if (!depth_pass(P, depth, G->depth[i])) return;
    if (P->mask) { G->pattr[i] |= A_STENCIL; return; }
    u32 a = shade(P, &r, &g, &b, s, t);
    if ((P->disp & 4) ? a <= (u32)(G->regs[0x40] & 31) : !a) return;     /* alpha test; zero alpha is never drawn */
    if (P->mode == 3 && P->id) {        /* a shadow where no mask covers it and the ID differs from what is there */
        if (G->pattr[i] & A_STENCIL) { G->pattr[i] &= ~(u32)A_STENCIL; return; }
        if ((G->pattr[i] & 63) == P->id) return;
    }
    if (a < 31) { translucent(P, i, r, g, b, a, depth); return; }
    G->color[i] = PIX(r, g, b, 31);
    G->depth[i] = depth;
    u32 pa = (G->pattr[i] & A_STENCIL) | P->id;
    if (P->attr & (1u << 15)) pa |= A_FOG;
    if (edge && P->edge) pa |= A_EDGE;
    if (cov < 31) pa |= cov << A_COV | (right ? A_RIGHT : 0);
    G->pattr[i] = pa;
}

/* thin: the polygon has no area or is no bigger than a pixel, so its run in the row is drawn
 * whole, and at least a pixel */
static void span(const Poly *P, int y, const Cross *l, const Cross *r, int edge_row, int thin) {
    s32 start = (s32)((l->x + 0x7FFF) >> 16), end = (s32)((r->x + 0x7FFF) >> 16);
    s32 llo = (s32)(l->lo >> 16), lhi = (s32)((l->hi + 0xFFFF) >> 16);
    s32 rlo = (s32)(r->lo >> 16), rhi = (s32)((r->hi + 0xFFFF) >> 16);
    if (P->wire || thin || P->full) {   /* the outline covers each edge's whole run */
        if (llo < start) start = llo;
        if (rhi > end) end = rhi;
        if (end <= start) end = start + 1;
    }
    s64 den = r->x - l->x;
    if (den <= 0) den = 1;
    s64 dz = (s64)r->z - l->z;
    int x0 = start < 0 ? 0 : start, x1 = end > 256 ? 256 : end;
    for (int x = x0; x < x1; x++) {
        int on_edge = edge_row || x == start || x == end - 1 || (x >= llo && x < lhi) || (x >= rlo && x < rhi);
        if (P->wire && !on_edge) continue;
        s64 num = ((s64)x << 16) + 0x8000 - l->x;
        num = num < 0 ? 0 : num > den ? den : num;
        s64 pd = (den - num) * r->wn + num * l->wn;
        s64 p = 0;
        u32 depth;
        if (P->wbuf) {
            p = pd ? ((num * l->wn) << 16) / pd : 0;
            s64 w = l->w + (((r->w - l->w) * p) >> 16); depth = (u32)(w < 0 ? 0 : w > 0xFFFFFF ? 0xFFFFFF : w);
        } else {
            depth = (u32)(l->z + (dz ? (s32)(dz * num / den) : 0));
        }
        /* a pixel behind what is drawn costs nothing more: the perspective factor and the attributes are
         * worked out only for the pixels that pass (fragment tests again, with the same answer) */
        if (!depth_pass(P, depth, P->g->depth[(u32)y * 256 + (u32)x])) continue;
        if (!P->wbuf) p = pd ? ((num * l->wn) << 16) / pd : 0;
        /* anti-aliasing: the side pixels of a polygon's row are partly covered, by how far the
         * edge's crossing is from the pixel's outer side (the crossing at a row's centre only:
         * no coverage along the top and bottom edges) */
        u32 cov = 31;
        int right = 0;
        if (P->aa && !thin && !P->wire) {
            s64 c = 31;
            if (x == start) { c = ((s64)(start + 1) << 16) - l->x; c = c < 0 ? 0 : c >> 11; }
            if (x == end - 1) {
                s64 c2 = r->x - ((s64)(end - 1) << 16);
                c2 = c2 < 0 ? 0 : c2 >> 11;
                if (c2 < c || x != start) { c = c2; right = x != start; }
            }
            cov = c > 31 ? 31 : c < 1 ? 1 : (u32)c;
        }
        fragment(P, x, y, depth,
                 (u32)(l->r + (s32)((((s64)r->r - l->r) * p) >> 16)) >> 8,
                 (u32)(l->g + (s32)((((s64)r->g - l->g) * p) >> 16)) >> 8,
                 (u32)(l->b + (s32)((((s64)r->b - l->b) * p) >> 16)) >> 8,
                 l->s + (s32)((((s64)r->s - l->s) * p) >> 16), l->t + (s32)((((s64)r->t - l->t) * p) >> 16),
                 cov, right, on_edge);
    }
}

static s64 edge_x(const G3dVertex *a, const G3dVertex *b, s64 y) {
    return (s64)a->x * 65536 + ((s64)(b->x - a->x) * 65536 * (y - a->y)) / (b->y - a->y);
}

/* edge a-b (a above b) at the centre of row y */
static void cross(const G3dVertex *a, const G3dVertex *b, s32 wa, s32 wb, int y, Cross *c) {
    s64 num = 2 * (s64)(y - a->y) + 1, den = 2 * (s64)(b->y - a->y);
    c->x = (s64)a->x * 65536 + ((s64)(b->x - a->x) * 65536 * num) / den;
    s64 xt = edge_x(a, b, y), xb = edge_x(a, b, y + 1);
    c->lo = xt < xb ? xt : xb; c->hi = xt < xb ? xb : xt;
    s64 pd = (den - num) * wb + num * wa;
    s64 p = pd ? ((num * wa) * 65536) / pd : 0;
    c->z = a->z + (s32)(((s64)b->z - a->z) * num / den);
    c->w = a->w + ((((s64)b->w - a->w) * p) >> 16);
    c->wn = wa + (s32)((((s64)wb - wa) * p) >> 16);
    c->r = a->r * 256 + (s32)((((s64)(b->r - a->r) * 256) * p) >> 16);
    c->g = a->g * 256 + (s32)((((s64)(b->g - a->g) * 256) * p) >> 16);
    c->b = a->b * 256 + (s32)((((s64)(b->b - a->b) * 256) * p) >> 16);
    c->s = a->s * 256 + (s32)((((s64)(b->s - a->s) * 256) * p) >> 16);
    c->t = a->t * 256 + (s32)((((s64)(b->t - a->t) * 256) * p) >> 16);
}

static void at_vertex(const G3dVertex *v, s32 wn, Cross *c) {
    c->x = c->lo = c->hi = (s64)v->x * 65536;
    c->w = v->w; c->z = v->z; c->wn = wn;
    c->r = v->r * 256; c->g = v->g * 256; c->b = v->b * 256;
    c->s = v->s * 256; c->t = v->t * 256;
}

/* rows ylo..yhi-1 only: the screen bands are drawn independently (gpu3d_render) */
static void draw_polygon(const Poly *P, const G3dList *L, const G3dPoly *poly, int ylo, int yhi) {
    const G3dVertex *v[G3D_POLY_VERTS];
    s32 wn[G3D_POLY_VERTS], wmax = 1;
    int n = poly->n, xmin = 1 << 20, xmax = -(1 << 20), ymin = xmin, ymax = xmax;
    for (int i = 0; i < n; i++) {
        v[i] = &L->vtx[poly->v[i]];
        if (v[i]->w > wmax) wmax = v[i]->w;
        if (v[i]->x < xmin) xmin = v[i]->x;
        if (v[i]->x > xmax) xmax = v[i]->x;
        if (v[i]->y < ymin) ymin = v[i]->y;
        if (v[i]->y > ymax) ymax = v[i]->y;
    }
    s64 area = 0;                       /* twice the screen area: zero for a line or a dot */
    for (int i = 0; i < n; i++) {
        const G3dVertex *a = v[i], *b = v[(i + 1) % n];
        area += (s64)a->x * b->y - (s64)b->x * a->y;
    }
    /* GBATEK: width and height are rounded up to 1, so 0x0, 1x0, 0x1 and 1x1 polygons are all one pixel */
    int thin = area == 0 || (xmax - xmin <= 1 && ymax - ymin <= 1);
    int sh = 0;
    while ((wmax >> sh) > 0xFFFF) sh++;
    for (int i = 0; i < n; i++) { wn[i] = v[i]->w >> sh; if (wn[i] < 1) wn[i] = 1; }
    if (ymin == ymax) {                 /* flat: one row, at least one pixel */
        if (ymin < ylo || ymin >= yhi) return;
        int lo = 0, hi = 0;
        for (int i = 1; i < n; i++) { if (v[i]->x < v[lo]->x) lo = i; if (v[i]->x > v[hi]->x) hi = i; }
        Cross l, r;
        at_vertex(v[lo], wn[lo], &l); at_vertex(v[hi], wn[hi], &r);
        span(P, ymin, &l, &r, 1, thin);
        return;
    }
    int y0 = ymin < ylo ? ylo : ymin, y1 = ymax > yhi ? yhi : ymax;
    for (int y = y0; y < y1; y++) {
        Cross l, r, c;
        int k = 0;
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n;
            const G3dVertex *a = v[i], *b = v[j];
            s32 wa = wn[i], wb = wn[j];
            if (a->y == b->y) continue;
            if (a->y > b->y) { const G3dVertex *tv = a; a = b; b = tv; s32 tw = wa; wa = wb; wb = tw; }
            if (y < a->y || y >= b->y) continue;
            cross(a, b, wa, wb, y, &c);
            if (!k++) l = r = c;
            else if (c.x < l.x) l = c;
            else if (c.x > r.x) r = c;
        }
        if (k) span(P, y, &l, &r, y == ymin || y == ymax - 1, thin);
    }
}

/* ---- the frame ------------------------------------------------------------------------------ */
/* The rear plane: CLEAR_COLOR and CLEAR_DEPTH for every pixel, or (DISP3DCNT.14) a colour bitmap
 * in texture slot 2 and a depth bitmap in slot 3, 256x256 and scrolled by CLRIMAGE_OFFSET with
 * wrap-around; the clear polygon ID stays in CLEAR_COLOR. The Y offset is taken as 8 bits
 * (GBATEK says bits 8-14: a guess that costs nothing for offsets below 128). */
static void rear_plane(Gpu3d *g, u32 y0, u32 y1) {
    u32 cc = rd32r(g->regs + 0x50);
    u32 id = (cc >> 24) & 63;
    if (g->disp3dcnt & 0x4000) {
        u32 off = rd16r(g->regs + 0x56), xo = off & 255, yo = (off >> 8) & 255;
        for (u32 y = y0; y < y1; y++)
            for (u32 x = 0; x < 256; x++) {
                u32 a = (((y + yo) & 255) * 256 + ((x + xo) & 255)) * 2;
                u32 c = tex16(0x40000 + a), d = tex16(0x60000 + a);
                u32 i = y * 256 + x;
                g->color[i] = rgb6((u16)c) | ((c & 0x8000) ? 31u << 18 : 0);
                g->depth[i] = depth24(d);
                g->pattr[i] = id | ((d & 0x8000) ? A_FOG : 0);
            }
        return;
    }
    u32 c = rgb6((u16)cc) | ((cc >> 16) & 31) << 18, d = depth24(rd16r(g->regs + 0x54));
    u32 pa = id | ((cc & 0x8000) ? A_FOG : 0);
    for (u32 i = y0 * 256; i < y1 * 256; i++) { g->color[i] = c; g->depth[i] = d; g->pattr[i] = pa; }
}

/* Edge marking, once the polygons are in: a pixel flagged as part of an opaque polygon's outline
 * takes the edge colour of its ID when a pixel above, below, left or right has another ID and a
 * greater depth; beyond the screen the neighbour is the rear plane (its ID, depth and colour). */
static void edge_marking(Gpu3d *g, int aa) {
    u32 cc = rd32r(g->regs + 0x50), rid = (cc >> 24) & 63, rdepth = depth24(rd16r(g->regs + 0x54));
    u32 ecol[8];
    for (int k = 0; k < 8; k++) ecol[k] = rgb6(rd16r(g->regs + 0x30 + 2 * k));
    for (int y = 0; y < 192; y++)
        for (int x = 0; x < 256; x++) {
            u32 i = (u32)y * 256 + (u32)x, pa = g->pattr[i];
            if (!(pa & A_EDGE)) continue;
            u32 id = pa & 63, d = g->depth[i];
            int mark = 0;
            for (int k = 0; k < 4 && !mark; k++) {
                int nx = x + (k == 0) - (k == 1), ny = y + (k == 2) - (k == 3);
                u32 nid, nd;
                if (nx < 0 || nx > 255 || ny < 0 || ny > 191) { nid = rid; nd = rdepth; }
                else { u32 j = (u32)ny * 256 + (u32)nx; nid = g->pattr[j] & 63; nd = g->depth[j]; }
                mark = nid != id && d < nd;
            }
            if (!mark) continue;
            /* with anti-aliasing the edge colour comes out translucent: GBATEK says "alpha=16 or so" */
            g->color[i] = ecol[id >> 3] | (aa ? 16u : 31u) << 18;
            g->pattr[i] &= ~(31u << A_COV);
        }
}

/* Fog: FogDepthBoundary[n] = FOG_OFFSET + (0x400 >> shift) * (n + 1), in the 15 bits above the
 * 9 low ones of the 24-bit depth (the clear depth's format). GBATEK words "DensityBoundary[31]
 * for further than boundary[0]" as boundary[0] twice; the table runs to boundary[31]. */
static void fog(Gpu3d *g) {
    u32 fc = rd32r(g->regs + 0x58), off = rd16r(g->regs + 0x5C) & 0x7FFF, step = 0x400 >> ((g->disp3dcnt >> 8) & 15);
    int alpha_only = (g->disp3dcnt >> 6) & 1;
    u32 col = rgb6((u16)fc), fa = (fc >> 16) & 31;
    for (u32 i = 0; i < 192 * 256; i++) {
        if (!(g->pattr[i] & A_FOG)) continue;
        u32 d = g->depth[i] >> 9, dens;
        u32 first = off + step;
        if (d < first) dens = g->regs[0x60] & 127;
        else if (d >= off + step * 32 || !step) dens = g->regs[0x7F] & 127;
        else {
            u32 n = (d - off) / step - 1, f = (d - off) % step;
            u32 d0 = g->regs[0x60 + n] & 127, d1 = g->regs[0x61 + n] & 127;
            dens = (d0 * (step - f) + d1 * f) / step;
        }
        if (dens >= 127) dens = 128;
        u32 c = g->color[i], a = (c >> 18) & 31;
        u32 ua = d < first ? 31 : fa;   /* the fog glitch: the alpha before the first boundary reads as 31 */
        u32 na = (ua * dens + a * (128 - dens)) / 128;
        if (!alpha_only) {
            u32 r = ((col & 63) * dens + (c & 63) * (128 - dens)) / 128;
            u32 gr = (((col >> 6) & 63) * dens + ((c >> 6) & 63) * (128 - dens)) / 128;
            u32 b = (((col >> 12) & 63) * dens + ((c >> 12) & 63) * (128 - dens)) / 128;
            g->color[i] = PIX(r, gr, b, na);
        } else g->color[i] = (c & 0x3FFFF) | na << 18;
    }
}

/* Anti-aliasing, the approximation: an opaque polygon's side pixels carry a coverage; each is
 * mixed with the pixel just outside it (a neighbouring polygon of the same mesh, or whatever is
 * behind) in proportion, and if nothing is there (a transparent rear plane) becomes translucent
 * by the coverage. Top and bottom edges and edges of other slopes get no smoothing beyond that. */
static void anti_alias(Gpu3d *g) {
    u32 row[256];
    for (int y = 0; y < 192; y++) {
        memcpy(row, g->color + y * 256, sizeof row);
        for (int x = 0; x < 256; x++) {
            u32 i = (u32)y * 256 + (u32)x, pa = g->pattr[i], cov = (pa >> A_COV) & 31;
            if (!cov) continue;
            int nx = (pa & A_RIGHT) ? x + 1 : x - 1;
            if (nx < 0 || nx > 255) continue;
            u32 c = row[x], n = row[nx], na = (n >> 18) & 31;
            if (!na) { g->color[i] = (c & 0x3FFFF) | (((c >> 18) & 31) < cov ? ((c >> 18) & 31) : cov) << 18; continue; }
            u32 r = ((c & 63) * cov + (n & 63) * (31 - cov)) / 31;
            u32 gr = (((c >> 6) & 63) * cov + ((n >> 6) & 63) * (31 - cov)) / 31;
            u32 b = (((c >> 12) & 63) * cov + ((n >> 12) & 63) * (31 - cov)) / 31;
            g->color[i] = PIX(r, gr, b, (c >> 18) & 31);
        }
    }
}

static int cmp_u64(const void *a, const void *b) { u64 x = *(const u64 *)a, y = *(const u64 *)b; return (x > y) - (x < y); }

static void setup(Poly *c, const G3dPoly *P, int wbuf) {
    u32 attr = P->attr, alpha = (attr >> 16) & 31;
    c->attr = attr;
    c->alpha = alpha; c->id = (attr >> 24) & 63;
    c->wire = !alpha;
    c->wbuf = wbuf;
    c->mode = (int)((attr >> 4) & 3);
    c->mask = c->mode == 3 && c->id == 0;
    int opaque = alpha == 31 || c->wire, aa = (c->disp >> 4) & 1, edge = (c->disp >> 5) & 1;
    /* GBATEK "Polygon Size": opaque polygons are full size with edge marking or anti-aliasing,
     * translucent ones with alpha blending */
    c->full = opaque ? (aa || edge) : (int)((c->disp >> 3) & 1);
    c->aa = aa && alpha == 31;
    c->edge = edge && opaque;
    u32 tex = P->tex, fmt = (tex >> 26) & 7;
    c->tex = (c->disp & 1) ? (int)fmt : 0;
    c->rep_s = (tex >> 16) & 1; c->rep_t = (tex >> 17) & 1;
    c->flip_s = (tex >> 18) & 1; c->flip_t = (tex >> 19) & 1;
    c->sw = 8u << ((tex >> 20) & 7); c->sh = 8u << ((tex >> 23) & 7);
    c->trans0 = (tex >> 29) & 1;
    c->base = (tex & 0xFFFF) * 8;
    c->pbase = P->pltt * (fmt == 2 ? 8 : 16);
}

/* Whether a polygon is drawn with the translucent ones, after every opaque polygon. The ones with
 * POLYGON_ATTR alpha 1..30 are (GBATEK: "translucent polygons are always rendered last, even with
 * auto-sort disabled"); so are those whose texture is a "Translucent Texture" (A3I5, A5I3), when
 * the texel's alpha reaches the pixel (everything but decal and shadow masks): GBATEK says
 * "Alpha-Blending occurs for pixels of translucent polygons" and that a wire frame (Av 31) "can
 * be made transparent by using Translucent Textures with At < 31", so such a pixel is a
 * translucent polygon's, and drawn first it would be overwritten by anything behind it that is
 * drawn later, since a translucent pixel does not write depth (bit 11). GBATEK states no rule for
 * which polygons are sorted as translucent, this is an inference. Only the pass changes: size,
 * anti-aliasing and edge flags (setup) follow POLYGON_ATTR's alpha, and a texel of alpha 31 is
 * still an opaque pixel (fragment). */
static int late_pass(const G3dPoly *P, u32 disp) {
    u32 alpha = (P->attr >> 16) & 31, mode = (P->attr >> 4) & 3, fmt = (P->tex >> 26) & 7;
    if (alpha && alpha < 31) return 1;
    if (!(disp & 1) || (fmt != 1 && fmt != 6)) return 0;
    return mode != 1 && !(mode == 3 && !((P->attr >> 24) & 63));      /* decal: A = Av; a mask draws no colour */
}

/* What a band job needs: the polygons in drawing order (the first nop are the opaque ones, in the order
 * sent; the rest are keyed, the polygon in the low 20 bits). */
typedef struct {
    Gpu3d *g;
    const G3dList *L;
    const u32 *toon;
    const u64 *order;
    u32 nop;
    int wbuf;
} Frame;

/* Rows ylo..yhi-1: the rear plane, then every polygon in order, clipped to the rows. A pixel depends
 * only on the polygons that cover it, in the same order whatever the rows around it, so any split of the
 * screen into bands gives the same frame. */
static void draw_rows(const Frame *F, int ylo, int yhi) {
    Gpu3d *g = F->g;
    const G3dList *L = F->L;
    rear_plane(g, (u32)ylo, (u32)yhi);
    for (u32 k = 0; k < L->npoly; k++) {
        const G3dPoly *P = &L->poly[k < F->nop ? F->order[k] : F->order[k] & 0xFFFFF];
        if (P->ymax < ylo || P->ymin >= yhi) continue;      /* no row of the band (a flat polygon on its first row is ymin == ymax) */
        Poly c;
        memset(&c, 0, sizeof c);
        c.g = g; c.toon = F->toon; c.disp = g->disp3dcnt;
        setup(&c, P, F->wbuf);
        draw_polygon(&c, L, P, ylo, yhi);
    }
}

#define BAND_ROWS 16            /* 12 bands: more than threads, so a busy band does not hold the others up */
#define PARALLEL_POLYS 48       /* fewer polygons than this are drawn in the caller: waking threads would cost more */

static void draw_band(void *arg, int band) { draw_rows(arg, band * BAND_ROWS, (band + 1) * BAND_ROWS); }

void gpu3d_render(Nds *s) {
    Gpu3d *g = &s->g3d;
    G3dList *L = &g->lists[g->cur ^ 1];
    u32 toon[32];
    for (int i = 0; i < 32; i++) toon[i] = rgb6(rd16r(g->regs + 0x80 + 2 * i));
    /* opaque polygons first, in the order they were sent; then the translucent ones, by bottom
     * then top Y (ties in order sent) unless the swap asked for manual order. GBATEK gives no
     * rule for the sort, this is a guess. */
    u64 order[G3D_MAX_POLYS];
    u32 nop = 0, nt = 0;
    for (u32 i = 0; i < L->npoly; i++) {
        G3dPoly *P = &L->poly[i];
        P->translucent = late_pass(P, g->disp3dcnt);
        if (!P->translucent) order[nop++] = i;
    }
    for (u32 i = 0; i < L->npoly; i++) {
        const G3dPoly *P = &L->poly[i];
        if (!P->translucent) continue;
        u64 key = (u64)(P->ymax + 512) << 40 | (u64)(P->ymin + 512) << 20 | i;
        order[nop + nt++] = (L->swap & 1) ? i : key;
    }
    if (!(L->swap & 1)) qsort(order + nop, nt, sizeof *order, cmp_u64);
    Frame F = {g, L, toon, order, nop, (L->swap >> 1) & 1};
    if (L->npoly >= PARALLEL_POLYS) workers_run(192 / BAND_ROWS, draw_band, &F);
    else draw_rows(&F, 0, 192);
    if (g->disp3dcnt & 0x20) edge_marking(g, (g->disp3dcnt >> 4) & 1);
    if (g->disp3dcnt & 0x80) fog(g);
    if (g->disp3dcnt & 0x10) anti_alias(g);
}

void gpu3d_bg0_line(Nds *s, int line, u16 *px, u8 *alpha) {
    const u32 *c = s->g3d.color + line * 256;
    for (int x = 0; x < 256; x++) {
        u32 v = c[x], a = (v >> 18) & 31;
        alpha[x] = (u8)a;
        px[x] = a ? (u16)(((v >> 1) & 31) | ((v >> 7) & 31) << 5 | ((v >> 13) & 31) << 10 | 0x8000) : 0;
    }
}
