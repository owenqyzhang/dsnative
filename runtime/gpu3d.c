/* dsnative runtime: the ARM9's geometry engine. Reference: GBATEK, DS 3D Video.
 *
 * Commands run as soon as their parameters are in, with no latency: results (the clip
 * matrix, box and position tests) are ready when software reads them. Each command's cycle
 * count only extends how long GXSTAT reports busy. The FIFO holds entries only while a
 * command waits for parameters and while SWAP_BUFFERS halts the engine until VBlank; then
 * GXSTAT shows its level and GX FIFO DMA pauses when it is half full. The CPU does not
 * stall when it writes to a full FIFO: the queue just takes G3D_FIFO_CAP entries, beyond
 * which commands are lost (counted in Gpu3d.dropped). */
#include "nds.h"

/* parameters + 1 per command (GBATEK "DS 3D I/O Map"); 0: not a command, ignored */
static const u8 np1[128] = {
    [0x10] = 2, [0x11] = 1, [0x12] = 2, [0x13] = 2, [0x14] = 2, [0x15] = 1, [0x16] = 17, [0x17] = 13,
    [0x18] = 17, [0x19] = 13, [0x1A] = 10, [0x1B] = 4, [0x1C] = 4,
    [0x20] = 2, [0x21] = 2, [0x22] = 2, [0x23] = 3, [0x24] = 2, [0x25] = 2, [0x26] = 2, [0x27] = 2,
    [0x28] = 2, [0x29] = 2, [0x2A] = 2, [0x2B] = 2,
    [0x30] = 2, [0x31] = 2, [0x32] = 2, [0x33] = 2, [0x34] = 33,
    [0x40] = 2, [0x41] = 1, [0x50] = 2, [0x60] = 2, [0x70] = 4, [0x71] = 3, [0x72] = 2,
};

/* execution time in bus cycles (33.51 MHz); SWAP_BUFFERS's 392 start at VBlank (vblank()) */
static const u16 cost[128] = {
    [0x10] = 1, [0x11] = 17, [0x12] = 36, [0x13] = 17, [0x14] = 36, [0x15] = 19, [0x16] = 34, [0x17] = 30,
    [0x18] = 35, [0x19] = 31, [0x1A] = 28, [0x1B] = 22, [0x1C] = 22,
    [0x20] = 1, [0x21] = 9, [0x22] = 1, [0x23] = 9, [0x24] = 8, [0x25] = 8, [0x26] = 8, [0x27] = 8,
    [0x28] = 8, [0x29] = 1, [0x2A] = 1, [0x2B] = 1,
    [0x30] = 4, [0x31] = 4, [0x32] = 6, [0x33] = 1, [0x34] = 32,
    [0x40] = 1, [0x41] = 1, [0x50] = 0, [0x60] = 1, [0x70] = 103, [0x71] = 9, [0x72] = 5,
};

static inline s32 sext(u32 v, int bits) { return (s32)(v << (32 - bits)) >> (32 - bits); }

/* ---- matrices ----------------------------------------------------------------------- */
static void mtx_identity(s32 *m) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 0x1000; }

static void mtx_mul(s32 *d, const s32 *a, const s32 *b) {      /* d = a * b */
    s32 r[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            s64 acc = 0;
            for (int k = 0; k < 4; k++) acc += (s64)a[i * 4 + k] * b[k * 4 + j];
            r[i * 4 + j] = (s32)(acc >> 12);
        }
    memcpy(d, r, sizeof r);
}

/* (x y z w) * m, 12-bit fractions */
static void xform(const s32 *m, s32 x, s32 y, s32 z, s32 w, s32 *out) {
    for (int j = 0; j < 4; j++)
        out[j] = (s32)(((s64)x * m[j] + (s64)y * m[4 + j] + (s64)z * m[8 + j] + (s64)w * m[12 + j]) >> 12);
}

/* a direction (12-bit fractions) through the upper-left 3x3 of m */
static void xform3(const s32 *m, const s32 *v, s32 *out) {
    for (int j = 0; j < 3; j++)
        out[j] = (s32)(((s64)v[0] * m[j] + (s64)v[1] * m[4 + j] + (s64)v[2] * m[8 + j]) >> 12);
}

static const s32 *clip_mtx(Gpu3d *g) {
    if (g->clip_dirty) { mtx_mul(g->clip, g->pos, g->proj); g->clip_dirty = 0; }
    return g->clip;
}

static void mtx_load(Gpu3d *g, const s32 *m) {
    switch (g->mtx_mode) {
    case 0: memcpy(g->proj, m, 64); break;
    case 1: memcpy(g->pos, m, 64); break;
    case 2: memcpy(g->pos, m, 64); memcpy(g->dir, m, 64); break;
    case 3: memcpy(g->tex, m, 64); break;
    }
    g->clip_dirty = 1;
}

/* C = M * C; MTX_SCALE leaves the vector matrix alone even in mode 2 (keeps light lengths) */
static void mtx_mult(Gpu3d *g, const s32 *m, int scale) {
    switch (g->mtx_mode) {
    case 0: mtx_mul(g->proj, m, g->proj); break;
    case 1: mtx_mul(g->pos, m, g->pos); break;
    case 2: mtx_mul(g->pos, m, g->pos); if (!scale) mtx_mul(g->dir, m, g->dir); break;
    case 3: mtx_mul(g->tex, m, g->tex); break;
    }
    g->clip_dirty = 1;
}

/* The stacks: projection and texture have one entry (pointer 0..1), position and vector 31
 * (pointer 0..63, entries mirrored every 32). Touching entry 1 or 31 and up sets GXSTAT.15.
 * In mode 1 the stack commands act on both position and vector stacks, as in mode 2. */
static void stack_error(Gpu3d *g) { g->gxstat |= 0x8000; }

static void mtx_push(Gpu3d *g) {
    switch (g->mtx_mode) {
    case 0: if (g->proj_sp & 1) stack_error(g); memcpy(g->proj_stack, g->proj, 64); g->proj_sp = (g->proj_sp + 1) & 1; break;
    case 3: if (g->tex_sp & 1) stack_error(g); memcpy(g->tex_stack, g->tex, 64); g->tex_sp = (g->tex_sp + 1) & 1; break;
    default:
        if (g->pos_sp >= 31) stack_error(g);
        memcpy(g->pos_stack[g->pos_sp & 31], g->pos, 64);
        memcpy(g->dir_stack[g->pos_sp & 31], g->dir, 64);
        g->pos_sp = (g->pos_sp + 1) & 63;
        break;
    }
}

static void mtx_pop(Gpu3d *g, u32 p) {
    switch (g->mtx_mode) {
    case 0: g->proj_sp = (g->proj_sp - 1) & 1; if (g->proj_sp) stack_error(g); memcpy(g->proj, g->proj_stack, 64); break;
    case 3: g->tex_sp = (g->tex_sp - 1) & 1; if (g->tex_sp) stack_error(g); memcpy(g->tex, g->tex_stack, 64); break;
    default:
        g->pos_sp = (g->pos_sp - (u32)sext(p, 6)) & 63;
        if (g->pos_sp >= 31) stack_error(g);
        memcpy(g->pos, g->pos_stack[g->pos_sp & 31], 64);
        memcpy(g->dir, g->dir_stack[g->pos_sp & 31], 64);
        break;
    }
    g->clip_dirty = 1;
}

static void mtx_store(Gpu3d *g, u32 p, int restore) {
    s32 *m, *st;
    switch (g->mtx_mode) {
    case 0: m = g->proj; st = g->proj_stack; break;
    case 3: m = g->tex; st = g->tex_stack; break;
    default: {
        u32 i = p & 31;
        if (i == 31) stack_error(g);
        if (restore) { memcpy(g->pos, g->pos_stack[i], 64); memcpy(g->dir, g->dir_stack[i], 64); }
        else { memcpy(g->pos_stack[i], g->pos, 64); memcpy(g->dir_stack[i], g->dir, 64); }
        g->clip_dirty = 1;
        return;
    }
    }
    if (restore) memcpy(m, st, 64); else memcpy(st, m, 64);
    g->clip_dirty = 1;
}

/* ---- polygons ------------------------------------------------------------------------- */
/* Orientation of three clip-space vertices: the sign of det[(x y w)...], which is the
 * winding the projected triangle has on screen. Scaled down first to stay within 64 bits. */
static int facing(const G3dClipVtx *a, const G3dClipVtx *b, const G3dClipVtx *c) {
    s64 m[9] = {a->x, a->y, a->w, b->x, b->y, b->w, c->x, c->y, c->w};
    s64 big = 0;
    for (int i = 0; i < 9; i++) big |= m[i] < 0 ? -m[i] : m[i];
    int sh = 0;
    while ((big >> sh) >= (1 << 20)) sh++;
    for (int i = 0; i < 9; i++) m[i] >>= sh;
    s64 d = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
    return (d > 0) - (d < 0);
}

static s32 lerp(s32 a, s32 b, s64 num, s64 den) { return a + (s32)(((s64)b - a) * num / den); }

/* Clipping a polygon of n vertices against one plane keeps k of them and adds one for each edge
 * that crosses, at most 2 per kept and per dropped vertex: so at most floor(3n / 2). A convex
 * polygon gains one vertex per plane, but a twisted quad can go 4 -> 6 -> 9 -> 13 -> 19 -> 28 -> 42
 * over the six planes, so the buffers hold that. */
#define CLIP_MAX 42

/* Sutherland-Hodgman against one plane: keeps sign * coord <= w. */
static int clip_plane(const G3dClipVtx *in, int n, G3dClipVtx *out, int axis, int sign) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        const G3dClipVtx *a = &in[i], *b = &in[(i + 1) % n];
        s64 ca = axis == 0 ? a->x : axis == 1 ? a->y : a->z, cb = axis == 0 ? b->x : axis == 1 ? b->y : b->z;
        s64 da = (s64)a->w - sign * ca, db = (s64)b->w - sign * cb;
        if (da >= 0) out[m++] = *a;
        if ((da >= 0) != (db >= 0)) {
            G3dClipVtx *v = &out[m++];
            s64 den = da - db;
            v->x = lerp(a->x, b->x, da, den); v->y = lerp(a->y, b->y, da, den);
            v->z = lerp(a->z, b->z, da, den); v->w = lerp(a->w, b->w, da, den);
            v->s = lerp(a->s, b->s, da, den); v->t = lerp(a->t, b->t, da, den);
            v->r = lerp(a->r, b->r, da, den); v->g = lerp(a->g, b->g, da, den); v->b = lerp(a->b, b->b, da, den);
            v->ram = -1;
            s32 on = sign * v->w;       /* exactly on the plane, whatever the rounding */
            if (axis == 0) v->x = on; else if (axis == 1) v->y = on; else v->z = on;
        }
    }
    return m;
}

/* Clips a triangle or quad against the view volume (-w <= x, y, z <= w): the new vertices are
 * interpolated. Returns the vertex count (0: all outside), up to CLIP_MAX; out holds that many. */
static int clip_polygon(const G3dClipVtx *in, int n, G3dClipVtx *out) {
    G3dClipVtx a[CLIP_MAX], b[CLIP_MAX];
    memcpy(a, in, sizeof *in * (size_t)n);
    for (int p = 0; p < 6 && n; p++) {
        n = clip_plane(a, n, b, p >> 1, (p & 1) ? -1 : 1);
        memcpy(a, b, sizeof *b * (size_t)n);
    }
    memcpy(out, a, sizeof *a * (size_t)n);
    return n;
}

static int inside(const G3dClipVtx *v) {
    return v->x <= v->w && -v->x <= v->w && v->y <= v->w && -v->y <= v->w && v->z <= v->w && -v->z <= v->w;
}

static s32 clamp6(s32 c) { return c < 0 ? 0 : c > 63 ? 63 : c; }

/* clip space to the screen (viewport, y down) and the 24-bit Z-buffer depth */
static void to_screen(const Gpu3d *g, const G3dClipVtx *c, G3dVertex *v) {
    s64 x1 = g->viewport & 0xFF, y1 = (g->viewport >> 8) & 0xFF, x2 = (g->viewport >> 16) & 0xFF, y2 = g->viewport >> 24;
    s64 w = c->w > 0 ? c->w : 1;
    v->x = (s32)(((s64)c->x + w) * (x2 - x1 + 1) / (2 * w) + x1);
    v->y = 192 - (s32)(((s64)c->y + w) * (y2 - y1 + 1) / (2 * w) + y1);
    s64 z = ((s64)c->z * 0x4000 / w + 0x3FFF) * 0x200;
    v->z = (s32)(z < 0 ? 0 : z > 0xFFFFFF ? 0xFFFFFF : z);
    v->w = (s32)w;
    v->s = c->s; v->t = c->t;
    v->r = (u8)clamp6(c->r); v->g = (u8)clamp6(c->g); v->b = (u8)clamp6(c->b);
}

static void unshare(G3dClipVtx **v, int n) { for (int i = 0; i < n; i++) v[i]->ram = -1; }

/* A polygon the vertex list completed: cull, clip, project and store it. Vertices of an
 * unclipped strip polygon that the previous one stored are shared, as in vertex RAM. */
static void polygon(Nds *s, G3dClipVtx **v, int n) {
    Gpu3d *g = &s->g3d;
    u32 attr = g->attr;
    int f = facing(v[0], v[1], v[2]);
    if (!f && n == 4) f = facing(v[0], v[2], v[3]);
    if (!(f > 0 ? (attr >> 7) & 1 : f < 0 ? (attr >> 6) & 1 : 1)) { unshare(v, n); return; }  /* lines always show */
    int clipped = 0, far = 0;
    for (int i = 0; i < n; i++) { clipped |= !inside(v[i]); far |= v[i]->z > v[i]->w; }
    if (far && !(attr & (1u << 12))) { unshare(v, n); return; }
    G3dClipVtx c[CLIP_MAX];
    int m = n;
    for (int i = 0; i < n; i++) c[i] = *v[i];
    /* a polygon with nothing left, or (a twisted quad, which GBATEK forbids) more vertices than polygon
     * RAM takes, is dropped; that is no RAM overflow */
    if (clipped && ((m = clip_polygon(c, n, c)) == 0 || m > G3D_POLY_VERTS)) { unshare(v, n); return; }
    G3dVertex sv[G3D_POLY_VERTS];
    for (int i = 0; i < m; i++) to_screen(g, &c[i], &sv[i]);
    if (!(attr & (1u << 13))) {         /* 0x0-dot polygons behind DISP_1DOT_DEPTH */
        int dot = 1, near = 0;
        for (int i = 0; i < m; i++) {
            dot &= sv[i].x == sv[0].x && sv[i].y == sv[0].y;
            near |= (u32)sv[i].w <= (u32)g->disp_1dot << 9;
        }
        if (dot && !near) { unshare(v, n); return; }
    }
    G3dList *L = &g->lists[g->cur];
    u32 fresh = 0;
    for (int i = 0; i < m; i++) fresh += clipped || v[i]->ram < 0;
    if (L->npoly >= G3D_MAX_POLYS || L->nvtx + fresh > G3D_MAX_VERTS) {
        g->disp3dcnt |= 0x2000;         /* polygon/vertex RAM overflow */
        unshare(v, n);
        return;
    }
    G3dPoly *P = &L->poly[L->npoly++];
    P->attr = attr; P->tex = g->texparam; P->pltt = g->pltt; P->n = (u8)m; P->translucent = 0;
    P->ymin = 255; P->ymax = -255;
    for (int i = 0; i < m; i++) {
        if (!clipped && v[i]->ram >= 0) P->v[i] = (u16)v[i]->ram;
        else {
            P->v[i] = (u16)L->nvtx;
            L->vtx[L->nvtx] = sv[i];
            if (!clipped) v[i]->ram = (s32)L->nvtx;
            L->nvtx++;
        }
        if (sv[i].y < P->ymin) P->ymin = (s16)sv[i].y;
        if (sv[i].y > P->ymax) P->ymax = (s16)sv[i].y;
    }
    if (clipped) unshare(v, n);         /* a clipped strip polygon is stored on its own */
}

static void vertex(Nds *s, s16 x, s16 y, s16 z) {
    Gpu3d *g = &s->g3d;
    g->vx = x; g->vy = y; g->vz = z;
    if (g->prim < 0) return;
    if ((g->texparam >> 30) == 3) {     /* texture coordinates from the vertex */
        const s32 *m = g->tex;
        g->s = (s16)(g->raw_s + (((s64)x * m[0] + (s64)y * m[4] + (s64)z * m[8]) >> 24));
        g->t = (s16)(g->raw_t + (((s64)x * m[1] + (s64)y * m[5] + (s64)z * m[9]) >> 24));
    }
    s32 c[4];
    xform(clip_mtx(g), x, y, z, 0x1000, c);
    G3dClipVtx v = {c[0], c[1], c[2], c[3], g->s, g->t, g->r, g->g, g->b, -1};
    G3dClipVtx *st = g->strip;
    u32 k = g->nsub++;
    switch (g->prim) {
    case 0:                             /* separate triangles */
        st[k % 3] = v;
        if (k % 3 == 2) polygon(s, (G3dClipVtx *[]){&st[0], &st[1], &st[2]}, 3);
        break;
    case 1:                             /* separate quads */
        st[k % 4] = v;
        if (k % 4 == 3) polygon(s, (G3dClipVtx *[]){&st[0], &st[1], &st[2], &st[3]}, 4);
        break;
    case 2:                             /* triangle strip: every second one is wound the other way */
        if (k < 2) { st[k] = v; break; }
        st[2] = v;
        if (k & 1) polygon(s, (G3dClipVtx *[]){&st[1], &st[0], &st[2]}, 3);
        else polygon(s, (G3dClipVtx *[]){&st[0], &st[1], &st[2]}, 3);
        st[0] = st[1]; st[1] = st[2];
        break;
    case 3:                             /* quad strip: v0 v1 v3 v2, then each pair on */
        if (k < 2) { st[k] = v; break; }
        if (!(k & 1)) { st[2] = v; break; }
        st[3] = v;
        polygon(s, (G3dClipVtx *[]){&st[0], &st[1], &st[3], &st[2]}, 4);
        st[0] = st[2]; st[1] = st[3];
        break;
    }
}

/* ---- lighting --------------------------------------------------------------------------- */
static void unpack10(u32 p, s32 *v) {   /* three 1.9 components, as 12-bit fractions */
    v[0] = sext(p, 10) << 3; v[1] = sext(p >> 10, 10) << 3; v[2] = sext(p >> 20, 10) << 3;
}

static void light_vector(Gpu3d *g, u32 p) {
    int i = p >> 30;
    s32 l[3];
    unpack10(p, l);
    xform3(g->dir, l, g->light_vec[i]);
    /* the half vector between the light and the line of sight, always (0, 0, -1) */
    g->half_vec[i][0] = g->light_vec[i][0] >> 1;
    g->half_vec[i][1] = g->light_vec[i][1] >> 1;
    g->half_vec[i][2] = (g->light_vec[i][2] - 0x1000) >> 1;
}

static s32 dot3(const s32 *a, const s32 *b) { return (s32)(((s64)a[0] * b[0] + (s64)a[1] * b[1] + (s64)a[2] * b[2]) >> 12); }

/* NORMAL: the vertex colour from the lights the polygon enables (GBATEK's "Internal
 * Operation on Normal Command"), in 5-bit colour units scaled by 31 * 4096. */
static void normal(Gpu3d *g, u32 p) {
    s32 n[3], nv[3];
    unpack10(p, n);
    if ((g->texparam >> 30) == 2) {     /* texture coordinates from the normal */
        const s32 *m = g->tex;
        g->s = (s16)(g->raw_s + (((s64)n[0] * m[0] + (s64)n[1] * m[4] + (s64)n[2] * m[8]) >> 24));
        g->t = (s16)(g->raw_t + (((s64)n[0] * m[1] + (s64)n[1] * m[5] + (s64)n[2] * m[9]) >> 24));
    }
    xform3(g->dir, n, nv);
    const s64 one = 31 * 4096;
    s64 col[3];
    for (int c = 0; c < 3; c++) col[c] = (s64)((g->spe_emi >> (16 + 5 * c)) & 31) * one;
    for (int i = 0; i < 4; i++) {
        if (!(g->attr & (1u << i))) continue;
        s32 diff = -dot3(g->light_vec[i], nv);
        if (diff < 0) diff = 0;
        s32 shin = -dot3(g->half_vec[i], nv);
        if (shin < 0) shin = 0;
        shin = (s32)(((s64)shin * shin) >> 12);
        if (g->spe_emi & 0x8000) { s32 k = shin >> 5; shin = g->shininess[k > 127 ? 127 : k] << 4; }
        for (int c = 0; c < 3; c++) {
            s64 lc = (g->light_col[i] >> (5 * c)) & 31;
            col[c] += ((g->spe_emi >> (5 * c)) & 31) * lc * shin;
            col[c] += ((g->dif_amb >> (5 * c)) & 31) * lc * diff;
            col[c] += ((g->dif_amb >> (16 + 5 * c)) & 31) * lc * 4096;
        }
    }
    u8 *out[3] = {&g->r, &g->g, &g->b};
    for (int c = 0; c < 3; c++) {
        s64 v = col[c] / one;
        v = v > 31 ? 31 : v;
        *out[c] = (u8)(v ? v * 2 + 1 : 0);
    }
}

/* ---- tests ------------------------------------------------------------------------------- */
/* BOX_TEST: does any face of the box reach into the view volume? */
static int box_test(Gpu3d *g, const u32 *p) {
    s32 x = (s16)p[0], y = (s16)(p[0] >> 16), z = (s16)p[1];
    s32 w = (s16)(p[1] >> 16), h = (s16)p[2], d = (s16)(p[2] >> 16);
    G3dClipVtx c[8];
    memset(c, 0, sizeof c);
    for (int i = 0; i < 8; i++) {
        s32 o[4];
        xform(clip_mtx(g), x + ((i & 1) ? w : 0), y + ((i & 2) ? h : 0), z + ((i & 4) ? d : 0), 0x1000, o);
        c[i].x = o[0]; c[i].y = o[1]; c[i].z = o[2]; c[i].w = o[3];
        if (inside(&c[i])) return 1;
    }
    static const u8 face[6][4] = {{0, 1, 3, 2}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 3, 7, 5}};
    for (int f = 0; f < 6; f++) {
        G3dClipVtx q[16];
        for (int k = 0; k < 4; k++) q[k] = c[face[f][k]];
        if (clip_polygon(q, 4, q)) return 1;
    }
    return 0;
}

/* ---- the command FIFO ------------------------------------------------------------------- */
static u32 fifo_level(const Gpu3d *g) {     /* entries in the FIFO proper: the PIPE takes 4 */
    u32 n = g->fifo_count > 4 ? g->fifo_count - 4 : 0;
    return n > 256 ? 256 : n;
}

int gpu3d_fifo_wants_dma(Nds *s) { return fifo_level(&s->g3d) < 128; }

/* GXSTAT's FIFO interrupt is a level: it stays requested while its condition holds. */
static void fifo_irq(Nds *s) {
    Gpu3d *g = &s->g3d;
    u32 mode = g->gxstat >> 30, n = fifo_level(g);
    if ((mode == 1 && n < 128) || (mode == 2 && n == 0)) nds_request_irq(s, 0, IRQ_GXFIFO);
}

/* a 5-bit colour as vertex colour: 6-bit inside, X*2+1 except for zero */
static void set_color(Gpu3d *g, u32 c) {
    u32 r = c & 31, gr = (c >> 5) & 31, b = (c >> 10) & 31;
    g->r = (u8)(r ? r * 2 + 1 : 0); g->g = (u8)(gr ? gr * 2 + 1 : 0); g->b = (u8)(b ? b * 2 + 1 : 0);
}

static void execute(Nds *s, u8 cmd, const u32 *p) {
    Gpu3d *g = &s->g3d;
    s32 m[16];
    u32 extra = 0;
    switch (cmd) {
    case 0x10: g->mtx_mode = p[0] & 3; break;
    case 0x11: mtx_push(g); break;
    case 0x12: mtx_pop(g, p[0]); break;
    case 0x13: mtx_store(g, p[0], 0); break;
    case 0x14: mtx_store(g, p[0], 1); break;
    case 0x15: mtx_identity(m); mtx_load(g, m); break;
    case 0x16: for (int i = 0; i < 16; i++) m[i] = (s32)p[i]; mtx_load(g, m); break;
    case 0x17: case 0x19: case 0x1A: case 0x1B: case 0x1C:
        mtx_identity(m);
        if (cmd == 0x17 || cmd == 0x19) for (int i = 0; i < 12; i++) m[(i / 3) * 4 + i % 3] = (s32)p[i];
        else if (cmd == 0x1A) for (int i = 0; i < 9; i++) m[(i / 3) * 4 + i % 3] = (s32)p[i];
        else if (cmd == 0x1B) { m[0] = (s32)p[0]; m[5] = (s32)p[1]; m[10] = (s32)p[2]; }
        else { m[12] = (s32)p[0]; m[13] = (s32)p[1]; m[14] = (s32)p[2]; }
        if (cmd == 0x17) mtx_load(g, m);
        else mtx_mult(g, m, cmd == 0x1B);
        if (cmd != 0x17 && cmd != 0x1B && g->mtx_mode == 2) extra = 30;
        break;
    case 0x18:
        for (int i = 0; i < 16; i++) m[i] = (s32)p[i];
        mtx_mult(g, m, 0);
        if (g->mtx_mode == 2) extra = 30;
        break;
    case 0x20: set_color(g, p[0]); break;
    case 0x21: normal(g, p[0]); extra = __builtin_popcount(g->attr & 15) > 0 ? (u32)__builtin_popcount(g->attr & 15) - 1 : 0; break;
    case 0x22:
        g->raw_s = (s16)p[0]; g->raw_t = (s16)(p[0] >> 16);
        if ((g->texparam >> 30) == 1) {     /* (S T 1/16 1/16) * texture matrix */
            const s32 *t = g->tex;
            g->s = (s16)(((s64)g->raw_s * t[0] + (s64)g->raw_t * t[4] + t[8] + t[12]) >> 12);
            g->t = (s16)(((s64)g->raw_s * t[1] + (s64)g->raw_t * t[5] + t[9] + t[13]) >> 12);
        } else { g->s = g->raw_s; g->t = g->raw_t; }
        break;
    case 0x23: vertex(s, (s16)p[0], (s16)(p[0] >> 16), (s16)p[1]); break;
    case 0x24: vertex(s, (s16)(sext(p[0], 10) << 6), (s16)(sext(p[0] >> 10, 10) << 6), (s16)(sext(p[0] >> 20, 10) << 6)); break;
    case 0x25: vertex(s, (s16)p[0], (s16)(p[0] >> 16), g->vz); break;
    case 0x26: vertex(s, (s16)p[0], g->vy, (s16)(p[0] >> 16)); break;
    case 0x27: vertex(s, g->vx, (s16)p[0], (s16)(p[0] >> 16)); break;
    case 0x28:                          /* 1.0.9 differences, added as 12-bit fractions / 8 */
        vertex(s, (s16)(g->vx + sext(p[0], 10)), (s16)(g->vy + sext(p[0] >> 10, 10)), (s16)(g->vz + sext(p[0] >> 20, 10)));
        break;
    case 0x29: g->attr_next = p[0]; break;
    case 0x2A: g->texparam = p[0]; break;
    case 0x2B: g->pltt = p[0] & 0x1FFF; break;
    case 0x30:
        g->dif_amb = p[0];
        if (p[0] & 0x8000) set_color(g, p[0]);      /* the diffuse colour as vertex colour */
        break;
    case 0x31: g->spe_emi = p[0]; break;
    case 0x32: light_vector(g, p[0]); break;
    case 0x33: g->light_col[p[0] >> 30] = (u16)(p[0] & 0x7FFF); break;
    case 0x34: for (int i = 0; i < 32; i++) for (int k = 0; k < 4; k++) g->shininess[i * 4 + k] = (u8)(p[i] >> (8 * k)); break;
    case 0x40:
        g->prim = (int)(p[0] & 3); g->nsub = 0; g->attr = g->attr_next;
        for (int i = 0; i < 4; i++) g->strip[i].ram = -1;
        break;
    case 0x41: break;                   /* END_VTXS does nothing on the hardware */
    case 0x50: g->swap_next = p[0] & 3; g->halted = 1; break;
    case 0x60: g->viewport = p[0]; break;
    case 0x70: g->gxstat = (g->gxstat & ~2u) | (u32)box_test(g, p) << 1; break;
    case 0x71:
        g->vx = (s16)p[0]; g->vy = (s16)(p[0] >> 16); g->vz = (s16)p[1];   /* overwrites the vertex */
        xform(clip_mtx(g), g->vx, g->vy, g->vz, 0x1000, g->pos_result);
        break;
    case 0x72: {
        s32 v[3], r[3];
        unpack10(p[0], v);
        xform3(g->dir, v, r);
        for (int j = 0; j < 3; j++) g->vec_result[j] = (u16)sext((u32)r[j], 13);   /* 4-bit sign, 12-bit fraction */
        break;
    }
    }
    u64 now = s->time;
    if (g->busy_until < now) g->busy_until = now;
    g->busy_until += cost[cmd] + extra;
    if (cmd >= 0x70) g->test_until = g->busy_until;
    if (cmd == 0x11 || cmd == 0x12) g->stack_until = g->busy_until;
}

/* Runs the commands whose parameters are all in, unless SWAP_BUFFERS halted the engine. */
static void fifo_run(Nds *s) {
    Gpu3d *g = &s->g3d;
    while (!g->halted && g->fifo_count) {
        u8 cmd = g->fifo_cmd[g->fifo_head];
        u32 need = np1[cmd] > 1 ? np1[cmd] - 1u : 1u;   /* a command without parameters takes one entry */
        if (g->fifo_count < need) break;
        u32 p[32];
        for (u32 k = 0; k < need; k++) {
            p[k] = g->fifo_par[g->fifo_head];
            g->fifo_head = (g->fifo_head + 1) % G3D_FIFO_CAP;
            g->fifo_count--;
        }
        execute(s, cmd, p);
    }
    fifo_irq(s);
}

static void fifo_push(Nds *s, u8 cmd, u32 par) {
    Gpu3d *g = &s->g3d;
    if (g->fifo_count >= G3D_FIFO_CAP) { g->dropped++; return; }
    u32 i = (g->fifo_head + g->fifo_count) % G3D_FIFO_CAP;
    g->fifo_cmd[i] = cmd; g->fifo_par[i] = par;
    g->fifo_count++;
    fifo_run(s);
}

/* GXFIFO (0x04000400): a word of up to four packed commands, then their parameters. NOP
 * bytes and invalid commands take none. */
static void packed_next(Nds *s) {
    Gpu3d *g = &s->g3d;
    while (g->packed) {
        u8 cmd = (u8)g->packed;
        g->packed >>= 8;
        if (cmd >= 128 || !np1[cmd]) continue;
        if (np1[cmd] == 1) { fifo_push(s, cmd, 0); continue; }
        g->packed_cmd = cmd;
        g->packed_params = np1[cmd] - 1u;
        return;
    }
}

static void gxfifo_write(Nds *s, u32 v) {
    Gpu3d *g = &s->g3d;
    if (g->packed_params) {
        fifo_push(s, g->packed_cmd, v);
        if (!--g->packed_params) packed_next(s);
        return;
    }
    g->packed = v;
    packed_next(s);
}

/* ---- I/O ---------------------------------------------------------------------------------- */
static u32 gxstat(Nds *s) {
    Gpu3d *g = &s->g3d;
    u64 now = s->time;
    u32 n = fifo_level(g);
    u32 v = g->gxstat & 0xC0008002u;
    if (now < g->test_until) v |= 1;
    if (now < g->stack_until) v |= 1u << 14;
    v |= (g->pos_sp & 31) << 8 | (g->proj_sp & 1) << 13 | n << 16;
    if (n < 128) v |= 1u << 25;
    if (!n) v |= 1u << 26;
    if (g->halted || g->fifo_count || now < g->busy_until) v |= 1u << 27;
    return v;
}

static u32 read_word(Nds *s, u32 a) {
    Gpu3d *g = &s->g3d;
    if (a == 0x060) return g->disp3dcnt;
    if (a == 0x320) return 46;          /* RDLINES_COUNT: never short of time */
    if (a == 0x600) return gxstat(s);
    if (a == 0x604) return g->lists[g->cur].npoly | g->lists[g->cur].nvtx << 16;
    if (a >= 0x620 && a < 0x630) return (u32)g->pos_result[(a - 0x620) >> 2];
    if (a == 0x630) return g->vec_result[0] | (u32)g->vec_result[1] << 16;
    if (a == 0x634) return g->vec_result[2];
    if (a >= 0x640 && a < 0x680) return (u32)clip_mtx(g)[(a - 0x640) >> 2];
    if (a >= 0x680 && a < 0x6A4) { u32 k = (a - 0x680) >> 2; return (u32)g->dir[(k / 3) * 4 + k % 3]; }
    return 0;                           /* write-only */
}

int gpu3d_io_read(Nds *s, int cpu, u32 off, int size, u32 *v) {
    if (cpu != 0 || !((off >= 0x60 && off < 0x64) || (off >= 0x320 && off < 0x6A4))) return 0;
    u32 w = read_word(s, off & ~3u);
    w >>= (off & 3) * 8;
    *v = size == 4 ? w : size == 2 ? (w & 0xFFFF) : (w & 0xFF);
    return 1;
}

int gpu3d_io_write(Nds *s, int cpu, u32 off, int size, u32 v) {
    Gpu3d *g = &s->g3d;
    if (cpu != 0) return 0;
    u32 sh = (off & 3) * 8, mask = (size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu : 0xFFu) << sh, val = v << sh;
    if (off >= 0x60 && off < 0x64) {    /* DISP3DCNT: bits 12-13 are acknowledged by writing 1 */
        g->disp3dcnt = (u16)((g->disp3dcnt & ~(mask & 0x4FFF)) | (val & mask & 0x4FFF));
        g->disp3dcnt &= (u16)~(val & mask & 0x3000);
        return 1;
    }
    if (off >= 0x320 && off < 0x400) {
        for (int i = 0; i < size && off + (u32)i < 0x400; i++) g->regs[off - 0x300 + (u32)i] = (u8)(v >> (8 * i));
        return 1;
    }
    if (off >= 0x400 && off < 0x440) { gxfifo_write(s, v); return 1; }
    if (off >= 0x440 && off < 0x600) {
        u32 cmd = ((off & ~3u) - 0x400) >> 2;
        if (np1[cmd]) fifo_push(s, (u8)cmd, v);
        return 1;
    }
    if (off >= 0x600 && off < 0x604) {
        if (mask & val & 0x8000) { g->gxstat &= ~0x8000u; g->proj_sp = 0; g->tex_sp = 0; }
        g->gxstat = (g->gxstat & ~(mask & 0xC0000000u)) | (val & mask & 0xC0000000u);
        fifo_irq(s);
        return 1;
    }
    if (off >= 0x610 && off < 0x612) { g->disp_1dot = (u16)(((g->disp_1dot & ~mask) | (val & mask)) & 0x7FFF); return 1; }
    return off >= 0x604 && off < 0x6A4;     /* the rest is read-only */
}

/* ---- the frame ------------------------------------------------------------------------------ */
/* VBlank: SWAP_BUFFERS hands the polygon list to the renderer and empties the other. */
static void vblank(Nds *s) {
    Gpu3d *g = &s->g3d;
    if (!g->halted) return;
    g->cur ^= 1;
    g->lists[g->cur].npoly = g->lists[g->cur].nvtx = 0;
    g->lists[g->cur].swap = g->swap_next;   /* the swap's bits apply to the polygons after it, not before */
    g->halted = 0;
    if (g->busy_until < s->time) g->busy_until = s->time;
    g->busy_until += 392;
    g->nsub = 0;
    for (int i = 0; i < 4; i++) g->strip[i].ram = -1;
    fifo_run(s);
    nds_gxfifo_dma(s);
}

void gpu3d_line(Nds *s, u32 line) {
    if (line == 191) vblank(s);
    if (line == 262) {                  /* before line 0: the frame engine A shows */
        u32 dispcnt;
        memcpy(&dispcnt, s->io[0], 4);
        if ((dispcnt & 8) && (s->powcnt1 & 8)) gpu3d_render(s);     /* POWCNT1.3: rendering engine on */
    }
    fifo_irq(s);
}

void gpu3d_init(Nds *s) {
    Gpu3d *g = &s->g3d;
    mtx_identity(g->proj); mtx_identity(g->pos); mtx_identity(g->dir); mtx_identity(g->tex);
    g->clip_dirty = 1;
    g->prim = -1;
    g->disp_1dot = 0x7FFF;
    for (int i = 0; i < 4; i++) g->strip[i].ram = -1;
}

int gpu3d_option(Nds *s, const char *name, const char *value) { (void)s; (void)name; (void)value; return 0; }
void gpu3d_shutdown(Nds *s) { (void)s; }
