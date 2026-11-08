/* The BIOS calls that decompress and copy, run from recompiled code (the unit test, tests/unit/
 * test_bios.c, covers the calls themselves on both CPUs; this checks the path through generated
 * code: a SWI that leaves the function for a callback, ARM and Thumb callbacks that return to the
 * BIOS, and the call coming back to the caller's next instruction with its registers). The callbacks
 * read their source at addresses nothing is mapped at, so a BIOS that read memory itself would
 * decompress garbage. Prints (tests/hw/expect_bios.txt has the expected lines):
 *   bioslz77    SWI 11h, memory to memory
 *   bioslz77cb  SWI 12h with Thumb callbacks: length returned, Open calls, Close calls
 *   bioslz77ab  the same with ARM Get callbacks beside Thumb Open and Close
 *   biosrlcb    SWI 15h
 *   bioshuffcb  SWI 13h: a tree of four 8-bit symbols with 2-bit codes
 *   biosunpack  SWI 10h, 1-bit units to 4-bit
 *   biosfast    SWI 0Ch: a word count that is not a multiple of 8
 *   biosdiff    SWI 16h
 *   bioscrc     SWI 0Eh against the bitwise CRC-16 */
#include "regs.h"

#define N 600
#define FAKE 0x08000000u
static u8 plain[N + 8], packed[2 * N + 64], out[N + 16], tmpbuf[0x200];
static u32 rng_s = 7;
static u32 rng(void) { rng_s ^= rng_s << 13; rng_s ^= rng_s >> 17; rng_s ^= rng_s << 5; return rng_s; }

#define SWI(n, a, b, c_, d) ({ register u32 r0 __asm__("r0") = (a), r1 __asm__("r1") = (b), r2 __asm__("r2") = (c_), r3 __asm__("r3") = (d); \
    __asm__ volatile("swi " #n : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3) :: "r12", "lr", "memory", "cc"); r0; })

static void header(u8 *o, u32 type, u32 n) { o[0] = (u8)type; o[1] = (u8)n; o[2] = (u8)(n >> 8); o[3] = (u8)(n >> 16); }

/* distances from min_disp: the 16-bit variants can't copy from the byte just written */
static u32 lz_encode(const u8 *in, u32 n, u8 *o, u32 min_disp) {
    u32 p = 4, i = 0;
    header(o, 0x10, n);
    while (i < n) {
        u32 fpos = p++;
        u8 flags = 0;
        for (int b = 0; b < 8 && i < n; b++) {
            u32 best = 0, bd = 0;
            for (u32 d = min_disp; d <= 4096 && d <= i; d++) {
                u32 l = 0;
                while (l < 18 && i + l < n && in[i + l - d] == in[i + l]) l++;
                if (l > best) { best = l; bd = d; }
            }
            if (best >= 3) { flags |= 0x80 >> b; o[p++] = (u8)(((best - 3) << 4) | ((bd - 1) >> 8)); o[p++] = (u8)(bd - 1); i += best; }
            else o[p++] = in[i++];
        }
        o[fpos] = flags;
    }
    while (p & 3) o[p++] = 0;
    return p;
}

static u32 rl_encode(const u8 *in, u32 n, u8 *o) {
    u32 p = 4, i = 0;
    header(o, 0x30, n);
    while (i < n) {
        u32 r = 1;
        while (i + r < n && r < 130 && in[i + r] == in[i]) r++;
        if (r >= 3) { o[p++] = (u8)(0x80 | (r - 3)); o[p++] = in[i]; i += r; continue; }
        u32 l = 0;
        while (i + l < n && l < 128) {
            u32 q = 1;
            while (i + l + q < n && q < 3 && in[i + l + q] == in[i + l]) q++;
            if (q >= 3) break;
            l++;
        }
        o[p++] = (u8)(l - 1);
        for (u32 k = 0; k < l; k++) o[p++] = in[i++];
    }
    while (p & 3) o[p++] = 0;
    return p;
}

/* the callbacks: the source is the buffer at `stream`, addressed from FAKE */
static const u8 *stream;
static u32 opens, closes, open_dst, open_param;
static u32 cb_open(u32 src, u32 dst, u32 param) { u32 v; opens++; open_dst = dst; open_param = param; for (int i = 0; i < 4; i++) ((u8 *)&v)[i] = stream[src - FAKE + (u32)i]; return v; }
static s32 cb_close(u32 src) { (void)src; closes++; return 0; }
static u32 cb_get8(u32 src) { return stream[src - FAKE]; }
static u32 cb_get32(u32 src) { u32 v; for (int i = 0; i < 4; i++) ((u8 *)&v)[i] = stream[src - FAKE + (u32)i]; return v; }
__attribute__((target("arm"))) static u32 cb_get8_arm(u32 src) { return stream[src - FAKE]; }
__attribute__((target("arm"))) static u32 cb_get32_arm(u32 src) { u32 v; for (int i = 0; i < 4; i++) ((u8 *)&v)[i] = stream[src - FAKE + (u32)i]; return v; }

static u32 cbs[5];
static void use_callbacks(const u8 *data, int arm_gets) {
    stream = data; opens = closes = 0;
    cbs[0] = (u32)cb_open; cbs[1] = (u32)cb_close; cbs[3] = 0;
    cbs[2] = arm_gets ? (u32)cb_get8_arm : (u32)cb_get8;
    cbs[4] = arm_gets ? (u32)cb_get32_arm : (u32)cb_get32;
}

static int same(const void *a, const void *b, u32 n) { const u8 *p = a, *q = b; while (n--) if (*p++ != *q++) return 0; return 1; }
static void hex(u32 v) { dbg(" "); dbg_hex(v); }

void bios_test9(void) {
    for (u32 i = 0; i < N; i++) plain[i] = i < 40 || rng() % 9 == 0 ? (u8)rng() : (rng() % 5 ? plain[i - 1 - (i % 3)] : plain[i - 16]);
    for (u32 i = 300; i < 380; i++) plain[i] = 0x55;

    lz_encode(plain, N, packed, 1);
    for (u32 i = 0; i < sizeof out; i++) out[i] = 0xCC;
    SWI(17, (u32)packed, (u32)out, 0, 0);
    dbg("bioslz77 "); dbg(same(out, plain, N) && out[N] == 0xCC ? "ok" : "FAIL"); dbg("\n");

    static u8 lz2[2 * N + 64], rl[2 * N + 64];
    lz_encode(plain, N, lz2, 2);
    for (int arm = 0; arm < 2; arm++) {
        for (u32 i = 0; i < sizeof out; i++) out[i] = 0xCC;
        use_callbacks(lz2, arm);
        u32 r = SWI(18, FAKE, (u32)out, 0x1234, (u32)cbs);
        dbg(arm ? "bioslz77ab " : "bioslz77cb ");
        dbg(same(out, plain, N) && out[N] == 0xCC && open_dst == (u32)out && open_param == 0x1234 ? "ok" : "FAIL");
        hex(r); hex(opens); hex(closes); dbg("\n");
    }

    rl_encode(plain, N, rl);
    for (u32 i = 0; i < sizeof out; i++) out[i] = 0xCC;
    use_callbacks(rl, 0);
    u32 r = SWI(21, FAKE, (u32)out, 0, (u32)cbs);
    dbg("biosrlcb "); dbg(same(out, plain, N) && out[N] == 0xCC ? "ok" : "FAIL"); hex(r); dbg("\n");

    /* four symbols, A B C D, as the leaves of a balanced tree: root, two nodes, four data nodes */
    static u8 hf[4 + 8 + N / 4 + 8];
    u32 words = N / 16;                 /* 16 two-bit codes a word, N symbols */
    header(hf, 0x28, N);
    static const u8 tree[8] = {3, 0x00, 0xC0, 0xC1, 'A', 'B', 'C', 'D'};
    for (int i = 0; i < 8; i++) hf[4 + i] = tree[i];
    static u8 sym[N];
    for (u32 i = 0; i < N; i++) sym[i] = (u8)('A' + (rng() >> 7) % 4);
    for (u32 w = 0; w < words + 1; w++) {
        u32 v = 0;
        for (u32 k = 0; k < 16 && w * 16 + k < N; k++) v |= (u32)(sym[w * 16 + k] - 'A') << (30 - 2 * k);
        for (int b = 0; b < 4; b++) hf[12 + 4 * w + (u32)b] = (u8)(v >> (8 * b));
    }
    for (u32 i = 0; i < sizeof out; i++) out[i] = 0xCC;
    use_callbacks(hf, 0);
    r = SWI(19, FAKE, (u32)out, (u32)tmpbuf, (u32)cbs);
    dbg("bioshuffcb "); dbg(same(out, sym, N) && out[N] == 0xCC && open_param == (u32)tmpbuf ? "ok" : "FAIL"); hex(r); hex(opens); hex(closes); dbg("\n");

    /* 1-bit font bytes to 4-bit tiles: a set bit becomes 1 + the offset 2 = 3, a clear one stays 0 */
    static const u8 font[4] = {0xA5, 0x0F, 0xF0, 0x81};
    static const u32 info[2] = {4 | 1u << 16 | 4u << 24, 2};   /* length 4, 1-bit units, 4-bit units, offset 2 */
    static u32 tiles[8];
    SWI(16, (u32)font, (u32)tiles, (u32)info, 0);
    u32 want[8] = {0};
    for (int i = 0; i < 4; i++)
        for (int b = 0; b < 8; b++) want[i] |= (u32)(((font[i] >> b) & 1) ? 3 : 0) << (4 * b);
    dbg("biosunpack "); dbg(same(tiles, want, 16) ? "ok" : "FAIL"); dbg("\n");

    /* CpuFastSet of 13 words (not a multiple of 8): the word after stays */
    static u32 src32[16], dst32[16];
    for (int i = 0; i < 16; i++) { src32[i] = 0x11111111u * (u32)(i + 1); dst32[i] = 0xDEAD0000u; }
    SWI(12, (u32)src32, (u32)dst32, 13, 0);
    int ok = 1;
    for (int i = 0; i < 13; i++) ok &= dst32[i] == src32[i];
    ok &= dst32[13] == 0xDEAD0000u;
    dbg("biosfast "); dbg(ok ? "ok" : "FAIL"); dbg("\n");

    /* Diff8bitUnFilter: running sums of the bytes */
    static u32 diffsrc[8], diffout[4];
    u8 *d8 = (u8 *)diffsrc;
    d8[0] = 0x08; d8[1] = 16; d8[2] = 0; d8[3] = 0;
    for (int i = 0; i < 16; i++) d8[4 + i] = (u8)(i == 0 ? 10 : 3 * i);
    for (int i = 0; i < 4; i++) diffout[i] = 0;
    SWI(22, (u32)diffsrc, (u32)diffout, 0, 0);
    u8 *o8 = (u8 *)diffout;
    ok = 1;
    u8 sum = 0;
    for (int i = 0; i < 16; i++) { sum = (u8)(sum + d8[4 + i]); ok &= o8[i] == sum; }
    dbg("biosdiff "); dbg(ok ? "ok" : "FAIL"); dbg("\n");

    /* GetCRC16 (r0 initial value, r1 address, r2 length) against the bitwise CRC-16 */
    u32 crc = 0xFFFF;
    for (u32 i = 0; i < 200; i++) {
        crc ^= plain[i];
        for (int k = 0; k < 8; k++) crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
    }
    u32 got = SWI(14, 0xFFFF, (u32)plain, 200, 0);
    dbg("bioscrc "); dbg(got == crc ? "ok" : "FAIL"); dbg("\n");
}
