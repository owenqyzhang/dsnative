/* The high-level BIOS (cpu.c): each call against a reference written here, the compressors as
 * tiny encoders (decompress through the SWI, compare with what went in), the callback variants
 * with the source behind callbacks that read a host buffer at addresses nothing is mapped at.
 * Everything runs on both CPUs where the call exists on both. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "dsr.h"

s32 dsr_run(Cpu *c, s32 cycles);
void dsr_reset(Cpu *c, int arch, u32 entry);

enum { RAM = 0x02000000, SRC = 0x02010000, DST = 0x02020000, INFO = 0x02030000, TMP = 0x02040000, RET = 0x02000F00,
       STREAM = 0x30000000, GUARD = 0xA5, POISON = 0xCC };

static u8 ram[0x80000], hdr[0x1000], low[0x1000], vram[0x10000], dtcm[0x8000], wram7[0x20000];
static Cpu cpu9, cpu7;
static int failures;

static void check(int cond, const char *what, int arch) {
    if (!cond) { printf("ARM%d: %s\n", arch == 5 ? 9 : 7, what); failures++; }
}
#define CHECK(c, cond, what) check(cond, what, (c)->arch)

/* ---- the machine: RAM, a "VRAM" that ignores 8-bit writes, I/O writes logged ---------------------- */
static struct { u32 a, v; int size; } wlog[64];
static int nwlog;

static u32 slow_read(Cpu *c, u32 a, int size) { (void)c; (void)a; (void)size; return 0; }
static void slow_write(Cpu *c, u32 a, u32 v, int size) {
    (void)c;
    if (nwlog < 64) { wlog[nwlog].a = a; wlog[nwlog].v = v; wlog[nwlog].size = size; nwlog++; }
}

static void map(Cpu *c, u32 base, u8 *mem, u32 len, int byte_writes) {
    for (u32 o = 0; o < len; o += 0x1000) {
        c->rp[(base + o) >> PAGE_SHIFT] = mem + o;
        c->wp[(base + o) >> PAGE_SHIFT] = mem + o;
        c->wp8[(base + o) >> PAGE_SHIFT] = byte_writes ? mem + o : 0;
    }
}

/* ---- callbacks: guest code, as C functions the dispatcher finds by address ------------------------- */
static u8 stream[0x4000];
static u32 stream_len;
static struct { int open, close, get8, get32, bad; u32 open_r[3], close_r0, next; s32 open_err, close_err; } cb;

static u32 cb_open(Cpu *c, u32 pc) {
    (void)pc;
    if (!cb.open++) { cb.open_r[0] = c->r[0]; cb.open_r[1] = c->r[1]; cb.open_r[2] = c->r[2]; }
    cb.bad += c->r[0] != STREAM;
    cb.next = STREAM + 4;
    u32 h;
    memcpy(&h, stream, 4);
    c->r[0] = cb.open_err ? (u32)cb.open_err : h;
    return dsr_bx(c, c->r[14]);
}
static u32 cb_close(Cpu *c, u32 pc) {
    (void)pc;
    cb.close++; cb.close_r0 = c->r[0];
    c->r[0] = (u32)cb.close_err;
    return dsr_bx(c, c->r[14]);
}
static u32 cb_get8(Cpu *c, u32 pc) {
    (void)pc;
    cb.get8++; cb.bad += c->r[0] != cb.next; cb.next++;
    u32 o = c->r[0] - STREAM;
    c->r[0] = 0xAB00u | (o < stream_len ? stream[o] : 0xEE);       /* the upper bits are garbage */
    return dsr_bx(c, c->r[14]);
}
static u32 cb_get32(Cpu *c, u32 pc) {
    (void)pc;
    cb.get32++; cb.bad += c->r[0] != cb.next; cb.next += 4;
    u32 o = c->r[0] - STREAM, v = 0;
    if (o + 4 <= stream_len) memcpy(&v, stream + o, 4);
    c->r[0] = v;
    return dsr_bx(c, c->r[14]);
}
/* the same, as Thumb code: its table keys have bit 0 set */
enum { CB_ARM = 0x02001000, CB_THUMB = 0x02002000 };
static const DispatchEntry table[] = {
    {CB_ARM + 0x00, 0, cb_open, 0}, {CB_ARM + 0x10, 0, cb_close, 0}, {CB_ARM + 0x20, 0, cb_get8, 0}, {CB_ARM + 0x30, 0, cb_get32, 0},
    {(CB_THUMB + 0x00) | 1, 0, cb_open, 0}, {(CB_THUMB + 0x10) | 1, 0, cb_close, 0}, {(CB_THUMB + 0x20) | 1, 0, cb_get8, 0},
    {(CB_THUMB + 0x30) | 1, 0, cb_get32, 0},
};
/* the callback structures in guest memory: open, close, get8, get16 (unused), get32 */
static void put_structure(u32 at, u32 base, int thumb, int with_close) {
    u32 t = thumb ? 1 : 0, w[5] = {base + 0x00 + t, with_close ? base + 0x10 + t : 0, base + 0x20 + t, 0, base + 0x30 + t};
    memcpy(ram + (at - RAM), w, sizeof w);
}

/* ---- running a SWI -------------------------------------------------------------------------------- */
static void swi(Cpu *c, u32 num, u32 r0, u32 r1, u32 r2, u32 r3) {
    c->r[0] = r0; c->r[1] = r1; c->r[2] = r2; c->r[3] = r3;
    c->budget = 0; c->stop = 0; c->halted = 0; c->exit_pc = 0xFFFFFFFFu;
    u32 pc = dsr_swi(c, num, RET);
    if (!pc || pc == RET) { c->r[15] = RET; return; }
    c->r[15] = pc;
    c->exit_pc = RET;
    dsr_run(c, 10000000);
    c->stop = 0;
}

static void setup(Cpu *c, int arch) {
    memset(c->r, 0, sizeof c->r);
    c->read_slow = slow_read;
    c->write_slow = slow_write;
    c->table = table;
    c->table_len = sizeof table / sizeof table[0];
    dsr_reset(c, arch, RAM);
    map(c, RAM, ram, sizeof ram, 1);
    map(c, 0x027FF000, hdr, sizeof hdr, 1);
    map(c, 0, low, sizeof low, 1);
    if (arch == 5) map(c, 0x06000000, vram, sizeof vram, 0);
    if (arch == 5) map(c, 0x00800000, dtcm, sizeof dtcm, 1);       /* where SoftReset's ARM9 stacks are (the default DTCM) */
    else map(c, 0x03800000, wram7, sizeof wram7, 1);
}

/* ---- reference encoders --------------------------------------------------------------------------- */
static void header(u8 *o, u32 type, u32 n) { o[0] = (u8)type; o[1] = (u8)n; o[2] = (u8)(n >> 8); o[3] = (u8)(n >> 16); }

/* LZ77: greedy longest match, distances from min_disp (the 16-bit variants can't copy from the
 * byte just written) */
static u32 lz_encode(const u8 *in, u32 n, u8 *out, u32 min_disp) {
    u32 o = 4, i = 0;
    header(out, 0x10, n);
    while (i < n) {
        u32 fpos = o++;
        u8 flags = 0;
        for (int b = 0; b < 8 && i < n; b++) {
            u32 best = 0, bd = 0;
            for (u32 d = min_disp; d <= 4096 && d <= i; d++) {
                u32 l = 0;
                while (l < 18 && i + l < n && in[i + l - d] == in[i + l]) l++;
                if (l > best) { best = l; bd = d; }
            }
            if (best >= 3) {
                flags |= 0x80 >> b;
                out[o++] = (u8)(((best - 3) << 4) | ((bd - 1) >> 8));
                out[o++] = (u8)(bd - 1);
                i += best;
            } else out[o++] = in[i++];
        }
        out[fpos] = flags;
    }
    while (o & 3) out[o++] = 0;
    return o;
}

static u32 rl_encode(const u8 *in, u32 n, u8 *out) {
    u32 o = 4, i = 0;
    header(out, 0x30, n);
    while (i < n) {
        u32 r = 1;
        while (i + r < n && r < 130 && in[i + r] == in[i]) r++;
        if (r >= 3) { out[o++] = (u8)(0x80 | (r - 3)); out[o++] = in[i]; i += r; continue; }
        u32 l = 0;
        while (i + l < n && l < 128) {
            u32 q = 1;
            while (i + l + q < n && q < 3 && in[i + l + q] == in[i + l]) q++;
            if (q >= 3) break;
            l++;
        }
        out[o++] = (u8)(l - 1);
        for (u32 k = 0; k < l; k++) out[o++] = in[i++];
    }
    while (o & 3) out[o++] = 0;
    return o;
}

/* Huffman over 4- or 8-bit units, the low unit of a byte first. The tree is laid out breadth
 * first, children in pairs right after the parents, as GBATEK's offsets need. */
static u32 huff_encode(const u8 *in, u32 n, int bits, u8 *out) {
    int nsym = 1 << bits;
    u32 count[256] = {0}, weight[512] = {0};
    int child[512][2], parent_of[512], nn = nsym;
    for (u32 i = 0; i < n; i++) {
        if (bits == 8) count[in[i]]++;
        else { count[in[i] & 15]++; count[in[i] >> 4]++; }
    }
    int live[512], nlive = 0;
    for (int s = 0; s < nsym; s++) { weight[s] = count[s]; parent_of[s] = -1; if (count[s]) live[nlive++] = s; }
    if (nlive == 1) { int d = live[0] ? 0 : 1; weight[d] = 1; live[nlive++] = d; }
    while (nlive > 1) {
        int a = 0, b = 1;
        if (weight[live[b]] < weight[live[a]]) { a = 1; b = 0; }
        for (int k = 2; k < nlive; k++) {
            if (weight[live[k]] < weight[live[a]]) { b = a; a = k; }
            else if (weight[live[k]] < weight[live[b]]) b = k;
        }
        int x = live[a], y = live[b];
        child[nn][0] = x; child[nn][1] = y; weight[nn] = weight[x] + weight[y];
        parent_of[x] = parent_of[y] = nn; parent_of[nn] = -1;
        int keep = 0;
        for (int k = 0; k < nlive; k++) if (k != a && k != b) live[keep++] = live[k];
        live[keep++] = nn++;
        nlive = keep;
    }
    int root = live[0];
    u8 tree[1024] = {0};
    int queue[512], qpos[512], qh = 0, qt = 0, next_pair = 2;
    queue[qt] = root; qpos[qt++] = 1;
    while (qh < qt) {
        int node = queue[qh], p = qpos[qh++], q = next_pair;
        next_pair += 2;
        u32 off = (u32)(q - (p & ~1) - 2) / 2;
        if (off > 63) { printf("huffman test tree too wide\n"); failures++; return 0; }
        u8 v = (u8)off;
        for (int k = 0; k < 2; k++) {
            int ch = child[node][k];
            if (ch < nsym) { tree[q + k] = (u8)ch; v |= k ? 0x40 : 0x80; }
            else { queue[qt] = ch; qpos[qt++] = q + k; }
        }
        tree[p] = v;
    }
    int tsz = next_pair;
    if (tsz & 2) tsz += 2;      /* the bit stream starts on a word */
    tree[0] = (u8)(tsz / 2 - 1);
    /* codes: walk up from each leaf */
    u32 code[256], len[256];
    for (int s = 0; s < nsym; s++) {
        code[s] = len[s] = 0;
        if (!weight[s]) continue;
        u32 bitsv = 0, l = 0;
        for (int x = s; parent_of[x] >= 0; x = parent_of[x]) { bitsv |= (u32)(child[parent_of[x]][1] == x) << l; l++; }
        code[s] = bitsv;        /* the root's edge is its top bit */
        len[s] = l;
    }
    header(out, 0x20 | bits, n);
    memcpy(out + 4, tree, (size_t)tsz);
    u32 o = 4 + (u32)tsz, word = 0, nb = 0, units = bits == 8 ? n : n * 2;
    for (u32 u = 0; u < units; u++) {
        u32 s = bits == 8 ? in[u] : (u & 1 ? in[u / 2] >> 4 : in[u / 2] & 15);
        for (u32 k = 0; k < len[s]; k++) {
            word |= ((code[s] >> (len[s] - 1 - k)) & 1) << (31 - nb);
            if (++nb == 32) { memcpy(out + o, &word, 4); o += 4; word = 0; nb = 0; }
        }
    }
    if (nb) { memcpy(out + o, &word, 4); o += 4; }
    return o;
}

/* ---- test data ------------------------------------------------------------------------------------ */
static u32 rng_state = 12345;
static u32 rng(void) { rng_state = rng_state * 1664525u + 1013904223u; return rng_state >> 8; }

/* kind 0: noise, 1: repeats and runs, 2: text-like, 3: one long run */
static u32 make_data(u8 *d, u32 n, int kind) {
    for (u32 i = 0; i < n; i++) {
        switch (kind) {
        case 0: d[i] = (u8)rng(); break;
        case 1: d[i] = i < 20 || rng() % 9 == 0 ? (u8)rng() : (rng() % 5 ? d[i - 1 - (i % 3)] : d[i - 16]); break;
        case 2: d[i] = (u8)("the quick brown fox jumps over the lazy dog. "[(i + (i / 45) % 3) % 45]); break;
        default: d[i] = 0x7E; break;
        }
    }
    return n;
}

static int eq(const u8 *a, const u8 *b, u32 n) { return memcmp(a, b, n) == 0; }

/* ---- the tests ------------------------------------------------------------------------------------ */
static void test_decompress_memory(Cpu *c) {
    static const u32 sizes[] = {1, 3, 64, 1000, 1001, 3001};
    u8 data[4096], enc[8192];
    for (int kind = 0; kind < 4; kind++)
        for (unsigned s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
            u32 n = make_data(data, sizes[s], kind);
            for (int rl = 0; rl < 2; rl++) {
                u32 m = rl ? rl_encode(data, n, enc) : lz_encode(data, n, enc, 1);
                memcpy(ram + (SRC - RAM), enc, m);
                memset(ram + (DST - RAM), POISON, n + 8);
                swi(c, rl ? 0x14 : 0x11, SRC, DST, 0, 0);
                CHECK(c, eq(ram + (DST - RAM), data, n), rl ? "RLUnComp (SWI 14h) output differs" : "LZ77UnComp (SWI 11h) output differs");
                CHECK(c, ram[DST - RAM + n] == POISON, "a Normal decompression wrote past its end");
            }
        }
    /* the 8-bit variants do not reach VRAM on the ARM9: byte writes are dropped there */
    if (c->arch == 5) {
        u32 n = make_data(data, 100, 1), m = lz_encode(data, n, enc, 1);
        memcpy(ram + (SRC - RAM), enc, m);
        memset(vram, POISON, 64);
        swi(c, 0x11, SRC, 0x06000000, 0, 0);
        CHECK(c, vram[0] == POISON && vram[50] == POISON, "an 8-bit decompression wrote to VRAM");
    }
}

static void put_stream(const u8 *enc, u32 m) { memcpy(stream, enc, m); stream_len = m; }

/* swi `num` over the callbacks; returns r0 */
static u32 swi_cb(Cpu *c, u32 num, u32 dst, u32 param, int thumb, int with_close) {
    u32 sp = 0x02050000;
    put_structure(sp, thumb ? CB_THUMB : CB_ARM, thumb, with_close);
    memset(&cb, 0, sizeof cb);
    c->r[12] = 0x1212; c->r[14] = 0x14141415;
    u32 t0 = c->t = thumb;
    swi(c, num, STREAM, dst, param, sp);
    CHECK(c, c->r[15] == RET && c->t == t0, "the call did not return to its caller in its state");
    CHECK(c, c->r[12] == 0x1212 && c->r[14] == 0x14141415, "the caller's r12 or lr was lost across the callbacks");
    CHECK(c, c->r[1] == dst && c->r[2] == param && c->r[3] == sp, "the caller's r1-r3 changed");
    CHECK(c, !cb.bad, "a callback was called with an unexpected source address");
    return c->r[0];
}

static void test_decompress_callbacks(Cpu *c, int thumb) {
    static const u32 sizes[] = {2, 64, 1000, 1001, 2999};
    u8 data[4096] = {0}, enc[8192];
    for (int kind = 0; kind < 4; kind++)
        for (unsigned s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
            u32 n = make_data(data, sizes[s], kind);
            for (int which = 0; which < 3; which++) {      /* LZ77 (12h), Huffman 8 and 4 bits (13h), RLE (15h) */
                u32 m, num = which == 0 ? 0x12 : which == 1 ? 0x13 : 0x15;
                int bits = 0;
                if (which == 0) m = lz_encode(data, n, enc, 2);
                else if (which == 1) {
                    bits = (kind & 1) ? 4 : 8;
                    u8 small[4096];
                    for (u32 i = 0; i < n; i++) small[i] = bits == 8 ? (u8)(data[i] % 11 * 3) : data[i];
                    memcpy(data, small, n);
                    m = huff_encode(data, (n + 3) & ~3u, bits, enc);     /* whole words: the BIOS writes words */
                    enc[1] = (u8)n; enc[2] = (u8)(n >> 8);                 /* but the size in the header is n */
                    if (!m) return;
                } else m = rl_encode(data, n, enc);
                put_stream(enc, m);
                memset(ram + (DST - RAM), POISON, n + 8);
                memset(ram + (TMP - RAM), 0, 0x200);
                u32 r0 = swi_cb(c, num, DST, which == 1 ? TMP : 0x5A5A, thumb, 1);
                char what[96];
                snprintf(what, sizeof what, "SWI %02Xh (kind %d, size %u): output differs", num, kind, n);
                CHECK(c, eq(ram + (DST - RAM), data, n), what);
                CHECK(c, r0 == n, "the decompressed length is not returned in r0");
                CHECK(c, cb.open == 1 && cb.close == 1 && cb.close_r0 == cb.next, "Open once, Close once with the final source address");
                CHECK(c, which == 1 || (cb.open_r[1] == DST && cb.open_r[2] == 0x5A5A), "Open did not get the destination and the parameter in r1 and r2");
                CHECK(c, which != 1 || (cb.open_r[1] == DST && cb.open_r[2] == TMP), "Huffman's Open did not get the temp buffer in r2");
                if (which != 1) CHECK(c, ram[DST - RAM + n] == POISON, "the 16-bit writes clobbered the byte after the end");
                if (which == 1) {
                    CHECK(c, cb.get32 > 0 && eq(ram + (TMP - RAM), enc + 4, (u32)(enc[4] + 1) * 2), "the Huffman tree was not copied into the temp buffer");
                    CHECK(c, ram[DST - RAM + ((n + 3) & ~3u)] == POISON, "Huffman wrote past its last word");
                }
            }
        }
    /* Close is optional */
    u32 n = make_data(data, 300, 1), m = lz_encode(data, n, enc, 2);
    put_stream(enc, m);
    memset(ram + (DST - RAM), POISON, n + 8);
    u32 r0 = swi_cb(c, 0x12, DST, 0, thumb, 0);
    CHECK(c, r0 == n && eq(ram + (DST - RAM), data, n) && cb.close == 0, "decompressing without a Close callback");
    /* errors: Open's or Close's negative code is the result */
    memset(ram + (DST - RAM), POISON, n + 8);
    memset(&cb, 0, sizeof cb);
    put_structure(0x02050000, thumb ? CB_THUMB : CB_ARM, thumb, 1);
    cb.open_err = -5;
    c->t = thumb;
    swi(c, 0x12, STREAM, DST, 0, 0x02050000);
    CHECK(c, (s32)c->r[0] == -5 && cb.get8 == 0 && cb.close == 0 && ram[DST - RAM] == POISON, "Open's error code did not abort the call");
    memset(&cb, 0, sizeof cb);
    cb.close_err = -7;
    c->t = thumb;
    swi(c, 0x12, STREAM, DST, 0, 0x02050000);
    CHECK(c, (s32)c->r[0] == -7 && eq(ram + (DST - RAM), data, n), "Close's error code is not returned");
}

/* the 16-bit variants write VRAM, which drops byte writes */
static void test_decompress_vram(Cpu *c) {
    u8 data[1000], enc[4096];
    u32 n = make_data(data, 1000, 1), m = lz_encode(data, n, enc, 2);
    put_stream(enc, m);
    memset(vram, POISON, 2048);
    swi_cb(c, 0x12, 0x06000000, 0, 0, 1);
    CHECK(c, eq(vram, data, n) && vram[n] == POISON, "LZ77 by callback into VRAM");
    m = rl_encode(data, n, enc);
    put_stream(enc, m);
    memset(vram, POISON, 2048);
    swi_cb(c, 0x15, 0x06000000, 0, 0, 1);
    CHECK(c, eq(vram, data, n), "run-length by callback into VRAM");
}

static void test_bitunpack(Cpu *c) {
    static const struct { u32 sw, dw, off; int zero; u32 len; } cases[] = {
        {1, 4, 0, 0, 8}, {1, 8, 0x10, 0, 16}, {1, 8, 1, 1, 12}, {2, 8, 0, 0, 20}, {2, 8, 3, 1, 8}, {4, 16, 0, 0, 24},
        {4, 8, 0x40, 0, 32}, {8, 32, 0x100, 0, 12}, {1, 32, 0x7FFF, 0, 4}, {8, 16, 0, 1, 10}, {1, 1, 0, 0, 4}, {2, 4, 1, 0, 16},
    };
    for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        u8 src[64];
        u32 sw = cases[k].sw, dw = cases[k].dw, len = cases[k].len;
        for (u32 i = 0; i < len; i++) src[i] = (u8)rng();
        u32 off = cases[k].off | (cases[k].zero ? 0x80000000u : 0);
        memcpy(ram + (SRC - RAM), src, len);
        u8 info[8] = {(u8)len, (u8)(len >> 8), (u8)sw, (u8)dw, (u8)off, (u8)(off >> 8), (u8)(off >> 16), (u8)(off >> 24)};
        memcpy(ram + (INFO - RAM), info, 8);
        memset(ram + (DST - RAM), POISON, 512);
        swi(c, 0x10, SRC, DST, INFO, 0);
        /* reference: a bit string, units from the low bits of each byte */
        u32 want[64], nw = 0, nb = 0;
        u64 acc = 0;
        for (u32 i = 0; i < len; i++)
            for (u32 u = 0; u < 8 / sw; u++) {
                u32 v = (src[i] >> (u * sw)) & ((1u << sw) - 1);
                if (v || cases[k].zero) v += cases[k].off;
                acc |= (u64)(v & (dw == 32 ? 0xFFFFFFFFu : (1u << dw) - 1)) << nb;
                nb += dw;
                if (nb >= 32) { want[nw++] = (u32)acc; acc >>= 32; nb -= 32; }
            }
        u32 got[64];
        memcpy(got, ram + (DST - RAM), nw * 4);
        char what[64];
        snprintf(what, sizeof what, "BitUnPack %u to %u bits, offset %x%s", sw, dw, cases[k].off, cases[k].zero ? " with zero flag" : "");
        CHECK(c, nb == 0 && memcmp(got, want, nw * 4) == 0, what);
        CHECK(c, ram[DST - RAM + nw * 4] == POISON, "BitUnPack wrote past its output");
    }
}

static void test_unfilter(Cpu *c) {
    u8 data[600], enc[700];
    for (int wide = 0; wide < 2; wide++) {
        u32 n = 600, unit = wide ? 2 : 1;
        for (u32 i = 0; i < n; i++) data[i] = (u8)(rng() % 7 ? i * 3 : rng());
        header(enc, 0x80 | unit, n);
        u32 prev = 0;
        for (u32 i = 0; i < n; i += unit) {
            u32 v = wide ? (u32)(data[i] | data[i + 1] << 8) : data[i];
            u32 d = (v - prev) & (wide ? 0xFFFF : 0xFF);
            enc[4 + i] = (u8)d;
            if (wide) enc[4 + i + 1] = (u8)(d >> 8);
            prev = v;
        }
        memcpy(ram + (SRC - RAM), enc, 4 + n);
        memset(ram + (DST - RAM), POISON, n + 8);
        swi(c, wide ? 0x18 : 0x16, SRC, DST, 0, 0);
        CHECK(c, eq(ram + (DST - RAM), data, n) && ram[DST - RAM + n] == POISON, wide ? "Diff16bitUnFilter" : "Diff8bitUnFilter");
    }
}

static void test_copy(Cpu *c) {
    u32 *src = (u32 *)(void *)(ram + (SRC - RAM)), *dst = (u32 *)(void *)(ram + (DST - RAM));
    for (u32 i = 0; i < 64; i++) src[i] = 0x01010101u * (i + 1) ^ rng();
    /* CpuFastSet takes any word count on the NDS, not multiples of 8 as on the GBA */
    for (u32 n = 1; n <= 20; n += 3) {
        memset(dst, GUARD, 0x200);
        swi(c, 0x0C, SRC, DST, n, 0);
        CHECK(c, memcmp(dst, src, n * 4) == 0 && ((u8 *)dst)[n * 4] == GUARD, "CpuFastSet copy of a word count that is not a multiple of 8");
        memset(dst, GUARD, 0x200);
        swi(c, 0x0C, SRC, DST, n | 1u << 24, 0);
        int ok = 1;
        for (u32 i = 0; i < n; i++) ok &= dst[i] == src[0];
        CHECK(c, ok && ((u8 *)dst)[n * 4] == GUARD, "CpuFastSet fill");
    }
    /* CpuSet: halfwords and words, copy and fill */
    for (u32 n = 1; n <= 9; n += 4) {
        memset(dst, GUARD, 0x200);
        swi(c, 0x0B, SRC, DST, n, 0);
        CHECK(c, memcmp(dst, src, n * 2) == 0 && ((u8 *)dst)[n * 2] == GUARD, "CpuSet halfword copy");
        memset(dst, GUARD, 0x200);
        swi(c, 0x0B, SRC, DST, n | 1u << 26, 0);
        CHECK(c, memcmp(dst, src, n * 4) == 0 && ((u8 *)dst)[n * 4] == GUARD, "CpuSet word copy");
        memset(dst, GUARD, 0x200);
        swi(c, 0x0B, SRC, DST, n | 1u << 24, 0);
        int ok = 1;
        for (u32 i = 0; i < n; i++) ok &= ((u16 *)(void *)dst)[i] == (u16)src[0];
        CHECK(c, ok && ((u8 *)dst)[n * 2] == GUARD, "CpuSet halfword fill");
        memset(dst, GUARD, 0x200);
        swi(c, 0x0B, SRC, DST, n | 1u << 24 | 1u << 26, 0);
        ok = 1;
        for (u32 i = 0; i < n; i++) ok &= dst[i] == src[0];
        CHECK(c, ok && ((u8 *)dst)[n * 4] == GUARD, "CpuSet word fill");
    }
    /* the ARM7 will not read its BIOS (below 4000h; page 0 is mapped on both CPUs so that only the
     * rule differs); the ARM9 has no such protection */
    for (int i = 0; i < 64; i++) low[0x100 + i] = (u8)(0x40 + i);
    for (int fast = 0; fast < 2; fast++) {
        memset(dst, GUARD, 0x200);
        swi(c, fast ? 0x0C : 0x0B, 0x100, DST, 16 | (fast ? 0 : 1u << 26), 0);
        if (c->arch == 5) CHECK(c, ((u8 *)dst)[0] == 0x40 && ((u8 *)dst)[63] == 0x7F, "the ARM9 copy from low memory");
        else CHECK(c, ((u8 *)dst)[0] == GUARD, "the ARM7 copied from its BIOS area");
    }
    memset(dst, GUARD, 0x200);          /* an end that wraps round into the BIOS counts too */
    swi(c, 0x0B, 0xFFFFFFF8u, DST, 4 | 1u << 26, 0);
    CHECK(c, ((u8 *)dst)[0] == (c->arch == 5 ? 0 : GUARD), "CpuSet with a source whose end wraps into the BIOS area");
}

/* GBATEK's recipe, as written there: eight constants, a register wider than 16 bits */
static u32 crc_gbatek(u32 crc, const u8 *p, u32 n) {
    static const u32 val[8] = {0xC0C1, 0xC181, 0xC301, 0xC601, 0xCC01, 0xD801, 0xF001, 0xA001};
    for (u32 i = 0; i < n; i++) {
        crc ^= p[i];
        for (int j = 0; j < 8; j++) {
            u32 carry = crc & 1;
            crc >>= 1;
            if (carry) crc ^= val[j] << (7 - j);
        }
    }
    return crc & 0xFFFF;
}

static void test_crc(Cpu *c) {
    u8 data[300];
    for (u32 i = 0; i < sizeof data; i++) data[i] = (u8)rng();
    memcpy(ram + (SRC - RAM), data, sizeof data);
    static const u32 lens[] = {2, 4, 30, 256, 300};
    for (unsigned k = 0; k < 5; k++)
        for (int init = 0; init < 2; init++) {
            u32 seed = init ? 0xFFFF : 0;
            swi(c, 0x0E, seed, SRC, lens[k], 0xDEAD);
            u32 last = data[lens[k] - 2] | data[lens[k] - 1] << 8;
            CHECK(c, c->r[0] == crc_gbatek(seed, data, lens[k]), "GetCRC16 differs from GBATEK's algorithm");
            CHECK(c, c->r[3] == last, "GetCRC16 does not leave the last halfword in r3");
        }
    swi(c, 0x0E, 0xFFFF, SRC, 0, 0xDEAD);
    CHECK(c, c->r[0] == 0xFFFF && c->r[3] == 0xDEAD, "GetCRC16 of nothing");
}

static void test_math(Cpu *c) {
    static const s32 ns[] = {0, 1, -1, 7, -7, 1234, -1234, 100000, 0x7FFFFFFF, (s32)0x80000000, (s32)0x80000001};
    static const s32 ds[] = {1, -1, 2, 10, -10, 3, 65536, 0x7FFFFFFF, (s32)0x80000000};
    for (unsigned a = 0; a < sizeof ns / sizeof ns[0]; a++)
        for (unsigned b = 0; b < sizeof ds / sizeof ds[0]; b++) {
            s64 n = ns[a], d = ds[b], q = n / d, r = n % d;
            swi(c, 0x09, (u32)ns[a], (u32)ds[b], 0, 0);
            CHECK(c, c->r[0] == (u32)q && c->r[1] == (u32)r && c->r[3] == (u32)(q < 0 ? -q : q), "Div differs from 64-bit division");
        }
    for (u32 v = 0; v < 70000; v += v < 300 ? 1 : 997) {
        swi(c, 0x0D, v, 0, 0, 0);
        CHECK(c, c->r[0] == (u32)floor(sqrt((double)v)), "Sqrt");
    }
    static const u32 big[] = {0xFFFFFFFFu, 0x80000000u, 0x7FFFFFFFu, 0xFFFE0001u, 0xFFFE0000u};
    for (unsigned k = 0; k < 5; k++) {
        swi(c, 0x0D, big[k], 0, 0, 0);
        CHECK(c, c->r[0] == (u32)floor(sqrt((double)big[k])), "Sqrt of a large number");
    }
}

static void test_arm7_tables(Cpu *c) {
    for (u32 i = 0; i < 64; i++) {
        swi(c, 0x1A, i, 0, 0, 0);
        CHECK(c, c->r[0] == (u32)floor(32767.0 * sin(i * M_PI / 128) + 0.5), "GetSineTable differs from 7FFFh * sin");
    }
    swi(c, 0x1A, 63, 0, 0, 0);
    CHECK(c, c->r[0] == 0x7FF5, "GetSineTable's last entry is not GBATEK's 7FF5h");
    swi(c, 0x1A, 0, 0, 0, 0);
    CHECK(c, c->r[0] == 0, "GetSineTable(0)");
}

static void test_reset_and_halt(Cpu *c) {
    int a9 = c->arch == 5;
    /* entry points the header copy in main RAM holds; ARM bit 0 clear, Thumb bit 0 set */
    u32 entry = a9 ? 0x02000801u : 0x02000900u;
    memcpy(hdr + (a9 ? 0xE24 : 0xE34), &entry, 4);
    u32 zero = a9 ? 0x00803E00u : 0x0380FE00u;
    u8 *area = a9 ? dtcm + (zero & 0x3FFF) : wram7 + (zero & 0xFFFF);
    memset(area - 8, 0xFF, 0x200 + 16);
    for (int i = 0; i < 16; i++) c->r[i] = 0xD0000000u + i;
    dsr_set_cpsr(c, 0x12, 0xFF);        /* in IRQ mode, interrupts on */
    c->r[13] = 0x11111111; c->r[14] = 0x22222222; c->spsr = 0x33333333;
    u32 pc = dsr_swi(c, 0x00, RET);
    CHECK(c, (a9 ? pc == 0x02000800u && c->t == 1 : pc == 0x02000900u && c->t == 0), "SoftReset did not jump to the header's entry point in its state");
    CHECK(c, (dsr_cpsr(c) & 0xDF) == (0x1F | 0xC0), "SoftReset did not enter system mode with interrupts off");
    int zeros = 1;
    for (int i = 0; i < 13; i++) zeros &= c->r[i] == 0;
    CHECK(c, zeros, "SoftReset left a register in r0-r12 non-zero");
    CHECK(c, c->r[14] == entry, "SoftReset's lr is not the entry address");
    CHECK(c, c->r[13] == (a9 ? 0x00803EC0u : 0x0380FF00u), "SoftReset's system stack");
    int cleared = 1, kept = 1;
    for (int i = 0; i < 0x200; i++) cleared &= area[i] == 0;
    for (int i = 1; i <= 8; i++) kept &= area[-i] == 0xFF && area[0x200 + i - 1] == 0xFF;
    CHECK(c, cleared && kept, "SoftReset must clear exactly the 200h bytes");
    dsr_set_cpsr(c, 0xD3, 0xFF);
    CHECK(c, c->r[13] == (a9 ? 0x00803FC0u : 0x0380FFDCu) && c->r[14] == 0 && c->spsr == 0, "SoftReset's svc stack, lr and spsr");
    dsr_set_cpsr(c, 0xD2, 0xFF);
    CHECK(c, c->r[13] == (a9 ? 0x00803FA0u : 0x0380FFB0u) && c->r[14] == 0 && c->spsr == 0, "SoftReset's irq stack, lr and spsr");
    if (a9) CHECK(c, dsr_cp15_read(c, 1, 0, 0) == 0x12078 && c->bios_base == 0xFFFF0000u, "SoftReset's CP15 control value");

    /* Halt: the ARM9 goes through CP15 and loses r0 */
    c->r[0] = 77; c->halted = 0;
    swi(c, 0x06, 77, 0, 0, 0);
    CHECK(c, c->halted == 1 && (!a9 || c->r[0] == 0) && (a9 || c->r[0] == 77), "Halt");
    /* WaitByLoop: r0 iterations of four cycles */
    c->budget = 100000;
    c->r[0] = 1000;
    dsr_swi(c, 0x03, RET);
    CHECK(c, c->budget == 100000 - 8 - 4000 && c->r[0] == 0, "WaitByLoop's cost or result");
    c->r[0] = 0;                /* BGT after one SUB: it runs once */
    dsr_swi(c, 0x03, RET);
    CHECK(c, c->budget == 100000 - 8 - 4000 - 8 - 4 && c->r[0] == 0xFFFFFFFFu, "WaitByLoop with r0 = 0");
    /* CustomHalt writes HALTCNT, CustomPost POSTFLG */
    nwlog = 0;
    swi(c, 0x1F, 0x1234, 0, 0xC0, 0);
    if (a9) CHECK(c, nwlog == 1 && wlog[0].a == 0x04000300 && wlog[0].v == 0x1234, "CustomPost did not write POSTFLG");
    else CHECK(c, nwlog == 1 && wlog[0].a == 0x04000301 && wlog[0].v == 0xC0 && wlog[0].size == 1, "CustomHalt did not write HALTCNT");
}

int main(void) {
    setup(&cpu9, 5);
    setup(&cpu7, 4);
    Cpu *cs[2] = {&cpu9, &cpu7};
    for (int i = 0; i < 2; i++) {
        Cpu *c = cs[i];
        test_decompress_memory(c);
        test_decompress_callbacks(c, 0);
        test_bitunpack(c);
        test_copy(c);
        test_crc(c);
        test_math(c);
        test_reset_and_halt(c);
    }
    test_decompress_callbacks(&cpu7, 1);        /* Thumb callbacks */
    test_decompress_callbacks(&cpu9, 1);
    test_decompress_vram(&cpu9);
    test_unfilter(&cpu9);
    test_arm7_tables(&cpu7);
    if (failures) printf("%d failures\n", failures);
    return failures != 0;
}
