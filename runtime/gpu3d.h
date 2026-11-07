/* dsnative runtime: the ARM9's 3D engine (GBATEK "DS 3D Video"). gpu3d.c is the geometry
 * engine: the command FIFO and ports (0x04000400-0x040005FF), matrices and their stacks,
 * lighting, polygon assembly, clipping, the box/position/vector tests and GXSTAT.
 * gpu3d_render.c is a software rasterizer for the polygon list SWAP_BUFFERS hands over; its
 * frame is engine A's BG0 when DISPCNT bit 3 is set. */
#ifndef GPU3D_H
#define GPU3D_H
#include "dsr.h"

struct Nds;

#define G3D_MAX_POLYS 2048
#define G3D_MAX_VERTS 6144
#define G3D_FIFO_CAP 16384          /* commands wait here while SWAP_BUFFERS waits for VBlank */
#define G3D_POLY_VERTS 10           /* a convex quad clips to at most 4 + 6 vertices; polygon RAM takes no more */

/* A vertex in vertex RAM: clipped, through the viewport. */
typedef struct {
    s32 x, y;                       /* screen pixels, y down (0..256, 0..192) */
    s32 z;                          /* Z-buffer depth, 0..0xFFFFFF */
    s32 w;                          /* clip w, 20.12 */
    s32 s, t;                       /* texture coordinates, 12.4 texels */
    u8 r, g, b;                     /* 6-bit colour */
} G3dVertex;

typedef struct {
    u32 attr, tex, pltt;            /* POLYGON_ATTR (as BEGIN_VTXS latched it), TEXIMAGE_PARAM, PLTT_BASE */
    u16 v[G3D_POLY_VERTS];          /* vertex RAM indexes; a twisted quad that clips to more is dropped */
    u8 n;                           /* vertices */
    u8 translucent;
    s16 ymin, ymax;
} G3dPoly;

/* One set of polygon and vertex RAM. */
typedef struct {
    G3dVertex vtx[G3D_MAX_VERTS];
    G3dPoly poly[G3D_MAX_POLYS];
    u32 nvtx, npoly;
    u32 swap;                       /* the bits of the SWAP_BUFFERS before it: they are for the polygons after that command */
} G3dList;

/* A vertex in clip space, as the geometry engine holds it before clipping. */
typedef struct {
    s32 x, y, z, w, s, t, r, g, b;
    s32 ram;                        /* its vertex RAM index when a strip can share it, else -1 */
} G3dClipVtx;

typedef struct {
    /* command FIFO and PIPE (one queue): 8-bit command + 32-bit parameter per entry */
    u8 fifo_cmd[G3D_FIFO_CAP];
    u32 fifo_par[G3D_FIFO_CAP];
    u32 fifo_head, fifo_count;
    u32 packed;                     /* packed command bytes still to decode (GXFIFO writes) */
    u32 packed_params;              /* parameters the current packed command still takes */
    u8 packed_cmd;
    int halted;                     /* SWAP_BUFFERS waits for VBlank */
    u32 swap_next;                  /* its bits (sort mode, W-buffering), for the list the next VBlank starts */
    u32 dropped;                    /* commands lost to a full queue */
    u64 busy_until, test_until, stack_until;    /* bus cycles (Nds.time) */
    u32 gxstat;                     /* stored bits: box test result (1), stack error (15), IRQ mode (30-31) */
    /* matrices: 4x4, 20.12, row vectors (v' = v * M) */
    s32 proj[16], pos[16], dir[16], tex[16], clip[16];
    s32 proj_stack[16], pos_stack[32][16], dir_stack[32][16], tex_stack[16];
    u32 proj_sp, pos_sp, tex_sp;
    u32 mtx_mode;
    int clip_dirty;
    /* vertex state */
    s16 vx, vy, vz;                 /* the last vertex, for VTX_XY/XZ/YZ/DIFF */
    s16 raw_s, raw_t;               /* TEXCOORD as sent */
    s32 s, t;                       /* as transformed */
    u8 r, g, b;                     /* vertex colour, 6-bit */
    u32 attr_next, attr;            /* POLYGON_ATTR, and as the last BEGIN_VTXS latched it */
    u32 texparam, pltt;
    int prim;                       /* BEGIN_VTXS type, -1 before the first */
    u32 nsub;                       /* vertices since BEGIN_VTXS */
    G3dClipVtx strip[4];            /* the list's last vertices */
    u32 viewport;
    /* lighting */
    u32 dif_amb, spe_emi;
    s32 light_vec[4][3], half_vec[4][3];
    u16 light_col[4];
    u8 shininess[128];
    /* test results */
    s32 pos_result[4];
    u16 vec_result[3];
    u16 disp_1dot;
    /* polygon lists: lists[cur] is the geometry engine's, lists[cur ^ 1] the renderer's */
    G3dList lists[2];
    int cur;
    /* rendering registers: DISP3DCNT and 0x04000300-0x040003FF as written */
    u16 disp3dcnt;
    u8 regs[0x100];
    /* the frame: colour (6-bit R, G, B in bits 0-17) and 5-bit alpha (bits 18-22), its depth
     * and attributes while rendering */
    u32 color[192 * 256];
    u32 depth[192 * 256];
    u32 pattr[192 * 256];
} Gpu3d;

void gpu3d_init(struct Nds *s);
/* An I/O access (offset from 0x04000000, size 1, 2 or 4) this subsystem handles: returns 1
 * (with *v for a read), or 0 to leave it to nds.c. */
int gpu3d_io_read(struct Nds *s, int cpu, u32 off, int size, u32 *v);
int gpu3d_io_write(struct Nds *s, int cpu, u32 off, int size, u32 v);
/* A command-line style option ("--name", "value"): returns 1 if it is this subsystem's. */
int gpu3d_option(struct Nds *s, const char *name, const char *value);
void gpu3d_shutdown(struct Nds *s);
/* End of a scanline: SWAP_BUFFERS takes effect at VBlank, the frame is drawn before line 0. */
void gpu3d_line(struct Nds *s, u32 line);
/* The geometry FIFO is less than half full: GX FIFO DMA (nds.c) may send its next burst. */
int gpu3d_fifo_wants_dma(struct Nds *s);

/* gpu3d_render.c */
void gpu3d_render(struct Nds *s);
/* A line of the 3D layer for engine A's BG0: colour | 0x8000 where drawn, and 5-bit alpha. */
void gpu3d_bg0_line(struct Nds *s, int line, u16 *px, u8 *alpha);
#endif
