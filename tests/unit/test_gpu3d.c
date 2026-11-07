/* The 3D engine (gpu3d.c, gpu3d_render.c), driven through the ARM9's bus the way a game drives it:
 * the command FIFO and direct command ports, GXSTAT, the matrix readbacks and the box, position
 * and vector tests; then polygons through the geometry engine and rasterizer, read from the
 * frame it keeps and from engine A's BG0.
 *
 * Pixels are checked well inside or outside polygons, except for the exact fills of axis-aligned
 * boxes: where the rasterizer's rules for edges differ from the hardware's (which GBATEK
 * describes only loosely) the tests stay clear. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nds.h"

static Nds *s;
static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("%s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define IO 0x04000000u
enum { DISP3DCNT = 0x60, CLEAR_COLOR = 0x350, CLEAR_DEPTH = 0x354, GXFIFO = 0x400, GXSTAT = 0x600, RAM_COUNT = 0x604,
       POS_RESULT = 0x620, VEC_RESULT = 0x630, CLIPMTX = 0x640, VECMTX = 0x680 };
/* command ids, which are also the port number: 0x04000400 + id * 4 */
enum { MTX_MODE = 0x10, MTX_PUSH, MTX_POP, MTX_STORE, MTX_RESTORE, MTX_IDENTITY, MTX_LOAD_4x4, MTX_LOAD_4x3, MTX_MULT_4x4,
       MTX_MULT_4x3, MTX_MULT_3x3, MTX_SCALE, MTX_TRANS,
       COLOR = 0x20, NORMAL, TEXCOORD, VTX_16, VTX_10, VTX_XY, VTX_XZ, VTX_YZ, VTX_DIFF, POLYGON_ATTR,
       DIF_AMB = 0x30, SPE_EMI, LIGHT_VECTOR, LIGHT_COLOR, SHININESS,
       BEGIN_VTXS = 0x40, END_VTXS, SWAP_BUFFERS = 0x50, VIEWPORT = 0x60, BOX_TEST = 0x70, POS_TEST, VEC_TEST };

static void reg(u32 off, u32 v) { wr32(&s->arm9, IO + off, v); }
static void reg16(u32 off, u32 v) { wr16(&s->arm9, IO + off, v); }
static u32 get(u32 off) { return rd32(&s->arm9, IO + off); }
static void cmd(int id, u32 p) { reg(GXFIFO + (u32)id * 4, p); }
static void cmd0(int id) { cmd(id, 0); }
static void cmd3(int id, u32 a, u32 b, u32 c) { cmd(id, a); cmd(id, b); cmd(id, c); }
static s32 F(double v) { return (s32)(v * 4096); }          /* 20.12 */
static u32 xy(double x, double y) { return (u32)(u16)(s16)F(x) | (u32)(u16)(s16)F(y) << 16; }
static u32 pack3(int x, int y, int z) { return (u32)(x & 0x3FF) | (u32)(y & 0x3FF) << 10 | (u32)(z & 0x3FF) << 20; }
static void advance(u64 cycles) { s->time += cycles; }
static u32 gxstat(void) { return get(GXSTAT); }

static void fresh(void) {
    static u8 rom[0x1000];
    if (s) nds_shutdown(s);
    s = nds_create(rom, sizeof rom);
    cmd(VIEWPORT, 0xBFFF0000u);             /* full screen: its power-on value is zero, which is one pixel */
}

static void load(const s32 *m) { for (int i = 0; i < 16; i++) cmd(MTX_LOAD_4x4, (u32)m[i]); }
static void mult(const s32 *m) { for (int i = 0; i < 16; i++) cmd(MTX_MULT_4x4, (u32)m[i]); }
static void clip_mtx(s32 *m) { for (int i = 0; i < 16; i++) m[i] = (s32)get(CLIPMTX + 4u * (u32)i); }
static void vec_mtx(s32 *m) { for (int i = 0; i < 9; i++) m[i] = (s32)get(VECMTX + 4u * (u32)i); }

/* the identity in a 4x4 (n = 16) or 3x3 (n = 9) matrix of 20.12 numbers */
static int is_ident(const s32 *m, int n) {
    int w = n == 9 ? 3 : 4;
    for (int i = 0; i < n; i++) if (m[i] != (i % w == i / w ? 0x1000 : 0)) return 0;
    return 1;
}

/* ---- matrices -------------------------------------------------------------------------- */
static void test_matrices(void) {
    s32 m[16];
    fresh();
    clip_mtx(m);
    CHECK(is_ident(m, 16), "power-on clip matrix is not the identity");
    /* C = M * C: a translation then a scale scales the object first, then moves it */
    cmd(MTX_MODE, 1);
    cmd0(MTX_IDENTITY);
    cmd3(MTX_TRANS, (u32)F(1), (u32)F(2), (u32)F(3));
    cmd3(MTX_SCALE, (u32)F(2), (u32)F(2), (u32)F(2));
    clip_mtx(m);
    CHECK(m[0] == F(2) && m[5] == F(2) && m[10] == F(2) && m[15] == F(1) && m[12] == F(1) && m[13] == F(2) && m[14] == F(3),
          "translate then scale: %x %x %x / %x %x %x", m[0], m[5], m[10], m[12], m[13], m[14]);
    cmd0(MTX_IDENTITY);
    cmd3(MTX_SCALE, (u32)F(2), (u32)F(2), (u32)F(2));
    cmd3(MTX_TRANS, (u32)F(1), (u32)F(2), (u32)F(3));
    clip_mtx(m);
    CHECK(m[0] == F(2) && m[12] == F(2) && m[13] == F(4) && m[14] == F(6), "scale then translate: %x %x %x", m[12], m[13], m[14]);

    /* the clip matrix is position * projection */
    fresh();
    cmd(MTX_MODE, 0);
    cmd0(MTX_IDENTITY);
    cmd3(MTX_SCALE, (u32)F(0.5), (u32)F(0.5), (u32)F(0.5));
    cmd(MTX_MODE, 1);
    cmd0(MTX_IDENTITY);
    cmd3(MTX_TRANS, (u32)F(4), 0, 0);
    clip_mtx(m);
    CHECK(m[0] == F(0.5) && m[12] == F(2) && m[15] == F(1), "clip = position * projection: %x %x", m[0], m[12]);

    /* 4x3 and 3x3 forms: the missing row and column are those of the identity */
    fresh();
    cmd(MTX_MODE, 1);
    static const s32 m43[12] = {0x2000, 0, 0, 0, 0x3000, 0, 0, 0, 0x4000, 0x100, 0x200, 0x300};
    for (int i = 0; i < 12; i++) cmd(MTX_LOAD_4x3, (u32)m43[i]);
    clip_mtx(m);
    CHECK(m[0] == 0x2000 && m[5] == 0x3000 && m[10] == 0x4000 && m[3] == 0 && m[7] == 0 && m[11] == 0 && m[15] == 0x1000 &&
          m[12] == 0x100 && m[13] == 0x200 && m[14] == 0x300, "4x3 load");
    cmd0(MTX_IDENTITY);
    for (int i = 0; i < 9; i++) cmd(MTX_MULT_3x3, (u32)(i % 4 == 0 ? 0x2000 : 0));
    clip_mtx(m);
    CHECK(m[0] == 0x2000 && m[5] == 0x2000 && m[10] == 0x2000 && m[15] == 0x1000 && m[12] == 0, "3x3 multiply");
    cmd0(MTX_IDENTITY);
    for (int i = 0; i < 12; i++) cmd(MTX_MULT_4x3, (u32)m43[i]);
    clip_mtx(m);
    CHECK(m[0] == 0x2000 && m[12] == 0x100 && m[15] == 0x1000, "4x3 multiply");

    /* the four products of a matrix element add up at 24 fractional bits before the shift:
     * 4 * (1 * 0x800) is 2, where four separately truncated products would be 0 */
    fresh();
    cmd(MTX_MODE, 1);
    s32 a[16], b[16];
    for (int i = 0; i < 16; i++) { a[i] = 0x800; b[i] = 1; }
    load(a);
    mult(b);
    clip_mtx(m);
    CHECK(m[0] == 2 && m[15] == 2, "accumulated products: %d", m[0]);
    /* and the shift rounds towards minus infinity: -1 * 0x800 is -1, not 0 */
    for (int i = 0; i < 16; i++) { a[i] = i % 5 == 0 ? 0x800 : 0; b[i] = i % 5 == 0 ? -1 : 0; }
    load(a);
    mult(b);
    clip_mtx(m);
    CHECK(m[0] == -1, "negative products truncate down: %d", m[0]);
    /* a 32-bit result wraps like the hardware's registers */
    for (int i = 0; i < 16; i++) { a[i] = i % 5 == 0 ? 0x7FFFFFFF : 0; b[i] = i % 5 == 0 ? 0x2000 : 0; }
    load(a);
    mult(b);
    clip_mtx(m);
    CHECK(m[0] == (s32)(((s64)0x7FFFFFFF * 0x2000) >> 12), "wide result %x", m[0]);
}

static void test_direction_matrix(void) {
    s32 v[9], m[16];
    fresh();
    vec_mtx(v);
    CHECK(is_ident(v, 9), "power-on vector matrix");
    /* mode 2 sets both; MTX_SCALE leaves the vector matrix alone even there */
    cmd(MTX_MODE, 2);
    cmd0(MTX_IDENTITY);
    cmd3(MTX_SCALE, (u32)F(2), (u32)F(2), (u32)F(2));
    vec_mtx(v);
    clip_mtx(m);
    CHECK(is_ident(v, 9), "scale in mode 2 changed the vector matrix");
    CHECK(m[0] == F(2), "scale in mode 2 skipped the position matrix");
    /* a rotation of 90 degrees about Z goes to both */
    static const s32 rz[9] = {0, 0x1000, 0, -0x1000, 0, 0, 0, 0, 0x1000};
    for (int i = 0; i < 9; i++) cmd(MTX_MULT_3x3, (u32)rz[i]);
    vec_mtx(v);
    CHECK(v[1] == 0x1000 && v[3] == -0x1000 && v[0] == 0 && v[8] == 0x1000, "rotation in the vector matrix");
    /* mode 1 changes only the position matrix, but its stack commands save and restore both */
    cmd(MTX_MODE, 1);
    cmd0(MTX_PUSH);
    cmd(MTX_MODE, 2);
    cmd0(MTX_IDENTITY);
    vec_mtx(v);
    CHECK(is_ident(v, 9), "identity in mode 2");
    cmd(MTX_MODE, 1);
    cmd(MTX_POP, 1);
    vec_mtx(v);
    CHECK(v[1] == 0x1000 && v[3] == -0x1000, "pop in mode 1 restored the vector matrix too");
    /* a light vector or VEC_TEST sees the rotation */
    cmd(MTX_MODE, 2);
    cmd(VEC_TEST, pack3(0x100, 0, 0));      /* (0.5, 0, 0) */
    advance(100);
    u32 r0 = get(VEC_RESULT), r1 = get(VEC_RESULT + 4);
    /* (0.5, 0, 0) * R: row 0 of R is (0, 1, 0), so the result is (0, 0.5, 0) */
    CHECK((r0 & 0xFFFF) == 0 && (r0 >> 16) == 0x0800 && (r1 & 0xFFFF) == 0, "VEC_TEST %04x %04x %04x", r0 & 0xFFFF, r0 >> 16, r1 & 0xFFFF);
}

static void test_vec_test(void) {
    fresh();
    cmd(MTX_MODE, 2);
    cmd0(MTX_IDENTITY);
    CHECK(!(gxstat() & 1), "busy before VEC_TEST");
    cmd(VEC_TEST, pack3(0x100, 0x300, 0));  /* (0.5, -0.5, 0) */
    CHECK(gxstat() & 1, "VEC_TEST not busy");
    advance(100);
    CHECK(!(gxstat() & 1), "VEC_TEST still busy");
    u32 r0 = get(VEC_RESULT), r1 = get(VEC_RESULT + 4);
    CHECK((r0 & 0xFFFF) == 0x0800 && (r0 >> 16) == 0xF800 && (r1 & 0xFFFF) == 0, "sign extension %04x %04x %04x", r0 & 0xFFFF, r0 >> 16, r1 & 0xFFFF);
    /* 0.75 through a matrix that doubles is 1.5: 4-bit sign, 12-bit fraction, so it overflows to -0.5 */
    cmd0(MTX_IDENTITY);
    for (int i = 0; i < 9; i++) cmd(MTX_MULT_3x3, (u32)(i % 4 == 0 ? 0x2000 : 0));
    cmd(VEC_TEST, pack3(0x180, 0, 0));
    advance(100);
    r0 = get(VEC_RESULT);
    CHECK((r0 & 0xFFFF) == 0xF800, "overflow %04x", r0 & 0xFFFF);
    /* halfword and byte reads of the same registers */
    CHECK(rd16(&s->arm9, IO + VEC_RESULT) == 0xF800 && rd8(&s->arm9, IO + VEC_RESULT + 1) == 0xF8, "narrow reads");
}

/* ---- stacks --------------------------------------------------------------------------------- */
static void test_stacks(void) {
    s32 m[16];
    fresh();
    cmd(MTX_MODE, 1);
    CHECK((gxstat() & 0xE000) == 0, "stack state at power-on %x", gxstat() & 0xE000);
    /* 31 pushes fill the stack's valid entries, 0 to 30 */
    for (int i = 0; i < 31; i++) {
        cmd0(MTX_IDENTITY);
        cmd3(MTX_TRANS, (u32)F(i), 0, 0);
        cmd0(MTX_PUSH);
    }
    CHECK(((gxstat() >> 8) & 31) == 31 && !(gxstat() & 0x8000), "after 31 pushes: level %u error %u", (gxstat() >> 8) & 31, (gxstat() >> 15) & 1);
    cmd0(MTX_PUSH);
    CHECK(gxstat() & 0x8000, "the 32nd push did not set the overflow flag");
    CHECK(((gxstat() >> 8) & 31) == 0, "the pointer is 6 bits and GXSTAT shows 5: %u", (gxstat() >> 8) & 31);
    reg(GXSTAT, 0x8000);
    CHECK(!(gxstat() & 0x8000), "writing 1 to bit 15 did not reset the flag");
    /* pop one and the matrix pushed last comes back */
    cmd(MTX_POP, 2);
    clip_mtx(m);
    CHECK(((gxstat() >> 8) & 31) == 30 && !(gxstat() & 0x8000), "level after popping 2: %u", (gxstat() >> 8) & 31);
    CHECK(m[12] == F(30), "entry 30 is back after popping 2: %x", m[12]);
    /* start over: a pop with nothing pushed underflows */
    fresh();
    cmd(MTX_MODE, 1);
    cmd(MTX_POP, 1);
    CHECK(gxstat() & 0x8000, "pop from an empty stack did not set the flag");
    CHECK(((gxstat() >> 8) & 31) == 31, "the pointer wrapped to 63, shown as %u", (gxstat() >> 8) & 31);
    fresh();                                /* the flag's reset leaves the position pointer where it is */
    cmd(MTX_MODE, 1);
    /* push, change, pop: the pushed matrix returns, and popping 1 is the usual offset */
    cmd0(MTX_IDENTITY);
    cmd3(MTX_TRANS, (u32)F(1), 0, 0);
    cmd0(MTX_PUSH);
    cmd0(MTX_IDENTITY);
    cmd(MTX_POP, 1);
    clip_mtx(m);
    CHECK(m[12] == F(1) && !(gxstat() & 0x8000) && ((gxstat() >> 8) & 31) == 0, "push/pop %x level %u", m[12], (gxstat() >> 8) & 31);
    /* store and restore address the stack without moving its pointer; 31 is an overflow */
    cmd0(MTX_IDENTITY);
    cmd3(MTX_TRANS, (u32)F(3), 0, 0);
    cmd(MTX_STORE, 5);
    cmd0(MTX_IDENTITY);
    cmd(MTX_RESTORE, 5);
    clip_mtx(m);
    CHECK(m[12] == F(3) && ((gxstat() >> 8) & 31) == 0 && !(gxstat() & 0x8000), "store/restore %x", m[12]);
    cmd(MTX_STORE, 30);
    CHECK(!(gxstat() & 0x8000), "entry 30 is valid");
    cmd(MTX_STORE, 31);
    CHECK(gxstat() & 0x8000, "entry 31 did not set the flag");
    reg(GXSTAT, 0x8000);
    /* a multi-level pop: three pushes, pop 3, the first one is back */
    cmd0(MTX_IDENTITY);
    cmd3(MTX_TRANS, (u32)F(7), 0, 0);
    cmd0(MTX_PUSH);
    cmd0(MTX_IDENTITY);
    cmd0(MTX_PUSH);
    cmd0(MTX_PUSH);
    cmd(MTX_POP, 3);
    clip_mtx(m);
    CHECK(m[12] == F(7), "pop 3 gave %x", m[12]);
    /* a negative offset goes up the stack again: (-1) & 63 */
    cmd(MTX_POP, 0x3F);
    clip_mtx(m);
    CHECK(is_ident(m, 16) && ((gxstat() >> 8) & 31) == 1, "pop -1: level %u", (gxstat() >> 8) & 31);

    /* the projection stack has one entry: a second push or a second pop is an error */
    fresh();
    cmd(MTX_MODE, 0);
    cmd0(MTX_PUSH);
    CHECK(((gxstat() >> 13) & 1) == 1 && !(gxstat() & 0x8000), "projection push: GXSTAT %x", gxstat());
    cmd0(MTX_PUSH);
    CHECK(gxstat() & 0x8000, "second projection push");
    reg(GXSTAT, 0x8000);
    CHECK(!(gxstat() & 0x8000) && !(gxstat() & 0x2000), "write to GXSTAT.15 resets the projection pointer");
    cmd0(MTX_PUSH);
    cmd(MTX_POP, 1);
    CHECK(!(gxstat() & 0x8000) && !(gxstat() & 0x2000), "projection pop");
    cmd(MTX_POP, 1);
    CHECK(gxstat() & 0x8000, "projection underflow");
    /* busy: a push or pop is under way for its cycle count */
    fresh();
    cmd0(MTX_PUSH);
    CHECK(gxstat() & (1u << 14), "matrix stack busy flag");
    advance(40);
    CHECK(!(gxstat() & (1u << 14)), "matrix stack busy flag stuck");
}

/* ---- the command FIFO ---------------------------------------------------------------------------- */
static void test_fifo(void) {
    s32 m[16];
    /* four packed commands in one word: MTX_MODE (1 parameter), MTX_IDENTITY and MTX_PUSH (none), MTX_TRANS (3) */
    fresh();
    reg(GXFIFO, 0x1C111510u);
    CHECK(((gxstat() >> 8) & 31) == 0, "nothing ran before MTX_MODE's parameter");
    reg(GXFIFO, 1);
    CHECK(((gxstat() >> 8) & 31) == 1, "identity and push ran after the first parameter: level %u", (gxstat() >> 8) & 31);
    reg(GXFIFO, (u32)F(1)); reg(GXFIFO, (u32)F(2));
    clip_mtx(m);
    CHECK(m[12] == 0, "MTX_TRANS ran short of parameters");
    reg(GXFIFO, (u32)F(3));
    clip_mtx(m);
    CHECK(m[12] == F(1) && m[13] == F(2) && m[14] == F(3), "packed word: %x %x %x", m[12], m[13], m[14]);
    /* NOP bytes and invalid commands take no parameters; the next word is a command word again */
    reg(GXFIFO, 0x0000FF00u | 0x15u);
    reg(GXFIFO, 0x00000015u);
    clip_mtx(m);
    CHECK(is_ident(m, 16), "identity after nops");
    /* unpacked: a command word with the upper 24 bits zero, then its parameters */
    reg(GXFIFO, MTX_TRANS);
    reg(GXFIFO, (u32)F(5)); reg(GXFIFO, 0); reg(GXFIFO, 0);
    clip_mtx(m);
    CHECK(m[12] == F(5), "unpacked command");
    /* GXFIFO is mirrored over 0x04000400-0x0400043F */
    reg(GXFIFO + 0x3C, MTX_IDENTITY);
    clip_mtx(m);
    CHECK(is_ident(m, 16), "mirror of GXFIFO");
    /* a packed word of commands without parameters runs them all: four pushes */
    fresh();
    cmd(MTX_MODE, 1);
    reg(GXFIFO, 0x11111111u);
    CHECK(((gxstat() >> 8) & 31) == 4, "four pushes: level %u", (gxstat() >> 8) & 31);
    /* the direct ports and the FIFO interleave in order */
    fresh();
    cmd(MTX_MODE, 1);
    cmd3(MTX_TRANS, (u32)F(1), 0, 0);
    reg(GXFIFO, 0x00001C00u | 0x15u);       /* identity, then trans */
    reg(GXFIFO, (u32)F(9)); reg(GXFIFO, 0); reg(GXFIFO, 0);
    clip_mtx(m);
    CHECK(m[12] == F(9), "port then packed");
}

static void test_gxstat(void) {
    fresh();
    advance(10);                            /* fresh()'s VIEWPORT command */
    u32 st = gxstat();
    CHECK((st & (3u << 25)) == (3u << 25) && !(st & (1u << 27)) && ((st >> 16) & 0x1FF) == 0, "idle: %08x", st);
    /* commands run for their cycle counts: busy while those last */
    cmd0(MTX_IDENTITY);                     /* 19 */
    CHECK(gxstat() & (1u << 27), "not busy after a command");
    advance(18);
    CHECK(gxstat() & (1u << 27), "busy ended early");
    advance(2);
    CHECK(!(gxstat() & (1u << 27)), "busy never ended");
    /* SWAP_BUFFERS halts the engine until VBlank: what follows waits in the FIFO */
    cmd(SWAP_BUFFERS, 0);
    CHECK(gxstat() & (1u << 27), "halted engine is busy");
    for (int i = 0; i < 10; i++) cmd0(MTX_IDENTITY);
    st = gxstat();
    CHECK(((st >> 16) & 0x1FF) == 6 && !(st & (1u << 26)) && (st & (1u << 25)), "10 queued (4 in the pipe): %08x", st);
    for (int i = 0; i < 130; i++) cmd0(MTX_IDENTITY);
    st = gxstat();
    CHECK(((st >> 16) & 0x1FF) == 136 && !(st & (1u << 25)), "140 queued: %08x", st);
    for (int i = 0; i < 130; i++) cmd0(MTX_IDENTITY);
    st = gxstat();
    CHECK(((st >> 16) & 0x1FF) == 256 && (st & (1u << 24)), "full at 256 plus the pipe: %08x", st);
    gpu3d_line(s, 191);                     /* VBlank: the swap goes through and the queue runs */
    st = gxstat();
    CHECK(((st >> 16) & 0x1FF) == 0 && (st & (1u << 26)) && (st & (1u << 27)), "after VBlank: %08x", st);
    advance(392 + 270 * 19);
    CHECK(!(gxstat() & (1u << 27)), "queue never finished");
    /* the interrupt: a level, while the condition holds */
    fresh();
    s->ifl[0] = 0;
    reg(GXSTAT, 1u << 30);                  /* less than half full: true when empty */
    CHECK(s->ifl[0] & IRQ_GXFIFO, "half-empty interrupt");
    s->ifl[0] = 0;
    reg(GXSTAT, 0);
    cmd(SWAP_BUFFERS, 0);
    for (int i = 0; i < 140; i++) cmd0(MTX_IDENTITY);
    reg(GXSTAT, 1u << 30);
    CHECK(!(s->ifl[0] & IRQ_GXFIFO), "interrupt while the FIFO is over half full");
    gpu3d_line(s, 191);
    CHECK(s->ifl[0] & IRQ_GXFIFO, "interrupt once the FIFO drained");
    s->ifl[0] = 0;
    gpu3d_line(s, 10);
    CHECK(s->ifl[0] & IRQ_GXFIFO, "the interrupt stays requested while true");
    s->ifl[0] = 0;
    reg(GXSTAT, 2u << 30);                  /* empty */
    CHECK(s->ifl[0] & IRQ_GXFIFO, "empty interrupt");
    s->ifl[0] = 0;
    reg(GXSTAT, 0);
    gpu3d_line(s, 11);
    CHECK(!(s->ifl[0] & IRQ_GXFIFO), "interrupt with the mode off");
    /* the mode bits read back; the rest of what was written does not stick */
    reg(GXSTAT, (2u << 30) | 0x3FFFFFFF);
    CHECK((gxstat() >> 30) == 2 && !(gxstat() & 0x8000) && ((gxstat() >> 8) & 31) == 0, "mode bits %x, GXSTAT %08x", gxstat() >> 30, gxstat());
}

static void test_tests(void) {
    s32 r[4];
    fresh();
    /* POS_TEST: (x, y, z, 1) times the clip matrix, 20.12 */
    cmd(MTX_MODE, 1);
    cmd0(MTX_IDENTITY);
    cmd3(MTX_TRANS, (u32)F(1), (u32)F(2), (u32)F(3));
    cmd(POS_TEST, xy(0.5, 0.25));
    cmd(POS_TEST, (u32)(u16)(s16)F(-0.5));
    CHECK(gxstat() & 1, "position test not busy");
    advance(1000);
    CHECK(!(gxstat() & 1), "position test still busy");
    for (int i = 0; i < 4; i++) r[i] = (s32)get(POS_RESULT + 4u * (u32)i);
    CHECK(r[0] == F(1.5) && r[1] == F(2.25) && r[2] == F(2.5) && r[3] == F(1), "POS_TEST %x %x %x %x", r[0], r[1], r[2], r[3]);
    /* with a perspective-style projection w is not 1: w = -z */
    fresh();
    cmd(MTX_MODE, 0);
    s32 p[16] = {0x1000, 0, 0, 0, 0, 0x1000, 0, 0, 0, 0, 0x1000, -0x1000, 0, 0, 0, 0};
    load(p);
    cmd(POS_TEST, xy(1, 1));
    cmd(POS_TEST, (u32)(u16)(s16)F(-2));
    for (int i = 0; i < 4; i++) r[i] = (s32)get(POS_RESULT + 4u * (u32)i);
    CHECK(r[0] == F(1) && r[1] == F(1) && r[2] == F(-2) && r[3] == F(2), "POS_TEST with w: %x %x %x %x", r[0], r[1], r[2], r[3]);
    /* a negative result keeps its sign in the 32-bit word */
    cmd(POS_TEST, xy(-1, -1));
    cmd(POS_TEST, 0);
    CHECK((s32)get(POS_RESULT) == F(-1), "negative POS_TEST");

    /* BOX_TEST: does a face of the box reach into the view volume (-1..1 on each axis here) */
    fresh();
    struct { double x, y, z, w, h, d; int want; const char *what; } box[] = {
        {0.25, 0.25, 0.25, 0.25, 0.25, 0.25, 1, "inside"},
        {2, 2, 2, 1, 1, 1, 0, "outside"},
        {0.5, 0.5, 0.5, 1, 1, 1, 1, "straddling a corner"},
        {-3, -0.25, -0.25, 6, 0.5, 0.5, 1, "a long box crossing the volume: no corner inside"},
        {-2, -2, -2, 4, 4, 4, 0, "enclosing the whole view volume"},
        {-1, -1, -1, 2, 2, 2, 1, "exactly the volume"},
        {1.5, -0.5, -0.5, 1, 1, 1, 0, "beside it"},
    };
    for (unsigned i = 0; i < sizeof box / sizeof *box; i++) {
        cmd(BOX_TEST, xy(box[i].x, box[i].y));
        cmd(BOX_TEST, (u32)(u16)(s16)F(box[i].z) | (u32)(u16)(s16)F(box[i].w) << 16);
        cmd(BOX_TEST, (u32)(u16)(s16)F(box[i].h) | (u32)(u16)(s16)F(box[i].d) << 16);
        CHECK(gxstat() & 1, "box test %u not busy", i);
        advance(110);
        CHECK(!(gxstat() & 1), "box test %u busy", i);
        CHECK(((gxstat() >> 1) & 1) == (u32)box[i].want, "BOX_TEST %s: %u", box[i].what, (gxstat() >> 1) & 1);
    }
    /* the box is transformed by the current matrix */
    cmd(MTX_MODE, 1);
    cmd0(MTX_IDENTITY);
    cmd3(MTX_TRANS, (u32)F(5), 0, 0);
    cmd(BOX_TEST, xy(0.25, 0.25));
    cmd(BOX_TEST, (u32)(u16)(s16)F(0.25) | (u32)(u16)(s16)F(0.25) << 16);
    cmd(BOX_TEST, (u32)(u16)(s16)F(0.25) | (u32)(u16)(s16)F(0.25) << 16);
    advance(110);
    CHECK(!(gxstat() & 2), "box moved out of the volume by the position matrix");
}

/* ---- polygons and the rasterizer ------------------------------------------------------------ */
/* With identity matrices and the full-screen viewport, NDC x is pixel x = (x + 1) * 128 and NDC y
 * is row 192 - (y + 1) * 96. The clear colour is opaque black. */
static void scene(void) {
    fresh();
    cmd(MTX_MODE, 0); cmd0(MTX_IDENTITY);
    cmd(MTX_MODE, 1); cmd0(MTX_IDENTITY);
    reg(CLEAR_COLOR, 31u << 16);
    reg(CLEAR_DEPTH, 0x7FFF);
}
enum { ATTR_FRONT = 1 << 7, ATTR_BACK = 1 << 6, ATTR_FAR = 1 << 12, ATTR_DOT = 1 << 13, ATTR_EQUAL = 1 << 14, ATTR_DEPTH = 1 << 11 };
static u32 attr(u32 alpha, u32 id, u32 flags) { return flags | alpha << 16 | id << 24; }
#define SOLID attr(31, 0, ATTR_FRONT)
static u32 rgb5(u32 r, u32 g, u32 b) { return r | g << 5 | b << 10; }
static void color(u32 r, u32 g, u32 b) { cmd(COLOR, rgb5(r, g, b)); }
static void vtx(double x, double y, double z) { cmd(VTX_16, xy(x, y)); cmd(VTX_16, (u32)(u16)(s16)F(z)); }
static void begin(int prim) { cmd(BEGIN_VTXS, (u32)prim); }
static void tri(double x0, double y0, double x1, double y1, double x2, double y2, double z) {
    begin(0); vtx(x0, y0, z); vtx(x1, y1, z); vtx(x2, y2, z); cmd0(END_VTXS);
}
static void quad(double x0, double y0, double x1, double y1, double z) {    /* counter-clockwise from the lower left */
    begin(1); vtx(x0, y0, z); vtx(x1, y0, z); vtx(x1, y1, z); vtx(x0, y1, z); cmd0(END_VTXS);
}
/* SWAP_BUFFERS, VBlank, and the frame drawn */
static void show(u32 swap_bits) {
    cmd(SWAP_BUFFERS, swap_bits);
    gpu3d_line(s, 191);
    gpu3d_render(s);
}
/* a pixel as BG0 shows it: BGR555 | 0x8000, alpha in *a */
static u32 px(int x, int y, u32 *a) {
    u16 line[256]; u8 al[256];
    gpu3d_bg0_line(s, y, line, al);
    if (a) *a = al[x];
    return line[x];
}
#define OPAQUE 0x8000u
static u32 pixel(int x, int y) { return px(x, y, NULL); }
static int near_color(u32 c, u32 want, int tol) {
    for (int k = 0; k < 15; k += 5) if (abs((int)((c >> k) & 31) - (int)((want >> k) & 31)) > tol) return 0;
    return (c & OPAQUE) != 0;
}
/* every pixel of the box [x0, x1) x [y0, y1) is c and no pixel outside it is drawn (the rest being the clear colour) */
static int filled(int x0, int y0, int x1, int y1, u32 c, int *bad_x, int *bad_y) {
    for (int y = 0; y < 192; y++) {
        u16 line[256]; u8 al[256];
        gpu3d_bg0_line(s, y, line, al);
        for (int x = 0; x < 256; x++) {
            int in = x >= x0 && x < x1 && y >= y0 && y < y1;
            if (line[x] != (in ? c : OPAQUE)) { *bad_x = x; *bad_y = y; return 0; }
        }
    }
    return 1;
}

static void test_flat(void) {
    scene();
    cmd(POLYGON_ATTR, SOLID);
    color(31, 0, 0);
    tri(-0.5, -0.5, 0.5, -0.5, 0, 0.5, 0);
    show(0);
    u32 red = OPAQUE | 31, a;
    CHECK(pixel(128, 100) == red, "inside the triangle: %04x", pixel(128, 100));
    CHECK(pixel(75, 138) == red && pixel(180, 138) == red, "near the bottom corners");
    CHECK(pixel(128, 56) == red, "near the apex");
    CHECK(pixel(10, 100) == OPAQUE && pixel(128, 40) == OPAQUE && pixel(128, 150) == OPAQUE && pixel(70, 60) == OPAQUE,
          "outside: %04x %04x %04x", pixel(10, 100), pixel(128, 40), pixel(128, 150));
    px(128, 100, &a);
    CHECK(a == 31, "alpha %u", a);
    u32 z = 0; (void)z;
    /* the rear plane's colour and alpha come through where nothing is drawn */
    px(5, 5, &a);
    CHECK(a == 31, "clear alpha %u", a);
    /* colours: 5 bits widen to 6 as X * 2 + 1 (0 stays 0) and narrow again */
    for (u32 c = 0; c < 32; c += 5) {
        scene();
        cmd(POLYGON_ATTR, SOLID);
        color(c, 31 - c, c / 2);
        quad(-0.5, -0.5, 0.5, 0.5, 0);
        show(0);
        CHECK(pixel(128, 96) == (OPAQUE | rgb5(c, 31 - c, c / 2)), "colour %u: %04x", c, pixel(128, 96));
    }
}

/* The vertices of the polygon just sent are on the corners of the box [x0, x0 + w] x [y0, y0 + h] */
static int bbox_is(int x0, int y0, int w, int h) {
    const G3dList *L = &s->g3d.lists[s->g3d.cur];
    if (!L->npoly) return 0;
    const G3dPoly *P = &L->poly[L->npoly - 1];
    int x1 = 1 << 20, x2 = -(1 << 20), y1 = x1, y2 = x2;
    for (int i = 0; i < P->n; i++) {
        const G3dVertex *v = &L->vtx[P->v[i]];
        if (v->x < x1) x1 = v->x;
        if (v->x > x2) x2 = v->x;
        if (v->y < y1) y1 = v->y;
        if (v->y > y2) y2 = v->y;
    }
    return x1 == x0 && x2 == x0 + w && y1 == y0 && y2 == y0 + h;
}

/* GBATEK: the hardware rounds the width and height of every polygon up to at least 1, so 0x0,
 * 1x0, 0x1 and 1x1 polygons all draw as one pixel. With the viewport below (256 x 128 pixels) a
 * pixel step is exact in the 4.12 coordinates of VTX_16, and the polygons sit on the corners of
 * the pixel (100, 50). */
static void dot_vtx(int px, int py) {
    cmd(VTX_16, (u32)(u16)(s16)(32 * px - 4096) | (u32)(u16)(s16)(4096 - 64 * py) << 16);
    cmd(VTX_16, 0);
}
static void test_one_pixel(void) {
    enum { X = 100, Y = 50 };
    const int cx[4] = {0, 1, 1, 0}, cy[4] = {0, 0, 1, 1};      /* the corners of the pixel, in order round it */
    u32 green = OPAQUE | rgb5(0, 31, 0), blended = OPAQUE | rgb5(0, 16, 0);
    int bx, by;
    for (int translucent = 0; translucent < 2; translucent++) {
        for (int drop = 0; drop < 4; drop++) {          /* the right triangles: three corners of the pixel */
            for (int rev = 0; rev < 2; rev++) {         /* in both windings */
                scene();
                cmd(VIEWPORT, 64u << 8 | 255u << 16 | 191u << 24);
                reg(DISP3DCNT, 8);
                cmd(POLYGON_ATTR, attr(translucent ? 16 : 31, 0, ATTR_FRONT | ATTR_BACK)); color(0, 31, 0);
                begin(0);
                for (int k = 0; k < 3; k++) {
                    int c = (drop + 1 + (rev ? 2 - k : k)) % 4;
                    dot_vtx(X + cx[c], Y + cy[c]);
                }
                CHECK(bbox_is(X, Y, 1, 1), "triangle %d/%d: the vertices are not on the corners of the pixel", drop, rev);
                show(0);
                CHECK(filled(X, Y, X + 1, Y + 1, translucent ? blended : green, &bx, &by),
                      "%s 1x1 triangle (corner %d dropped, winding %d): not exactly its one pixel (%d, %d: %04x)",
                      translucent ? "translucent" : "opaque", drop, rev, bx, by, pixel(bx, by));
            }
        }
    }
    /* a unit quad, and the polygons with no width, no height or neither */
    for (int shape = 0; shape < 4; shape++) {
        static const char *name[4] = {"1x1 quad", "0x0 triangle", "1x0 triangle", "0x1 triangle"};
        scene();
        cmd(VIEWPORT, 64u << 8 | 255u << 16 | 191u << 24);
        cmd(POLYGON_ATTR, attr(31, 0, ATTR_FRONT | ATTR_BACK)); color(0, 31, 0);
        if (shape == 0) { begin(1); dot_vtx(X, Y + 1); dot_vtx(X + 1, Y + 1); dot_vtx(X + 1, Y); dot_vtx(X, Y); }
        else {
            begin(0);
            dot_vtx(X, Y);
            dot_vtx(shape == 3 ? X : X + (shape == 2), Y + (shape == 3));
            dot_vtx(shape == 3 ? X : X + (shape == 2), Y + (shape == 3));
        }
        CHECK(bbox_is(X, Y, shape == 0 || shape == 2, shape == 0 || shape == 3), "%s: the vertices", name[shape]);
        show(0);
        CHECK(filled(X, Y, X + 1, Y + 1, green, &bx, &by), "%s: not exactly its one pixel (%d, %d: %04x)", name[shape], bx, by, pixel(bx, by));
    }
    /* bigger polygons still follow the pixel-centre rule: a 3x3 square covers nine */
    scene();
    cmd(VIEWPORT, 64u << 8 | 255u << 16 | 191u << 24);
    cmd(POLYGON_ATTR, attr(31, 0, ATTR_FRONT | ATTR_BACK)); color(0, 31, 0);
    begin(1); dot_vtx(X, Y + 3); dot_vtx(X + 3, Y + 3); dot_vtx(X + 3, Y); dot_vtx(X, Y);
    show(0);
    CHECK(filled(X, Y, X + 3, Y + 3, green, &bx, &by), "3x3 quad (%d, %d: %04x)", bx, by, pixel(bx, by));
}

static void test_gouraud(void) {
    scene();
    cmd(POLYGON_ATTR, SOLID);
    begin(0);
    color(31, 0, 0); vtx(-0.5, -0.5, 0);
    color(0, 31, 0); vtx(0.5, -0.5, 0);
    color(0, 0, 31); vtx(0, 0.5, 0);
    show(0);
    u32 c = pixel(128, 112);       /* the centroid: a third of each */
    CHECK(near_color(c, rgb5(10, 10, 10), 1), "centroid %u %u %u", c & 31, (c >> 5) & 31, (c >> 10) & 31);
    c = pixel(72, 140);
    CHECK((c & 31) > 24 && ((c >> 5) & 31) < 6 && ((c >> 10) & 31) < 6, "near the red corner: %04x", c);
    c = pixel(184, 140);
    CHECK((c & 31) < 6 && ((c >> 5) & 31) > 24, "near the green corner: %04x", c);
    c = pixel(128, 54);
    CHECK(((c >> 10) & 31) > 24 && (c & 31) < 6, "near the blue corner: %04x", c);
    /* halfway along the bottom edge: red and green in equal parts */
    c = pixel(128, 142);
    CHECK(abs((int)(c & 31) - (int)((c >> 5) & 31)) <= 1 && (c & 31) > 12 && ((c >> 10) & 31) <= 1, "edge midpoint %u %u %u", c & 31, (c >> 5) & 31, (c >> 10) & 31);
    /* a quad's colours too: left red, right blue, top and bottom alike */
    scene();
    cmd(POLYGON_ATTR, SOLID);
    begin(1);
    color(31, 0, 0); vtx(-0.5, -0.5, 0);
    color(0, 0, 31); vtx(0.5, -0.5, 0);
    color(0, 0, 31); vtx(0.5, 0.5, 0);
    color(31, 0, 0); vtx(-0.5, 0.5, 0);
    show(0);
    c = pixel(128, 70);
    CHECK(abs((int)(c & 31) - (int)((c >> 10) & 31)) <= 1 && ((c >> 5) & 31) == 0, "quad centre %04x", c);
    CHECK((pixel(70, 70) & 31) > 26 && ((pixel(70, 70) >> 10) & 31) < 4 && ((pixel(188, 70) >> 10) & 31) > 26 && (pixel(188, 70) & 31) < 4, "quad ends %04x %04x", pixel(70, 70), pixel(188, 70));
    /* 1/w: with perspective the colour is interpolated in 3D, not on the screen: the nearer end dominates */
    scene();
    cmd(MTX_MODE, 0);
    {
        s32 p[16] = {0x1000, 0, 0, 0, 0, 0x1000, 0, 0, 0, 0, -0x1397, -0x1000, 0, 0, -0x2397, 0};   /* near 1, far 10 */
        load(p);
    }
    cmd(MTX_MODE, 1);
    cmd(POLYGON_ATTR, attr(31, 0, ATTR_FRONT | ATTR_BACK));
    begin(0);
    color(31, 0, 0); vtx(-0.5, -1, -2);     /* w 2 */
    color(0, 0, 31); vtx(5, -1, -10);       /* w 10 */
    color(0, 0, 31); vtx(0, 1, -2);
    show(0);
    /* the screen point halfway between the red and blue vertices lies 1/6 of the way along the 3D edge: mostly red */
    int found = 0;
    for (int x = 0; x < 256 && !found; x++) {
        u32 q = pixel(x, 150);
        if ((q & OPAQUE) && x > 140 && x < 200 && (q & 31) > ((q >> 10) & 31) * 2) found = 1;
    }
    CHECK(found, "perspective-correct colour: no reddish pixel on row 150");
}

static void test_assembly(void) {
    int bx, by;
    /* a quad is one polygon of four vertices */
    scene();
    cmd(POLYGON_ATTR, SOLID); color(0, 31, 0);
    quad(-0.5, -0.5, 0.5, 0.5, 0);
    CHECK((get(RAM_COUNT) & 0xFFF) == 1 && (get(RAM_COUNT) >> 16) == 4, "RAM_COUNT %08x", get(RAM_COUNT));
    show(0);
    CHECK(filled(64, 48, 192, 144, OPAQUE | rgb5(0, 31, 0), &bx, &by), "quad fills [64,192) x [48,144): pixel %d,%d is %04x", bx, by, pixel(bx, by));
    CHECK(get(RAM_COUNT) == 0, "RAM_COUNT after the swap %08x", get(RAM_COUNT));
    /* a triangle strip: each second triangle is wound the other way; six vertices cover the square */
    scene();
    cmd(POLYGON_ATTR, SOLID); color(0, 0, 31);
    begin(2);
    vtx(-0.5, 0.5, 0); vtx(-0.5, -0.5, 0); vtx(0, 0.5, 0); vtx(0, -0.5, 0); vtx(0.5, 0.5, 0); vtx(0.5, -0.5, 0);
    CHECK((get(RAM_COUNT) & 0xFFF) == 4 && (get(RAM_COUNT) >> 16) == 6, "strip: %u polygons %u vertices", get(RAM_COUNT) & 0xFFF, get(RAM_COUNT) >> 16);
    show(0);
    CHECK(filled(64, 48, 192, 144, OPAQUE | rgb5(0, 0, 31), &bx, &by), "triangle strip: pixel %d,%d is %04x", bx, by, pixel(bx, by));
    /* the same with a quad strip: two quads from six vertices */
    scene();
    cmd(POLYGON_ATTR, SOLID); color(31, 31, 0);
    begin(3);
    vtx(-0.5, 0.5, 0); vtx(-0.5, -0.5, 0); vtx(0, 0.5, 0); vtx(0, -0.5, 0); vtx(0.5, 0.5, 0); vtx(0.5, -0.5, 0);
    CHECK((get(RAM_COUNT) & 0xFFF) == 2 && (get(RAM_COUNT) >> 16) == 6, "quad strip: %u polygons %u vertices", get(RAM_COUNT) & 0xFFF, get(RAM_COUNT) >> 16);
    show(0);
    CHECK(filled(64, 48, 192, 144, OPAQUE | rgb5(31, 31, 0), &bx, &by), "quad strip: pixel %d,%d is %04x", bx, by, pixel(bx, by));
    /* four triangles and two leftover vertices: only the complete ones count */
    scene();
    cmd(POLYGON_ATTR, SOLID);
    begin(0);
    for (int i = 0; i < 4; i++) { vtx(-0.5, -0.5, 0); vtx(0.5, -0.5, 0); vtx(0, 0.1 * i + 0.2, 0); }
    vtx(0, 0, 0); vtx(0.5, 0, 0);
    CHECK((get(RAM_COUNT) & 0xFFF) == 4 && (get(RAM_COUNT) >> 16) == 12, "separate triangles: %u, %u", get(RAM_COUNT) & 0xFFF, get(RAM_COUNT) >> 16);
    /* a new list drops the half polygon before it: its first vertex does not complete the leftover
     * pair (0, 0), (0.5, 0) into a triangle, and three counter-clockwise vertices make one */
    begin(0);
    vtx(0, 0.2, 0);
    CHECK((get(RAM_COUNT) & 0xFFF) == 4 && (get(RAM_COUNT) >> 16) == 12, "a leftover vertex completed a polygon: %u, %u", get(RAM_COUNT) & 0xFFF, get(RAM_COUNT) >> 16);
    vtx(-0.5, -0.2, 0); vtx(0.5, -0.2, 0);
    CHECK((get(RAM_COUNT) & 0xFFF) == 5 && (get(RAM_COUNT) >> 16) == 15, "BEGIN_VTXS restarted the list: %u, %u", get(RAM_COUNT) & 0xFFF, get(RAM_COUNT) >> 16);
}

static void test_culling(void) {
    /* counter-clockwise is the front: CW is the back, shown only if the attribute says so */
    struct { u32 attr; int ccw; int shown; const char *what; } t[] = {
        {attr(31, 0, ATTR_FRONT), 1, 1, "front, front rendered"},
        {attr(31, 0, ATTR_FRONT), 0, 0, "back, front rendered only"},
        {attr(31, 0, ATTR_BACK), 1, 0, "front, back rendered only"},
        {attr(31, 0, ATTR_BACK), 0, 1, "back, back rendered"},
        {attr(31, 0, 0), 1, 0, "front, neither rendered"},
        {attr(31, 0, ATTR_FRONT | ATTR_BACK), 0, 1, "back, both rendered"},
    };
    for (unsigned i = 0; i < sizeof t / sizeof *t; i++) {
        scene();
        cmd(POLYGON_ATTR, t[i].attr); color(31, 31, 31);
        if (t[i].ccw) tri(-0.5, -0.5, 0.5, -0.5, 0, 0.5, 0); else tri(-0.5, -0.5, 0, 0.5, 0.5, -0.5, 0);
        CHECK((get(RAM_COUNT) & 0xFFF) == (u32)t[i].shown, "%s: stored %u", t[i].what, get(RAM_COUNT) & 0xFFF);
        show(0);
        CHECK((pixel(128, 100) != OPAQUE) == t[i].shown, "%s: pixel %04x", t[i].what, pixel(128, 100));
    }
    /* a line is a triangle with a vertex twice: no front or back, always drawn */
    scene();
    cmd(POLYGON_ATTR, SOLID); color(31, 31, 31);
    tri(-0.5, -0.5, 0.5, 0.5, -0.5, -0.5, 0);
    show(0);
    int hits = 0, strays = 0;
    for (int y = 60; y < 130; y++)
        for (int x = 0; x < 256; x++)
            if (pixel(x, y) != OPAQUE) {
                double ideal = 64 + (144 - (y + 0.5)) * (128.0 / 96.0);
                if (fabs(x + 0.5 - ideal) <= 2.5) hits++; else strays++;
            }
    CHECK(hits >= 70 && strays == 0, "line segment: %d pixels on it, %d beside it", hits, strays);
    /* a line of 0 and 1 pixels still shows: 'at least one pixel' */
    scene();
    cmd(POLYGON_ATTR, SOLID | ATTR_DOT); color(31, 31, 31);
    tri(0, 0, 0.001, 0, 0, 0.001, 0);
    show(0);
    CHECK(pixel(128, 95) != OPAQUE || pixel(128, 96) != OPAQUE || pixel(127, 96) != OPAQUE, "a polygon under a pixel in size vanished");
}

static void test_depth(void) {
    u32 red = OPAQUE | rgb5(31, 0, 0), green = OPAQUE | rgb5(0, 31, 0);
    for (int order = 0; order < 2; order++) {
        scene();
        cmd(POLYGON_ATTR, SOLID);
        for (int k = 0; k < 2; k++) {
            int near_one = (k == 0) == (order == 1);        /* order 0: far red, then near green; order 1: the other way round */
            if (near_one) { color(0, 31, 0); quad(-0.2, -0.2, 0.8, 0.8, -0.5); }
            else { color(31, 0, 0); quad(-0.8, -0.8, 0.2, 0.2, 0.5); }
        }
        show(0);
        CHECK(pixel(128, 96) == green, "order %d: the near quad lost in the overlap: %04x", order, pixel(128, 96));
        CHECK(pixel(48, 150) == red && pixel(200, 40) == green, "order %d: the parts that don't overlap: %04x %04x", order, pixel(48, 150), pixel(200, 40));
    }
    /* Depth Equal (bit 14) draws where the depth matches, within a tolerance, and not where it is farther */
    scene();
    cmd(POLYGON_ATTR, SOLID); color(31, 0, 0); quad(-0.5, -0.5, 0.5, 0.5, 0);
    cmd(POLYGON_ATTR, attr(31, 0, ATTR_FRONT | ATTR_EQUAL)); color(0, 31, 0); quad(-0.25, -0.25, 0.25, 0.25, 0);
    cmd(POLYGON_ATTR, attr(31, 0, ATTR_FRONT | ATTR_EQUAL)); color(0, 0, 31); quad(0.0, 0.0, 0.4, 0.4, 0.5);
    show(0);
    CHECK(pixel(128 - 16, 96 + 16) == green, "equal depth: %04x", pixel(112, 112));
    /* (170, 64) is inside the red and the blue quad but outside the green one */
    CHECK(pixel(170, 64) == red, "a farther polygon with the equal test drew: %04x", pixel(170, 64));
    /* the rear plane's depth: a polygon at the far plane still shows (below CLEAR_DEPTH's 0x7FFF) */
    scene();
    cmd(POLYGON_ATTR, SOLID); color(31, 31, 31);
    quad(-0.5, -0.5, 0.5, 0.5, 1.0);
    show(0);
    CHECK(pixel(128, 96) != OPAQUE, "a polygon at the far plane is hidden");
    /* a clear depth in front of it hides it */
    scene();
    reg(CLEAR_DEPTH, 0x4000);               /* z = 0 */
    cmd(POLYGON_ATTR, SOLID); color(31, 31, 31);
    quad(-0.5, -0.5, 0.5, 0.5, 0.5);
    show(0);
    CHECK(pixel(128, 96) == OPAQUE, "a polygon behind the clear depth was drawn");
    /* Z and W buffering. SWAP_BUFFERS' bits are for the polygons sent after it (GBATEK), so the
     * W bit comes from the swap before the list, not from the one that ends it. With an
     * orthographic projection every w is 1, so with W the first polygon wins. */
    scene();
    cmd(SWAP_BUFFERS, 2); gpu3d_line(s, 191);       /* the list below is built after this swap */
    cmd(POLYGON_ATTR, SOLID); color(31, 0, 0); quad(-0.5, -0.5, 0.5, 0.5, 0.5);
    color(0, 31, 0); quad(-0.5, -0.5, 0.5, 0.5, -0.5);
    show(0);                                        /* its own bits are not W: they are for the next list */
    CHECK(pixel(128, 96) == red, "W-buffer with equal w: %04x", pixel(128, 96));
    /* the converse: no swap before the polygons is Z, whatever bits the swap that ends the list carries */
    scene();
    cmd(POLYGON_ATTR, SOLID); color(31, 0, 0); quad(-0.5, -0.5, 0.5, 0.5, 0.5);
    color(0, 31, 0); quad(-0.5, -0.5, 0.5, 0.5, -0.5);
    show(2);
    CHECK(pixel(128, 96) == green, "a swap's bits applied to the list it ends: %04x", pixel(128, 96));
    /* but with a perspective projection W orders by distance */
    scene();
    cmd(SWAP_BUFFERS, 2); gpu3d_line(s, 191);
    cmd(MTX_MODE, 0);
    {
        s32 p[16] = {0x1000, 0, 0, 0, 0, 0x1000, 0, 0, 0, 0, -0x1397, -0x1000, 0, 0, -0x2397, 0};
        load(p);
    }
    cmd(MTX_MODE, 1);
    cmd(POLYGON_ATTR, attr(31, 0, ATTR_FRONT));
    color(31, 0, 0); quad(-1.5, -1.5, 1.5, 1.5, -6);     /* far */
    color(0, 31, 0); quad(-0.5, -0.5, 0.5, 0.5, -3);     /* near, sent second */
    show(0);
    CHECK(pixel(128, 96) == green, "W-buffer, perspective: %04x", pixel(128, 96));
    CHECK(pixel(100, 96) == red, "W-buffer: the far polygon around it %04x", pixel(100, 96));
}

static void test_clipping(void) {
    /* near plane: a vertex at z = -2 is cut away; the cut runs from (0, -0.5) to (-0.25, 0) */
    scene();
    cmd(POLYGON_ATTR, SOLID); color(31, 31, 31);
    begin(0);
    vtx(-0.5, -0.5, -2); vtx(0.5, -0.5, 0); vtx(0, 0.5, 0);
    CHECK((get(RAM_COUNT) & 0xFFF) == 1 && (get(RAM_COUNT) >> 16) == 4, "near plane: %u polygons, %u vertices", get(RAM_COUNT) & 0xFFF, get(RAM_COUNT) >> 16);
    show(0);
    CHECK(pixel(150, 130) != OPAQUE && pixel(120, 60) != OPAQUE, "the kept part is missing");
    CHECK(pixel(74, 136) == OPAQUE && pixel(100, 125) == OPAQUE, "the cut-away corner is drawn: %04x %04x", pixel(74, 136), pixel(100, 125));
    /* a clipped vertex gets interpolated colour: the new vertices are halfway to the red one */
    scene();
    cmd(POLYGON_ATTR, SOLID);
    begin(0);
    color(31, 0, 0); vtx(-0.5, -0.5, -2);
    color(0, 0, 31); vtx(0.5, -0.5, 0);
    color(0, 0, 31); vtx(0, 0.5, 0);
    show(0);
    u32 c = pixel(130, 142);                /* by the new vertex at (0, -0.5): half red, half blue */
    CHECK(abs((int)(c & 31) - 15) <= 3 && abs((int)((c >> 10) & 31) - 15) <= 3, "colour of a clipped vertex %04x", c);
    /* the screen edges: x = 2 is beyond the right side; the polygon is cut at pixel 255 */
    scene();
    cmd(POLYGON_ATTR, SOLID); color(31, 31, 31);
    tri(0, -0.5, 2, -0.5, 0.5, 0.5, 0);
    CHECK((get(RAM_COUNT) & 0xFFF) == 1 && (get(RAM_COUNT) >> 16) == 4, "side plane: %u polygons, %u vertices", get(RAM_COUNT) & 0xFFF, get(RAM_COUNT) >> 16);
    show(0);
    CHECK(pixel(250, 130) != OPAQUE && pixel(255, 143) != OPAQUE && pixel(200, 100) != OPAQUE && pixel(128, 100) == OPAQUE, "right of the screen");
    CHECK(pixel(128, 60) == OPAQUE, "above the edge's slope");
    /* top and left too: this one runs off the top and left sides, and its corner (0, 0) is inside */
    scene();
    cmd(POLYGON_ATTR, SOLID); color(31, 31, 31);
    tri(-2, 0.5, 0.5, -0.5, 0.5, 2, 0);
    show(0);
    CHECK(pixel(0, 0) != OPAQUE && pixel(10, 40) != OPAQUE && pixel(128, 0) != OPAQUE && pixel(190, 100) != OPAQUE, "the top-left is not covered: %04x", pixel(10, 40));
    CHECK(pixel(200, 100) == OPAQUE && pixel(10, 150) == OPAQUE, "beyond the triangle");
    /* wholly outside: neither stored nor drawn */
    scene();
    cmd(POLYGON_ATTR, SOLID); color(31, 31, 31);
    tri(1.5, -0.5, 2.5, -0.5, 2, 0.5, 0);
    tri(-0.5, -0.5, 0.5, -0.5, 0, 0.5, -3);
    tri(-0.5, -0.5, 0.5, -0.5, 0, 0.5, 3);
    CHECK(get(RAM_COUNT) == 0, "polygons outside the volume were stored: %08x", get(RAM_COUNT));
    /* the far plane: hidden unless POLYGON_ATTR bit 12, then clipped at it */
    scene();
    cmd(POLYGON_ATTR, SOLID);
    begin(0);
    vtx(-0.5, -0.5, 0); vtx(0.5, -0.5, 0); vtx(0, 0.5, 1.5);
    CHECK(get(RAM_COUNT) == 0, "far-plane polygon stored without bit 12");
    cmd(POLYGON_ATTR, SOLID | ATTR_FAR);
    begin(0);
    vtx(-0.5, -0.5, 0); vtx(0.5, -0.5, 0); vtx(0, 0.5, 1.5);        /* one vertex beyond the far plane */
    CHECK((get(RAM_COUNT) & 0xFFF) == 1 && (get(RAM_COUNT) >> 16) == 4, "far-plane polygon with bit 12: %u, %u", get(RAM_COUNT) & 0xFFF, get(RAM_COUNT) >> 16);
    /* a clipped polygon in a strip is stored alone: its vertices aren't shared with the next one */
    scene();
    cmd(POLYGON_ATTR, attr(31, 0, ATTR_FRONT | ATTR_BACK));
    begin(2);
    vtx(-0.5, -0.5, 0); vtx(0.5, -0.5, 0); vtx(0, 0.5, 0);
    CHECK((get(RAM_COUNT) >> 16) == 3, "first strip triangle: %u vertices", get(RAM_COUNT) >> 16);
    vtx(0.9, 0.5, 0);                       /* shares two with the first */
    CHECK((get(RAM_COUNT) & 0xFFF) == 2 && (get(RAM_COUNT) >> 16) == 4, "second strip triangle: %u polygons, %u vertices", get(RAM_COUNT) & 0xFFF, get(RAM_COUNT) >> 16);
    vtx(2.0, 0.5, 0);                       /* the third is clipped: its own vertices */
    CHECK((get(RAM_COUNT) & 0xFFF) == 3 && (get(RAM_COUNT) >> 16) > 5, "clipped strip triangle: %u polygons, %u vertices", get(RAM_COUNT) & 0xFFF, get(RAM_COUNT) >> 16);
}

/* A quad whose edges cross (GBATEK forbids them) can clip to more vertices than a convex one:
 * Sutherland-Hodgman adds a vertex for every crossing edge, up to 42 for a quad. Polygon RAM holds
 * 10 per polygon, so such a quad is dropped; nothing may be written beyond it. */
static void check_polygons(const char *what) {
    const G3dList *L = &s->g3d.lists[s->g3d.cur];
    for (u32 i = 0; i < L->npoly; i++) {
        const G3dPoly *P = &L->poly[i];
        CHECK(P->n >= 3 && P->n <= 10, "%s: polygon %u has %u vertices", what, i, P->n);
        for (int k = 0; k < P->n && k < 10; k++) CHECK(P->v[k] < L->nvtx, "%s: polygon %u: vertex index %u of %u", what, i, P->v[k], L->nvtx);
    }
}
static void test_twisted_clip(void) {
    scene();
    cmd(POLYGON_ATTR, attr(31, 0, ATTR_FRONT | ATTR_BACK));
    begin(1);
    vtx(3, -3, -2); vtx(-3, 1, 0); vtx(3, -3, -3); vtx(1, -1, -1);      /* clips to 11 vertices */
    CHECK(get(RAM_COUNT) == 0, "a twisted quad that clips to 11 vertices was stored: %u polygons, %u vertices", get(RAM_COUNT) & 0xFFF, get(RAM_COUNT) >> 16);
    CHECK(!(get(DISP3DCNT) & 0x2000), "a dropped polygon is not a RAM overflow");
    color(31, 31, 31);
    tri(-0.5, -0.5, 0.5, -0.5, 0, 0.5, 0);                              /* the next polygon is unharmed */
    CHECK((get(RAM_COUNT) & 0xFFF) == 1 && (get(RAM_COUNT) >> 16) == 3, "triangle after it: %u polygons, %u vertices", get(RAM_COUNT) & 0xFFF, get(RAM_COUNT) >> 16);
    check_polygons("after the twisted quad");
    show(0);
    CHECK(pixel(128, 100) != OPAQUE, "the triangle after a dropped quad is not drawn");
    /* quads and quad strips with random corners, a lot of them beyond every plane */
    scene();
    cmd(POLYGON_ATTR, attr(31, 0, ATTR_FRONT | ATTR_BACK | ATTR_FAR));
    u32 seed = 12345;
    for (int batch = 0; batch < 40; batch++) {
        for (int q = 0; q < 100; q++) {
            begin(q & 1 ? 3 : 1);
            for (int k = 0; k < 4 + (q & 1) * 2; k++) {
                double c[3];
                for (int j = 0; j < 3; j++) { seed = seed * 1664525u + 1013904223u; c[j] = (double)((seed >> 8) % 1580) / 100.0 - 7.9; }
                vtx(c[0], c[1], c[2]);
            }
        }
        check_polygons("random quads");
        show(0);
    }
}

static void test_viewport(void) {
    int bx, by;
    scene();
    cmd(VIEWPORT, 64 | 48u << 8 | 191u << 16 | 143u << 24);
    cmd(POLYGON_ATTR, SOLID); color(31, 0, 31);
    quad(-1, -1, 1, 1, 0);
    show(0);
    CHECK(filled(64, 48, 192, 144, OPAQUE | rgb5(31, 0, 31), &bx, &by), "viewport: pixel %d,%d is %04x", bx, by, pixel(bx, by));
}

static void test_vertex_commands(void) {
    /* the screen position of each vertex as stored: VTX_16, VTX_10, VTX_XY/XZ/YZ and VTX_DIFF */
    scene();
    cmd(POLYGON_ATTR, attr(31, 0, ATTR_FRONT | ATTR_BACK));
    begin(0);
    vtx(0.25, -0.25, 0.5);                                  /* (160, 120), z 0.5 */
    cmd(VTX_10, pack3(32, -32, 0));                         /* 6 fractional bits: (0.5, -0.5, 0) */
    cmd(VTX_XY, xy(-0.5, 0.5));                             /* z stays 0 */
    G3dList *L = &s->g3d.lists[s->g3d.cur];
    CHECK(L->npoly == 1 && L->nvtx == 3, "3 vertices, %u polygon", L->npoly);
    CHECK(L->vtx[0].x == 160 && L->vtx[0].y == 120, "VTX_16 at %d,%d", L->vtx[0].x, L->vtx[0].y);
    CHECK(L->vtx[1].x == 192 && L->vtx[1].y == 144, "VTX_10 at %d,%d", L->vtx[1].x, L->vtx[1].y);
    CHECK(L->vtx[2].x == 64 && L->vtx[2].y == 48, "VTX_XY at %d,%d", L->vtx[2].x, L->vtx[2].y);
    CHECK(L->vtx[0].z > L->vtx[1].z && L->vtx[1].z == L->vtx[2].z, "z: %x %x %x", L->vtx[0].z, L->vtx[1].z, L->vtx[2].z);
    begin(0);
    vtx(0, 0, 0.25);
    cmd(VTX_XZ, (u32)(u16)(s16)F(0.5) | (u32)(u16)(s16)F(-0.5) << 16);     /* y stays 0 */
    cmd(VTX_YZ, (u32)(u16)(s16)F(0.5) | (u32)(u16)(s16)F(0.75) << 16);     /* x stays 0.5 */
    L = &s->g3d.lists[s->g3d.cur];
    CHECK(L->vtx[3].x == 128 && L->vtx[3].y == 96, "centre at %d,%d", L->vtx[3].x, L->vtx[3].y);
    CHECK(L->vtx[4].x == 192 && L->vtx[4].y == 96, "VTX_XZ at %d,%d", L->vtx[4].x, L->vtx[4].y);
    CHECK(L->vtx[5].x == 192 && L->vtx[5].y == 48, "VTX_YZ at %d,%d", L->vtx[5].x, L->vtx[5].y);
    /* depth is z from -1..1 spread over 24 bits: 0.25 -> 0xA00000, -0.5 -> 0x400000, 0.75 -> 0xE00000 */
    CHECK(abs(L->vtx[3].z - 0xA00000) < 0x1000 && abs(L->vtx[4].z - 0x400000) < 0x1000 && abs(L->vtx[5].z - 0xE00000) < 0x1000,
          "z with XZ (which also sets z) and YZ: %x %x %x", L->vtx[3].z, L->vtx[4].z, L->vtx[5].z);
    /* VTX_DIFF adds 10-bit signed steps of 1/4096 to the last vertex: +0.0625 is 8 pixels across */
    begin(0);
    vtx(0, 0, 0);
    cmd(VTX_DIFF, pack3(256, -256, 0));
    cmd(VTX_DIFF, pack3(-256, 0, 0));
    L = &s->g3d.lists[s->g3d.cur];
    CHECK(L->vtx[7].x == 136 && L->vtx[7].y == 102, "VTX_DIFF at %d,%d", L->vtx[7].x, L->vtx[7].y);
    CHECK(L->vtx[8].x == 128 && L->vtx[8].y == 102, "second VTX_DIFF at %d,%d", L->vtx[8].x, L->vtx[8].y);
    /* POS_TEST sets the vertex registers: a relative vertex after it starts from its point */
    begin(0);
    vtx(0, 0, 0);
    cmd(POS_TEST, xy(0.5, 0.5)); cmd(POS_TEST, 0);
    cmd(VTX_DIFF, pack3(256, 0, 0));
    vtx(0, 0.5, 0);
    L = &s->g3d.lists[s->g3d.cur];
    CHECK(L->nvtx == 12 && L->vtx[10].x == 200 && L->vtx[10].y == 48, "VTX_DIFF after POS_TEST at %d,%d", L->vtx[10].x, L->vtx[10].y);
}

static void test_alpha(void) {
    u32 a;
    reg(DISP3DCNT, 0);
    scene();
    reg(DISP3DCNT, 8);                      /* alpha blending */
    cmd(POLYGON_ATTR, SOLID); color(31, 0, 0); quad(-0.8, -0.8, 0.2, 0.2, 0.5);
    cmd(POLYGON_ATTR, attr(16, 1, ATTR_FRONT)); color(0, 31, 0); quad(-0.2, -0.2, 0.8, 0.8, 0);
    show(0);
    /* (0 * 17 + 63 * 15) >> 5 = 29 red, (63 * 17) >> 5 = 33 green, in 6 bits: 14 and 16 in 5 */
    u32 c = px(128, 96, &a);
    CHECK(c == (OPAQUE | rgb5(14, 16, 0)) && a == 31, "blend over opaque: %04x alpha %u", c, a);
    c = px(208, 40, &a);                    /* over the black rear plane: (63 * 17) >> 5 = 33, 16 */
    CHECK(c == (OPAQUE | rgb5(0, 16, 0)) && a == 31, "blend over the rear plane: %04x alpha %u", c, a);
    /* blending off: the polygon's colour replaces what's there, its alpha stays for the 2D blend */
    scene();
    reg(DISP3DCNT, 0);
    cmd(POLYGON_ATTR, SOLID); color(31, 0, 0); quad(-0.8, -0.8, 0.2, 0.2, 0.5);
    cmd(POLYGON_ATTR, attr(16, 1, ATTR_FRONT)); color(0, 31, 0); quad(-0.2, -0.2, 0.8, 0.8, 0);
    show(0);
    c = px(128, 96, &a);
    CHECK(c == (OPAQUE | rgb5(0, 31, 0)) && a == 16, "blending off: %04x alpha %u", c, a);
    /* a rear plane with alpha 0: translucent pixels aren't blended with it */
    scene();
    reg(DISP3DCNT, 8);
    reg(CLEAR_COLOR, 0);
    cmd(POLYGON_ATTR, attr(16, 1, ATTR_FRONT)); color(0, 31, 0); quad(-0.5, -0.5, 0.5, 0.5, 0);
    show(0);
    c = px(128, 96, &a);
    CHECK(c == (OPAQUE | rgb5(0, 31, 0)) && a == 16, "over a transparent rear plane: %04x alpha %u", c, a);
    c = px(10, 10, &a);
    CHECK(c == 0 && a == 0, "the transparent rear plane: %04x alpha %u", c, a);
    /* polygon ID: two translucent polygons of one ID blend once; of two IDs, twice. Sent order
     * (the manual-sort bit of the swap before the list), so the result does not depend on how
     * auto-sort orders them */
    for (int ids = 1; ids <= 2; ids++) {
        scene();
        reg(DISP3DCNT, 8);
        cmd(SWAP_BUFFERS, 1); gpu3d_line(s, 191);
        cmd(POLYGON_ATTR, attr(16, 1, ATTR_FRONT)); color(0, 0, 31); quad(-0.8, -0.8, 0.2, 0.2, 0);
        cmd(POLYGON_ATTR, attr(16, ids == 1 ? 1 : 2, ATTR_FRONT)); color(0, 31, 0); quad(-0.2, -0.2, 0.8, 0.8, 0);
        show(0);
        c = px(128, 96, &a);
        u32 want = ids == 1 ? rgb5(0, 0, 16) : rgb5(0, 16, 7);
        CHECK(c == (OPAQUE | want), "%d polygon ID(s): %04x, wanted %04x", ids, c, want);
    }
    /* translucent depth: bit 11 decides whether a translucent pixel updates the depth buffer */
    scene();
    reg(DISP3DCNT, 8);
    cmd(POLYGON_ATTR, attr(16, 1, ATTR_FRONT | ATTR_DEPTH)); color(0, 0, 31); quad(-0.5, -0.5, 0.5, 0.5, -0.5);
    cmd(POLYGON_ATTR, attr(16, 2, ATTR_FRONT)); color(0, 31, 0); quad(-0.5, -0.5, 0.5, 0.5, 0.5);
    show(0);
    c = px(128, 96, &a);
    CHECK(c == (OPAQUE | rgb5(0, 0, 16)), "a farther translucent polygon behind one that set the depth: %04x", c);
    /* alpha test: pixels with alpha at or below ALPHA_TEST_REF are dropped, opaque ones too at 31 */
    scene();
    reg(DISP3DCNT, 4 | 8);
    reg(0x340, 20);
    cmd(POLYGON_ATTR, attr(16, 1, ATTR_FRONT)); color(0, 31, 0); quad(-0.8, -0.8, 0, 0, 0);
    cmd(POLYGON_ATTR, attr(25, 2, ATTR_FRONT)); color(31, 0, 0); quad(0, 0, 0.8, 0.8, 0);
    show(0);
    CHECK(pixel(128 - 50, 96 + 50) == OPAQUE && pixel(128 + 50, 96 - 50) != OPAQUE, "alpha test: %04x %04x", pixel(78, 146), pixel(178, 46));
    reg(0x340, 31);
    show(0);
    scene();
    reg(DISP3DCNT, 4);
    reg(0x340, 31);
    cmd(POLYGON_ATTR, SOLID); color(31, 31, 31); quad(-0.5, -0.5, 0.5, 0.5, 0);
    show(0);
    CHECK(pixel(128, 96) == OPAQUE, "reference 31 hides opaque polygons too");
}

static void test_wire_and_attr(void) {
    /* wire frame (alpha 0): the outline only */
    scene();
    cmd(POLYGON_ATTR, attr(0, 0, ATTR_FRONT)); color(31, 31, 0);
    tri(-0.5, -0.5, 0.5, -0.5, 0, 0.5, 0);
    show(0);
    int left = 0;
    for (int x = 88; x < 98; x++) left += pixel(x, 100) != OPAQUE;
    CHECK(left >= 1 && left <= 3, "left edge of the wire frame: %d pixels on row 100", left);
    CHECK(pixel(128, 100) == OPAQUE && pixel(128, 120) == OPAQUE, "the inside of a wire frame is drawn");
    CHECK(pixel(128, 143) != OPAQUE, "the bottom edge of the wire frame");
    /* POLYGON_ATTR takes effect at BEGIN_VTXS */
    scene();
    cmd(POLYGON_ATTR, attr(31, 0, ATTR_FRONT)); color(31, 31, 31);
    begin(0);
    cmd(POLYGON_ATTR, attr(31, 0, 0));      /* hide: not until the next BEGIN_VTXS */
    vtx(-0.5, -0.5, 0); vtx(0.5, -0.5, 0); vtx(0, 0.5, 0);
    begin(0);
    vtx(-0.5, -0.5, 0.1); vtx(0.5, -0.5, 0.1); vtx(0, 0.5, 0.1);
    CHECK((get(RAM_COUNT) & 0xFFF) == 1, "attribute change within a list: %u polygons", get(RAM_COUNT) & 0xFFF);
}

/* ---- the 3D layer as BG0 of engine A -------------------------------------------------------- */
/* A scene for the 2D compositor: a red quad over x 64..191 and y 48..143 on a transparent rear
 * plane, a text BG1 of green tiles, and a blue backdrop. */
enum { BLDCNT = 0x50, BG0CNT = 0x08, BG1CNT = 0x0A, BG0HOFS = 0x10 };
static u32 argb(u32 c) {                /* BGR555 as the screen shows it */
    u32 r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return 0xFF000000u | (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
}
static u32 screen_px(int x, int y) {
    u32 line[256];
    gpu2d_render_line(s, 0, y, line);
    return line[x];
}
static void layers(u32 bg0cnt, u32 bg1cnt, u32 alpha, u32 bldcnt) {
    scene();
    s->powcnt1 = 0x820F;                /* nds_create leaves the screens off: the boot value */
    reg(CLEAR_COLOR, 0);                /* transparent rear plane: the 2D layers behind show through */
    reg(DISP3DCNT, 8);
    cmd(POLYGON_ATTR, attr(alpha, 1, ATTR_FRONT)); color(31, 0, 0);
    quad(-0.5, -0.5, 0.5, 0.5, 0);
    show(0);
    wr8(&s->arm9, IO + 0x240, 0x81);    /* VRAM bank A: engine A's BG VRAM */
    for (u32 i = 0; i < 512; i++) wr32(&s->arm9, 0x06002000u + i * 4, 0x00010001);   /* a map of tile 1 */
    for (u32 i = 0; i < 16; i++) wr32(&s->arm9, 0x06004040u + i * 4, 0x05050505);    /* tile 1: colour 5 */
    wr16(&s->arm9, 0x05000000, 0x7C00);                 /* the backdrop: blue */
    wr16(&s->arm9, 0x0500000A, 0x03E0);                 /* colour 5: green */
    reg16(BG0CNT, bg0cnt);
    reg16(BG1CNT, bg1cnt | 1u << 2 | 4u << 8 | 1u << 7);  /* tiles at 0x4000, map at 0x2000, 256 colours */
    reg16(BLDCNT, bldcnt);
    reg(0, 1u << 16 | 8 | 0x100 | 0x200);               /* graphics display, 3D as BG0, BG0 and BG1 on */
}

static void test_bg0(void) {
    u32 red = argb(31), green = argb(31 << 5), blue = argb(31 << 10);
    /* BG0 alone over the backdrop, then with BG1 behind it: the quad and what the rear plane leaves */
    layers(0, 3, 31, 0);
    wr16(&s->arm9, IO + 0, 8 | 0x100 | 1u << 16);
    CHECK(screen_px(128, 96) == red && screen_px(10, 10) == blue && screen_px(200, 150) == blue, "BG0 over the backdrop: %08x %08x", screen_px(128, 96), screen_px(10, 10));
    reg(0, 1u << 16 | 8 | 0x100 | 0x200);
    CHECK(screen_px(128, 96) == red && screen_px(10, 10) == green && screen_px(200, 150) == green, "BG0 over BG1: %08x %08x", screen_px(128, 96), screen_px(10, 10));
    /* priority: BG1 in front of the 3D layer when it has the lower number, BG0 first on a tie */
    layers(1, 0, 31, 0);
    CHECK(screen_px(128, 96) == green, "BG1 (priority 0) in front of the 3D layer: %08x", screen_px(128, 96));
    layers(0, 1, 31, 0);
    CHECK(screen_px(128, 96) == red, "the 3D layer (priority 0) in front of BG1: %08x", screen_px(128, 96));
    layers(1, 1, 31, 0);
    CHECK(screen_px(128, 96) == red, "equal priority: BG0 first: %08x", screen_px(128, 96));
    /* BG0HOFS scrolls the 3D layer in a region of 512 pixels, 256 of image and 256 transparent */
    layers(0, 3, 31, 0);
    reg16(BG0HOFS, 64);
    CHECK(screen_px(0, 96) == red && screen_px(127, 96) == red && screen_px(128, 96) == green && screen_px(255, 96) == green, "HOFS 64: %08x %08x %08x", screen_px(0, 96), screen_px(127, 96), screen_px(128, 96));
    reg16(BG0HOFS, 200);                  /* the quad's pixels are 64..191: all scrolled into the left, 0 shows 3D 200 */
    CHECK(screen_px(0, 96) == green && screen_px(100, 96) == green, "HOFS 200: the image's right end is empty: %08x", screen_px(0, 96));
    reg16(BG0HOFS, 500);                  /* 12 transparent pixels, then the image from its left end */
    CHECK(screen_px(11, 96) == green && screen_px(12 + 64, 96) == red && screen_px(12 + 191, 96) == red && screen_px(12 + 192, 96) == green,
          "HOFS 500, wrapped: %08x %08x %08x", screen_px(11, 96), screen_px(76, 96), screen_px(204, 96));
    reg16(BG0HOFS, 0x1C0 | 0xFE00);       /* only the low 9 bits count: 0x1C0 = 448 */
    CHECK(screen_px(64, 96) == green && screen_px(64 + 64, 96) == red, "HOFS is 9 bits: %08x %08x", screen_px(64, 96), screen_px(128, 96));
    /* alpha: BLDCNT effect 1, BG0 first and BG1 second target blends with the 3D alpha, EVA = A/2 and
     * EVB = 16 - A/2 (GBATEK's guess), whatever BLDALPHA says */
    layers(0, 1, 16, 1u << 6 | 1 | 1u << 9);
    reg16(0x52, 3 | 3u << 8);
    /* the 3D engine drew the pixel at alpha 16 without blending it in the rear plane's transparent pixels */
    u32 a;
    px(128, 96, &a);
    CHECK(a == 16, "the 3D alpha %u", a);
    CHECK(screen_px(128, 96) == argb(15 | 15 << 5), "alpha 16 over BG1: %08x, wanted %08x", screen_px(128, 96), argb(15 | 15 << 5));
    layers(0, 1, 8, 1u << 6 | 1 | 1u << 9);
    CHECK(screen_px(128, 96) == argb(31 * 4 / 16 | 31 * 12 / 16 << 5), "alpha 8 over BG1: %08x, wanted %08x", screen_px(128, 96), argb(7 | 23 << 5));
    layers(0, 1, 16, 1u << 6 | 1);      /* BG1 is not a second target: the 3D pixel stays whole */
    CHECK(screen_px(128, 96) == red, "no second target: %08x", screen_px(128, 96));
    layers(0, 1, 16, 1 | 1u << 9);      /* no blend effect selected */
    CHECK(screen_px(128, 96) == red, "no effect: %08x", screen_px(128, 96));
    layers(0, 1, 31, 1u << 6 | 1 | 1u << 9);
    CHECK(screen_px(128, 96) == red, "an opaque 3D pixel is not blended: %08x", screen_px(128, 96));
    layers(0, 1, 16, 1u << 6 | 1 | 1u << 9);
    CHECK(screen_px(10, 10) == green, "a transparent 3D pixel shows BG1: %08x", screen_px(10, 10));
    /* BG0 as the second target blends as in 2D, with EVA and EVB: a sprite-less check with the 3D layer behind BG1 */
    layers(1, 0, 31, 1u << 6 | 1u << 1 | 1u << 8);
    reg16(0x52, 8 | 8u << 8);
    CHECK(screen_px(128, 96) == argb(15 << 5 | 15), "3D layer as the second target: %08x, wanted %08x", screen_px(128, 96), argb(15 | 15 << 5));
}

static void test_limits(void) {
    scene();
    cmd(POLYGON_ATTR, SOLID);
    /* 1600 quads need 6400 vertices: the 1537th does not fit in vertex RAM */
    for (int i = 0; i < 1600; i++) quad(-0.1, -0.1, 0.1, 0.1, 0);
    u32 rc = get(RAM_COUNT);
    CHECK((rc & 0xFFF) == 1536 && (rc >> 16) == 6144, "vertex RAM full: %u polygons, %u vertices", rc & 0xFFF, rc >> 16);
    CHECK(get(DISP3DCNT) & 0x2000, "no overflow flag");
    reg(DISP3DCNT, 0x2000);
    CHECK(!(get(DISP3DCNT) & 0x2000), "writing 1 to bit 13 did not clear it");
    /* a strip of 2060 triangles shares vertices: the polygon list fills first, at 2048 polygons and 2050 vertices */
    scene();
    cmd(POLYGON_ATTR, attr(31, 0, ATTR_FRONT | ATTR_BACK));
    begin(2);
    for (int i = 0; i < 2062; i++) vtx(-0.5 + 0.0001 * (i & 7), (i & 1) ? 0.1 : -0.1, 0);
    rc = get(RAM_COUNT);
    CHECK((rc & 0xFFF) == 2048 && (rc >> 16) == 2050, "polygon RAM full: %u polygons, %u vertices", rc & 0xFFF, rc >> 16);
    CHECK(get(DISP3DCNT) & 0x2000, "polygon overflow flag");
    show(0);
    CHECK(get(RAM_COUNT) == 0, "counts after the swap");
}

static void test_swap(void) {
    scene();
    cmd(POLYGON_ATTR, SOLID); color(31, 31, 31);
    quad(-0.5, -0.5, 0.5, 0.5, 0);
    gpu3d_render(s);
    CHECK(pixel(128, 96) == OPAQUE, "drawn before SWAP_BUFFERS");
    cmd(SWAP_BUFFERS, 0);
    gpu3d_render(s);
    CHECK(pixel(128, 96) == OPAQUE, "drawn before VBlank");
    CHECK((get(RAM_COUNT) & 0xFFF) == 1, "polygon counts before VBlank");
    gpu3d_line(s, 190);
    CHECK((get(RAM_COUNT) & 0xFFF) == 1, "swapped early");
    gpu3d_line(s, 191);
    gpu3d_render(s);
    CHECK(pixel(128, 96) != OPAQUE && get(RAM_COUNT) == 0, "after VBlank");
    /* the frame stays: with no new swap the same list is drawn again */
    gpu3d_render(s);
    CHECK(pixel(128, 96) != OPAQUE, "the list went away without a swap");
    /* a new frame without polygons replaces it */
    cmd(SWAP_BUFFERS, 0);
    gpu3d_line(s, 191);
    gpu3d_render(s);
    CHECK(pixel(128, 96) == OPAQUE, "an empty frame");
}

/* The pair of translucent quads (blue sent first, green second, different IDs) that sorting
 * puts in the other order; the centre pixel shows which way they were drawn. SWAP_BUFFERS' bits
 * are for the polygons sent after it, so only the swap before the list can ask for sent order. */
static u32 pair_pixel(u32 bits_before, u32 bits_after) {
    scene();
    reg(DISP3DCNT, 8);
    cmd(SWAP_BUFFERS, bits_before); gpu3d_line(s, 191);
    cmd(POLYGON_ATTR, attr(16, 1, ATTR_FRONT)); color(0, 0, 31); quad(-0.8, -0.8, 0.2, 0.2, 0);
    cmd(POLYGON_ATTR, attr(16, 2, ATTR_FRONT)); color(0, 31, 0); quad(-0.2, -0.2, 0.8, 0.8, 0);
    show(bits_after);
    return pixel(128, 96);
}
static void test_swap_bits(void) {
    u32 sent = OPAQUE | rgb5(0, 16, 7), sorted = OPAQUE | rgb5(0, 7, 16);
    u32 c = pair_pixel(1, 0);
    CHECK(c == sent, "manual sort from the swap before the list: %04x, wanted %04x", c, sent);
    c = pair_pixel(0, 1);
    CHECK(c == sorted, "the swap that ends a list asked for manual sort of it: %04x, wanted %04x", c, sorted);
    /* the bits last until the next swap: a second list after SWAP_BUFFERS 1 is in sent order too,
     * and one after a swap of 0 is sorted again */
    pair_pixel(1, 0);
    cmd(POLYGON_ATTR, attr(16, 1, ATTR_FRONT)); color(0, 0, 31); quad(-0.8, -0.8, 0.2, 0.2, 0);
    cmd(POLYGON_ATTR, attr(16, 2, ATTR_FRONT)); color(0, 31, 0); quad(-0.2, -0.2, 0.8, 0.8, 0);
    show(1);
    c = pixel(128, 96);
    CHECK(c == sorted, "after a swap of 0 the next list is sorted: %04x, wanted %04x", c, sorted);
    cmd(POLYGON_ATTR, attr(16, 1, ATTR_FRONT)); color(0, 0, 31); quad(-0.8, -0.8, 0.2, 0.2, 0);
    cmd(POLYGON_ATTR, attr(16, 2, ATTR_FRONT)); color(0, 31, 0); quad(-0.2, -0.2, 0.8, 0.8, 0);
    show(0);
    c = pixel(128, 96);
    CHECK(c == sent, "after a swap of 1 the next list is in sent order: %04x, wanted %04x", c, sent);
}

/* Geometry FIFO DMA: 112-word bursts while the FIFO is under half full. A SWAP_BUFFERS in the
 * list halts the engine until VBlank, so the commands behind it pile up and the DMA must stop
 * with words left, then carry on when VBlank lets the queue drain. */
static void test_dma(void) {
    fresh();
    u32 list[1000], n = 0;
    list[n++] = MTX_MODE; list[n++] = 1;
    list[n++] = MTX_IDENTITY;
    for (int i = 0; i < 100; i++) { list[n++] = MTX_TRANS; list[n++] = 1; list[n++] = 0; list[n++] = 0; }
    list[n++] = SWAP_BUFFERS; list[n++] = 0;
    for (int i = 0; i < 100; i++) { list[n++] = MTX_TRANS; list[n++] = 1; list[n++] = 0; list[n++] = 0; }
    memcpy(s->ram + 0x1000, list, n * 4);
    reg(0xD4, 0x02001000u);
    reg(0xD8, IO + GXFIFO);
    reg(0xDC, n | 2u << 21 | 1u << 26 | 7u << 27 | 1u << 30 | 1u << 31);
    s32 m[16];
    clip_mtx(m);
    CHECK(m[12] == 100, "commands before SWAP_BUFFERS ran: %d of 100", m[12]);
    CHECK(s->dma[0][3].cnt & 0x80000000u, "the channel finished with the engine halted");
    CHECK(s->dma[0][3].icount > 0 && s->dma[0][3].icount < n - 2 * 112, "words left: %u of %u", s->dma[0][3].icount, n);
    CHECK(!(gxstat() & (1u << 25)), "the DMA stopped with the FIFO under half full: %08x", gxstat());
    CHECK(!(s->ifl[0] & (IRQ_DMA0 << 3)), "DMA interrupt before the transfer ended");
    gpu3d_line(s, 191);
    CHECK(!(s->dma[0][3].cnt & 0x80000000u), "the channel never finished");
    CHECK(s->ifl[0] & (IRQ_DMA0 << 3), "no DMA interrupt");
    clip_mtx(m);
    CHECK(m[12] == 200, "commands after SWAP_BUFFERS ran: %d of 200", m[12]);
    CHECK((gxstat() >> 16 & 0x1FF) == 0, "FIFO level after the transfer: %u", gxstat() >> 16 & 0x1FF);
    CHECK(rd32(&s->arm9, IO + 0xDC) >> 31 == 0, "DMA3CNT still enabled");
    /* a small list goes in one burst, without waiting for anything */
    fresh();
    cmd(MTX_MODE, 1);
    list[0] = MTX_TRANS; list[1] = 7; list[2] = 0; list[3] = 0;
    memcpy(s->ram + 0x1000, list, 16);
    reg(0xD4, 0x02001000u);
    reg(0xD8, IO + GXFIFO);
    reg(0xDC, 4u | 2u << 21 | 1u << 26 | 7u << 27 | 1u << 31);
    clip_mtx(m);
    CHECK(m[12] == 7 && !(s->dma[0][3].cnt & 0x80000000u) && !(s->ifl[0] & (IRQ_DMA0 << 3)), "small list: %d", m[12]);
}

int main(void) {
    test_matrices();
    test_direction_matrix();
    test_vec_test();
    test_stacks();
    test_fifo();
    test_gxstat();
    test_tests();
    test_dma();
    test_flat();
    test_one_pixel();
    test_gouraud();
    test_assembly();
    test_culling();
    test_depth();
    test_clipping();
    test_twisted_clip();
    test_viewport();
    test_vertex_commands();
    test_alpha();
    test_wire_and_attr();
    test_bg0();
    test_limits();
    test_swap();
    test_swap_bits();
    if (!fails) printf("gpu3d: all checks passed\n");
    return fails != 0;
}
