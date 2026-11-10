/* The worker threads (workers.c) and the rasterizer drawing screen bands on them (gpu3d_render.c):
 * every job of a run happens once, and a frame is the same whatever the number of threads. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nds.h"
#include "workers.h"

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("%s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int hits[64];
static void job(void *arg, int j) { (void)arg; __atomic_fetch_add(&hits[j], 1, __ATOMIC_SEQ_CST); }

static void test_jobs(void) {
    for (int threads = 1; threads <= 5; threads++) {
        workers_set_threads(threads);
        for (int jobs = 0; jobs <= 40; jobs += 13) {
            memset(hits, 0, sizeof hits);
            for (int rep = 0; rep < 20; rep++) workers_run(jobs, job, 0);
            for (int j = 0; j < 64; j++) CHECK(hits[j] == (j < jobs ? 20 : 0), "%d threads, %d jobs: job %d ran %d times", threads, jobs, j, hits[j]);
        }
    }
}

/* ---- a frame of random polygons ---- */
#define IO 0x04000000u
enum { DISP3DCNT = 0x60, CLEAR_COLOR = 0x350, CLEAR_DEPTH = 0x354, GXFIFO = 0x400 };
enum { MTX_MODE = 0x10, MTX_IDENTITY = 0x15, COLOR = 0x20, VTX_16 = 0x23, POLYGON_ATTR = 0x29, BEGIN_VTXS = 0x40, END_VTXS, SWAP_BUFFERS = 0x50, VIEWPORT = 0x60 };

static Nds *s;
static u32 rng;
static u32 rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }
static void cmd(int id, u32 p) { wr32(&s->arm9, IO + GXFIFO + (u32)id * 4, p); }
static u32 f12(int v) { return (u32)(v * 4096 / 1000); }
static void vtx(int x, int y, int z) { cmd(VTX_16, (f12(x) & 0xFFFF) | (f12(y) & 0xFFFF) << 16); cmd(VTX_16, f12(z) & 0xFFFF); }

/* Draws the frame for `threads` threads and leaves the machine's colour, depth and attribute buffers in out. */
static void render(int threads, u32 disp, u32 swap, u32 *out) {
    static u8 rom[0x1000];
    workers_set_threads(threads);
    s = nds_create(rom, sizeof rom);
    wr32(&s->arm9, IO + DISP3DCNT, disp);
    cmd(VIEWPORT, 0xBFFF0000u);
    cmd(MTX_MODE, 0); cmd(MTX_IDENTITY, 0);
    cmd(MTX_MODE, 1); cmd(MTX_IDENTITY, 0);
    wr32(&s->arm9, IO + CLEAR_COLOR, 31u << 16 | 0x1234u);
    wr32(&s->arm9, IO + CLEAR_DEPTH, 0x7FFF);
    rng = 12345;
    for (int i = 0; i < 300; i++) {
        u32 alpha = i % 7 == 0 ? 0 : i % 5 == 0 ? 8 + rnd() % 20 : 31;      /* wire frames, translucent, opaque */
        cmd(POLYGON_ATTR, 0xC0 | alpha << 16 | (rnd() % 8) << 24 | (i % 11 == 0 ? 1u << 14 : 0));
        int cx = (int)(rnd() % 1800) - 900, cy = (int)(rnd() % 1800) - 900, z = (int)(rnd() % 1800) - 900;
        int size = i % 13 == 0 ? 1 : 40 + (int)(rnd() % 400);                          /* a few tiny and some tall ones */
        cmd(BEGIN_VTXS, i % 3 == 0 ? 1u : 0u);
        int n = i % 3 == 0 ? 4 : 3;
        for (int k = 0; k < n; k++) {
            cmd(COLOR, rnd() & 0x7FFF);
            int dx = k == 0 || k == 3 ? -size : size, dy = k < 2 ? size : -size;
            vtx(cx + dx, cy + dy + (i % 17 == 0 ? 0 : (int)(rnd() % 30)), z);
        }
        cmd(END_VTXS, 0);
    }
    cmd(SWAP_BUFFERS, swap);                /* bit 1: the W buffer */
    gpu3d_line(s, 191);
    gpu3d_render(s);
    memcpy(out, s->g3d.color, sizeof s->g3d.color);
    memcpy(out + 192 * 256, s->g3d.depth, sizeof s->g3d.depth);
    memcpy(out + 2 * 192 * 256, s->g3d.pattr, sizeof s->g3d.pattr);
    CHECK(s->g3d.lists[s->g3d.cur ^ 1].npoly >= 100, "only %u polygons reached the renderer", s->g3d.lists[s->g3d.cur ^ 1].npoly);
    nds_shutdown(s);
}

static void test_frame(void) {
    static u32 ref[3 * 192 * 256], got[3 * 192 * 256];
    u32 configs[] = {0x0000, 0x0008, 0x0038 | 0x0010};      /* plain; alpha blending; blending, edge marking and anti-aliasing */
    for (u32 swap = 0; swap <= 2; swap += 2)                /* the Z buffer, then the W buffer */
    for (unsigned c = 0; c < sizeof configs / sizeof *configs; c++) {
        render(1, configs[c], swap, ref);
        int drawn = 0;
        for (int i = 0; i < 192 * 256; i++) drawn += ref[i] != ref[0];
        CHECK(drawn > 5000, "config %x, swap %u: the scene draws almost nothing (%d pixels differ from the corner)", configs[c], swap, drawn);
        for (int threads = 2; threads <= 4; threads++) {
            render(threads, configs[c], swap, got);
            int bad = -1;
            for (int i = 0; i < 3 * 192 * 256 && bad < 0; i++) if (ref[i] != got[i]) bad = i;
            CHECK(bad < 0, "config %x, swap %u, %d threads: buffer %d pixel %d differs: %08x against %08x", configs[c], swap, threads,
                  bad / (192 * 256), bad % (192 * 256), got[bad < 0 ? 0 : bad], ref[bad < 0 ? 0 : bad]);
        }
    }
}

int main(void) {
    test_jobs();
    test_frame();
    if (!fails) printf("worker threads and banded rendering ok\n");
    return fails != 0;
}
