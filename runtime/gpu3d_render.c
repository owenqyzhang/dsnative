/* dsnative runtime: the 3D rasterizer, first cut. Draws the polygon list SWAP_BUFFERS handed
 * over into a 256x192 colour + alpha frame with a depth buffer (Z or W): the rear plane, then
 * the opaque polygons in the order they were sent, then the translucent ones (sorted by Y
 * unless the swap before the list asked for manual order), each scanline by scanline.
 * Reference: GBATEK, DS 3D Video.
 *
 * Vertices sit on whole pixels, as on the DS. A pixel is drawn when its centre is inside the
 * polygon, so the right and bottom edges are left out and polygons that share an edge never
 * both draw its pixels. A polygon with no area on screen (a line or a dot, which a game makes
 * with repeated vertices) is the exception: it draws every pixel along its run, at least one
 * per row; a polygon no bigger than 1x1 is one pixel (GBATEK rounds width and height up to 1).
 * Colour and W are interpolated perspective-correctly (w scaled to 16 bits), Z linearly in
 * screen space.
 *
 * Untextured: every polygon takes its colour from its vertices, whatever its mode or texture.
 * Not done: textures, toon/highlight shading, shadow polygons (not drawn), fog, edge marking,
 * anti-aliasing, the rear-plane bitmap. The first draft of those is in commit 1d916a5. */
#include <stdlib.h>
#include "nds.h"

/* attribute buffer: bits 6-11 hold the polygon ID of a translucent pixel, A_TRANS says one is there */
enum { A_TRANS = 1 << 12 };

#define PIX(r, g, b, a) ((u32)(r) | (u32)(g) << 6 | (u32)(b) << 12 | (u32)(a) << 18)

static inline u32 x6(u32 c) { return c ? c * 2 + 1 : 0; }      /* 5-bit to 6-bit */
static inline u16 rd16r(const u8 *p) { u16 v; memcpy(&v, p, 2); return v; }
static inline u32 rd32r(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }
static inline u32 rgb6(u16 c) { return PIX(x6(c & 31), x6((c >> 5) & 31), x6((c >> 10) & 31), 0); }
/* the 15-bit clear depth as 24 bits: 0x7FFF is the farthest, 0xFFFFFF */
static inline u32 depth24(u32 d) { d &= 0x7FFF; return d * 0x200 + ((d + 1) / 0x8000) * 0x1FF; }

typedef struct {
    Gpu3d *g;
    u32 attr, disp;                     /* POLYGON_ATTR, DISP3DCNT */
    u32 alpha, id;
    int wire, wbuf;
} Poly;

/* where a polygon edge crosses a scanline's centre */
typedef struct {
    s64 x, lo, hi;                      /* 16.16: the crossing, and the edge's run within the row */
    s64 w;                              /* clip w */
    s32 z, wn;                          /* Z depth, w scaled to 16 bits */
    s32 r, g, b;                        /* 6-bit colour << 8 */
} Cross;

static int depth_pass(const Poly *P, u32 d, u32 old) {
    if (P->attr & (1u << 14)) return d + 0x200 >= old && d <= old + 0x200;   /* "equal", with GBATEK's tolerance */
    return d < old;
}

/* A translucent pixel blends with what is there, unless that is a translucent pixel of the same
 * polygon ID (overlapping translucent polygons with one ID blend once; GBATEK states this for
 * shadow volumes, where it picks whether overlaps shade once or twice). */
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
    G->pattr[i] = P->id << 6 | A_TRANS;
}

static void fragment(const Poly *P, int x, int y, u32 depth, u32 r, u32 g, u32 b) {
    Gpu3d *G = P->g;
    u32 i = (u32)y * 256 + (u32)x;
    u32 a = P->wire ? 31 : P->alpha;    /* a wire frame's lines are opaque */
    if ((P->disp & 4) ? a <= (u32)(G->regs[0x40] & 31) : !a) return;     /* alpha test */
    if (!depth_pass(P, depth, G->depth[i])) return;
    if (a < 31) { translucent(P, i, r, g, b, a, depth); return; }
    G->color[i] = PIX(r, g, b, 31);
    G->depth[i] = depth;
    G->pattr[i] = P->id;
}

/* thin: the polygon has no area or is no bigger than a pixel, so its run in the row is drawn
 * whole, and at least a pixel */
static void span(const Poly *P, int y, const Cross *l, const Cross *r, int edge_row, int thin) {
    s32 start = (s32)((l->x + 0x7FFF) >> 16), end = (s32)((r->x + 0x7FFF) >> 16);
    s32 llo = (s32)(l->lo >> 16), lhi = (s32)((l->hi + 0xFFFF) >> 16);
    s32 rlo = (s32)(r->lo >> 16), rhi = (s32)((r->hi + 0xFFFF) >> 16);
    if (P->wire || thin) {              /* the outline covers each edge's whole run */
        if (llo < start) start = llo;
        if (rhi > end) end = rhi;
        if (end <= start) end = start + 1;
    }
    s64 den = r->x - l->x;
    if (den <= 0) den = 1;
    int x0 = start < 0 ? 0 : start, x1 = end > 256 ? 256 : end;
    for (int x = x0; x < x1; x++) {
        if (P->wire && !(edge_row || x == start || x == end - 1 || (x >= llo && x < lhi) || (x >= rlo && x < rhi))) continue;
        s64 num = ((s64)x << 16) + 0x8000 - l->x;
        num = num < 0 ? 0 : num > den ? den : num;
        s64 pd = (den - num) * r->wn + num * l->wn;
        s64 p = pd ? ((num * l->wn) << 16) / pd : 0;
        u32 depth;
        if (P->wbuf) { s64 w = l->w + (((r->w - l->w) * p) >> 16); depth = (u32)(w < 0 ? 0 : w > 0xFFFFFF ? 0xFFFFFF : w); }
        else depth = (u32)(l->z + (s32)(((s64)r->z - l->z) * num / den));
        fragment(P, x, y, depth,
                 (u32)(l->r + (s32)((((s64)r->r - l->r) * p) >> 16)) >> 8,
                 (u32)(l->g + (s32)((((s64)r->g - l->g) * p) >> 16)) >> 8,
                 (u32)(l->b + (s32)((((s64)r->b - l->b) * p) >> 16)) >> 8);
    }
}

static s64 edge_x(const G3dVertex *a, const G3dVertex *b, s64 y) {
    return ((s64)a->x << 16) + (((s64)(b->x - a->x) << 16) * (y - a->y)) / (b->y - a->y);
}

/* edge a-b (a above b) at the centre of row y */
static void cross(const G3dVertex *a, const G3dVertex *b, s32 wa, s32 wb, int y, Cross *c) {
    s64 num = 2 * (s64)(y - a->y) + 1, den = 2 * (s64)(b->y - a->y);
    c->x = ((s64)a->x << 16) + (((s64)(b->x - a->x) << 16) * num) / den;
    s64 xt = edge_x(a, b, y), xb = edge_x(a, b, y + 1);
    c->lo = xt < xb ? xt : xb; c->hi = xt < xb ? xb : xt;
    s64 pd = (den - num) * wb + num * wa;
    s64 p = pd ? ((num * wa) << 16) / pd : 0;
    c->z = a->z + (s32)(((s64)b->z - a->z) * num / den);
    c->w = a->w + ((((s64)b->w - a->w) * p) >> 16);
    c->wn = wa + (s32)((((s64)wb - wa) * p) >> 16);
    c->r = (a->r << 8) + (s32)((((s64)(b->r - a->r) << 8) * p) >> 16);
    c->g = (a->g << 8) + (s32)((((s64)(b->g - a->g) << 8) * p) >> 16);
    c->b = (a->b << 8) + (s32)((((s64)(b->b - a->b) << 8) * p) >> 16);
}

static void at_vertex(const G3dVertex *v, s32 wn, Cross *c) {
    c->x = c->lo = c->hi = (s64)v->x << 16;
    c->w = v->w; c->z = v->z; c->wn = wn;
    c->r = v->r << 8; c->g = v->g << 8; c->b = v->b << 8;
}

static void draw_polygon(const Poly *P, const G3dList *L, const G3dPoly *poly) {
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
        if (ymin < 0 || ymin >= 192) return;
        int lo = 0, hi = 0;
        for (int i = 1; i < n; i++) { if (v[i]->x < v[lo]->x) lo = i; if (v[i]->x > v[hi]->x) hi = i; }
        Cross l, r;
        at_vertex(v[lo], wn[lo], &l); at_vertex(v[hi], wn[hi], &r);
        span(P, ymin, &l, &r, 1, thin);
        return;
    }
    int y0 = ymin < 0 ? 0 : ymin, y1 = ymax > 192 ? 192 : ymax;
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
static void rear_plane(Gpu3d *g) {
    u32 cc = rd32r(g->regs + 0x50);
    u32 c = rgb6((u16)cc) | ((cc >> 16) & 31) << 18, d = depth24(rd16r(g->regs + 0x54));
    u32 id = (cc >> 24) & 63;
    for (u32 i = 0; i < 192 * 256; i++) { g->color[i] = c; g->depth[i] = d; g->pattr[i] = id; }
}

static int cmp_u64(const void *a, const void *b) { u64 x = *(const u64 *)a, y = *(const u64 *)b; return (x > y) - (x < y); }

void gpu3d_render(Nds *s) {
    Gpu3d *g = &s->g3d;
    G3dList *L = &g->lists[g->cur ^ 1];
    rear_plane(g);
    /* opaque polygons first, in the order they were sent; then the translucent ones, by bottom
     * then top Y (ties in order sent) unless the swap asked for manual order. GBATEK gives no
     * rule for the sort, this is a guess. */
    u64 order[G3D_MAX_POLYS];
    u32 nop = 0, nt = 0;
    for (u32 i = 0; i < L->npoly; i++) {
        G3dPoly *P = &L->poly[i];
        u32 alpha = (P->attr >> 16) & 31;
        P->translucent = alpha && alpha < 31;
        if (!P->translucent) order[nop++] = i;
    }
    for (u32 i = 0; i < L->npoly; i++) {
        const G3dPoly *P = &L->poly[i];
        if (!P->translucent) continue;
        u64 key = (u64)(P->ymax + 512) << 40 | (u64)(P->ymin + 512) << 20 | i;
        order[nop + nt++] = (L->swap & 1) ? i : key;
    }
    if (!(L->swap & 1)) qsort(order + nop, nt, sizeof *order, cmp_u64);
    for (u32 k = 0; k < L->npoly; k++) {
        const G3dPoly *P = &L->poly[k < nop ? order[k] : order[k] & 0xFFFFF];
        if (((P->attr >> 4) & 3) == 3) continue;        /* shadow volumes: not done */
        Poly c = {g, P->attr, g->disp3dcnt, (P->attr >> 16) & 31, (P->attr >> 24) & 63, 0, (L->swap >> 1) & 1};
        c.wire = !c.alpha;
        draw_polygon(&c, L, P);
    }
}

void gpu3d_bg0_line(Nds *s, int line, u16 *px, u8 *alpha) {
    const u32 *c = s->g3d.color + line * 256;
    for (int x = 0; x < 256; x++) {
        u32 v = c[x], a = (v >> 18) & 31;
        alpha[x] = (u8)a;
        px[x] = a ? (u16)(((v >> 1) & 31) | ((v >> 7) & 31) << 5 | ((v >> 13) & 31) << 10 | 0x8000) : 0;
    }
}
