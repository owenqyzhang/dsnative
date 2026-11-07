/* The sound channels, mixer and capture against GBATEK ("DS Sound"): sample timing of each
 * format, the ADPCM decoder, PSG duties and noise, one-shot ends and loops, the volume,
 * panning and master volume arithmetic, capture, the frame ring and the WAV recording.
 * Registers are written through the ARM7's memory map, as a game does. */
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include "nds.h"

static Nds *s;
static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("%s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

#define CH(n, r) (0x04000400u + 16u * (n) + (r))
#define SOUNDCNT 0x04000500u
#define SOUNDBIAS 0x04000504u
#define CAPCNT(x) (0x04000508u + (x))
#define CAPDAD(x) (0x04000510u + 8u * (x))
#define CAPLEN(x) (0x04000514u + 8u * (x))
#define BUSY 0x80000000u
#define HOLD 0x8000u
enum { MANUAL, LOOP, ONESHOT };
enum { PCM8, PCM16, ADPCM, PSG };
#define CNT(vol, div, pan, duty, rep, fmt) \
    ((u32)(vol) | (u32)(div) << 8 | (u32)(pan) << 16 | (u32)(duty) << 24 | (u32)(rep) << 27 | (u32)(fmt) << 29)

static void w32(u32 a, u32 v) { wr32(&s->arm7, a, v); }
static void w16(u32 a, u32 v) { wr16(&s->arm7, a, v); }
static void w8(u32 a, u32 v) { wr8(&s->arm7, a, v); }
static u32 r32(u32 a) { return rd32(&s->arm7, a); }
static u32 r8(u32 a) { return rd8(&s->arm7, a); }
static void at(u64 t) { s->time = t; sound_line(s, 0); }
static void run(u64 dt) { at(s->time + dt); }
static s32 out(int n) { return sound_channel_output(&s->sound, n); }
static int busy(int n) { return (r8(CH(n, 3)) & 0x80) != 0; }

static void fresh(void) {
    static u8 rom[0x1000];
    s = nds_create(rom, sizeof rom);
    w16(SOUNDCNT, 0x807F);              /* master on, volume 127 */
}

static void setup(int n, u32 sad, u32 period, u32 pnt, u32 len) {
    w32(CH(n, 4), sad);
    w32(CH(n, 8), (0x10000u - period / 2) | pnt << 16);
    w32(CH(n, 12), len);
}
static void start(int n, u32 cnt) { w32(CH(n, 0), cnt | BUSY); }

/* a channel looping a constant PCM16 value, past its start delay */
static void constant(int n, s16 v, u32 vol, u32 div, u32 pan) {
    u32 a = 0x02100000u + 0x100u * (u32)n;
    for (int i = 0; i < 8; i++) memcpy(s->ram + (a & 0x3FFFFF) + 2 * i, &v, 2);
    setup(n, a, 64, 0, 4);
    start(n, CNT(vol, div, pan, 0, LOOP, PCM16));
    run(64 * 4);
}

/* the frames mixed over the next few hundred bus cycles, all expected equal */
static void expect_frames(s16 l, s16 r, const char *what) {
    s16 buf[64 * 2];
    nds_audio_take(s, buf, 64);
    run(8 * 1024);
    u32 n = nds_audio_take(s, buf, 64);
    CHECK(n >= 7, "%s: %u frames", what, n);
    for (u32 i = 0; i < n; i++)
        if (buf[2 * i] != l || buf[2 * i + 1] != r) {
            CHECK(0, "%s: frame %u is (%d, %d), expected (%d, %d)", what, i, buf[2 * i], buf[2 * i + 1], l, r);
            break;
        }
}

/* ---- formats and timing --------------------------------------------------------------- */
static void test_pcm16_timing(void) {
    fresh();
    static const s16 smp[8] = {0x1000, -0x1000, 0x7FFF, -0x8000, 1, -1, 0x1234, 0x4321};
    memcpy(s->ram + 0x1000, smp, sizeof smp);
    const u64 P = 512, t0 = 1000;
    at(t0);
    setup(0, 0x02001000, P, 0, 4);              /* 4 words: 8 samples */
    start(0, CNT(127, 0, 64, 0, ONESHOT, PCM16));
    CHECK(busy(0), "not busy after the start");
    /* PCM starts after 3 samples: sample k comes out at the (3 + k)th timer overflow */
    /* one-shot: busy clears at the start of the last sample's period, and the sample stays
     * out to its end */
    for (int k = 0; k < 8; k++) {
        at(t0 + (3 + k) * P - 1);
        CHECK(out(0) == (k ? smp[k - 1] : 0), "before sample %d: %d", k, out(0));
        CHECK(busy(0), "not busy before sample %d", k);
        at(t0 + (3 + k) * P);
        CHECK(out(0) == smp[k], "sample %d: %d", k, out(0));
        CHECK(busy(0) == (k < 7), "busy %d at sample %d", busy(0), k);
    }
    at(t0 + 11 * P - 1);
    CHECK(out(0) == smp[7], "the last sample ended early");
    at(t0 + 11 * P);
    CHECK(out(0) == 0, "output %d after the end", out(0));
    CHECK((r32(CH(0, 0)) & 0x7FFFFFFFu) == CNT(127, 0, 64, 0, ONESHOT, PCM16), "SOUND0CNT reads %08x", r32(CH(0, 0)));
}

static void test_pcm8_and_wram(void) {
    fresh();
    s8 smp[16];
    for (int i = 0; i < 16; i++) smp[i] = (s8)(i * 17 - 128);
    memcpy(s->wram7 + 0x100, smp, sizeof smp);              /* ARM7 WRAM, 0x03800100 */
    const u64 P = 300, t0 = 5000;
    at(t0);
    setup(5, 0x03800100, P, 1, 3);                          /* PNT + LEN: 16 samples */
    start(5, CNT(127, 0, 64, 0, ONESHOT, PCM8));
    for (int k = 0; k < 16; k++) {
        at(t0 + (3 + k) * P - 1);
        CHECK(busy(5), "not busy before sample %d", k);
        at(t0 + (3 + k) * P);
        CHECK(out(5) == smp[k] * 256, "sample %d: %d", k, out(5));
    }
    CHECK(!busy(5), "busy at the last sample");
}

static void test_loop(void) {
    fresh();
    s16 smp[8];
    for (int i = 0; i < 8; i++) smp[i] = (s16)(100 * (i + 1));
    memcpy(s->ram + 0x2000, smp, sizeof smp);
    const u64 P = 128;
    at(0);
    setup(2, 0x02002000, P, 1, 3);              /* loop from word 1 (sample 2) to word 4 */
    start(2, CNT(127, 0, 64, 0, LOOP, PCM16));
    int k = 0;
    for (int i = 0; i < 8; i++, k++) { at((3 + k) * P); CHECK(out(2) == smp[i], "first pass, sample %d: %d", i, out(2)); }
    for (int pass = 0; pass < 3; pass++)
        for (int i = 2; i < 8; i++, k++) { at((3 + k) * P); CHECK(out(2) == smp[i], "pass %d, sample %d: %d", pass, i, out(2)); }
    CHECK(busy(2), "a loop ended");
}

static void test_hold(void) {
    fresh();
    s16 smp[8] = {1, 2, 3, 4, 5, 6, 7, 0x777};
    memcpy(s->ram + 0x3000, smp, sizeof smp);
    const u64 P = 200;
    at(0);
    setup(1, 0x02003000, P, 0, 4);
    start(1, CNT(127, 0, 64, 0, ONESHOT, PCM16) | HOLD);
    at(11 * P + 5000);
    CHECK(!busy(1) && out(1) == 0x777, "Hold: %d after the end", out(1));
    /* a restart keeps the held level for one sample, then is silent until its first sample */
    u64 t1 = s->time;
    smp[0] = 0x111;
    memcpy(s->ram + 0x3000, smp, 2);
    start(1, CNT(127, 0, 64, 0, ONESHOT, PCM16) | HOLD);
    at(t1 + P - 1); CHECK(out(1) == 0x777, "restart: %d in the first period", out(1));
    at(t1 + P); CHECK(out(1) == 0, "restart: %d in the second period", out(1));
    at(t1 + 3 * P); CHECK(out(1) == 0x111, "restart: %d at the first sample", out(1));
    at(t1 + 11 * P + 100);
    CHECK(out(1) == 0x777, "Hold: %d after the second end", out(1));
    w16(CH(1, 0), 127);                         /* clearing Hold ends the held level */
    CHECK(out(1) == 0, "%d after Hold was cleared", out(1));
    /* without Hold the restart starts from silence */
    start(1, CNT(127, 0, 64, 0, ONESHOT, PCM16));
    run(P - 1); CHECK(out(1) == 0, "no Hold: %d in the first period", out(1));
}

static void test_short(void) {
    fresh();
    at(0);
    setup(3, 0x02000000, 64, 1, 2);             /* PNT + LEN < 4 words: hangs busy and silent */
    s16 v = 0x4000;
    memcpy(s->ram, &v, 2);
    start(3, CNT(127, 0, 64, 0, ONESHOT, PCM16));
    run(64 * 100);
    CHECK(busy(3) && out(3) == 0, "short sound: busy %d, output %d", busy(3), out(3));
    w8(CH(3, 3), 0);                            /* stopped by a byte write */
    CHECK(!busy(3), "still busy after a stop");
}

static void put_adpcm(u32 off, s16 pcm, u32 idx, const u8 *nibbles, int n) {
    u32 h = (u16)pcm | idx << 16;
    memcpy(s->ram + off, &h, 4);
    for (int i = 0; i < n; i += 2) s->ram[off + 4 + i / 2] = (u8)(nibbles[i] | (i + 1 < n ? nibbles[i + 1] << 4 : 0));
}

static void test_adpcm(void) {
    fresh();
    /* worked by hand from GBATEK's decoder (each term rounded down on its own) */
    static const u8 nib[8] = {7, 7, 0xF, 0, 8, 3, 0xB, 4};
    static const s16 want[8] = {11, 41, -22, -13, -21, 31, -15, 41};
    u8 all[24] = {0};
    memcpy(all, nib, 8);
    put_adpcm(0x4000, 0, 0, all, 24);
    const u64 P = 100, t0 = 777;
    at(t0);
    setup(4, 0x02004000, P, 0, 4);              /* header + 3 words: 24 samples */
    start(4, CNT(127, 0, 64, 0, ONESHOT, ADPCM));
    /* ADPCM starts after 11 samples: 3 as PCM, 8 for the header */
    at(t0 + 11 * P - 1);
    CHECK(out(4) == 0, "%d before the first sample", out(4));
    for (int k = 0; k < 8; k++) { at(t0 + (11 + k) * P); CHECK(out(4) == want[k], "sample %d: %d, expected %d", k, out(4), want[k]); }
    /* nibble 0 adds step / 8 and lowers the index by one: index 22 after the vector above */
    static const u16 step[] = {0x3C, 0x37, 0x32, 0x2D, 0x29, 0x25, 0x22, 0x1F, 0x1C, 0x19, 0x17, 0x15, 0x13, 0x11, 0x10, 0x0E};
    s32 pcm = want[7];
    for (int k = 8; k < 24; k++) {
        pcm += step[k - 8] >> 3;
        at(t0 + (11 + k) * P);
        CHECK(out(4) == pcm, "sample %d: %d, expected %d", k, out(4), pcm);
    }
    CHECK(!busy(4), "busy at the last sample");

    /* clipping: to +7FFFh and -7FFFh, but a -8000h header stays until something goes down */
    static const u8 clip[3] = {7, 0xF, 0xF};
    memset(all, 0, sizeof all); memcpy(all, clip, 3);
    put_adpcm(0x5000, 0x7FF0, 88, all, 24);
    static const u8 low[2] = {0, 8};
    u8 all2[24] = {0}; memcpy(all2, low, 2);
    put_adpcm(0x5100, -0x8000, 0, all2, 24);
    u8 all3[24] = {0}; all3[0] = 7;
    put_adpcm(0x5200, 0, 100, all3, 24);        /* index past 88: the table reads 0 */
    u64 t1 = s->time + 1000;
    at(t1);
    setup(8, 0x02005000, P, 0, 4); start(8, CNT(127, 0, 64, 0, ONESHOT, ADPCM));
    setup(9, 0x02005100, P, 0, 4); start(9, CNT(127, 0, 64, 0, ONESHOT, ADPCM));
    setup(10, 0x02005200, P, 0, 4); start(10, CNT(127, 0, 64, 0, ONESHOT, ADPCM));
    at(t1 + 11 * P);
    CHECK(out(8) == 0x7FFF, "clip high: %d", out(8));
    CHECK(out(9) == -0x8000, "-8000h header: %d", out(9));
    CHECK(out(10) == 0, "index past 88: %d", out(10));
    at(t1 + 12 * P);
    CHECK(out(8) == 0x7FFF - (0xFFF + 0x1FFF + 0x3FFF + 0x7FFF), "step 88 down: %d", out(8));
    CHECK(out(9) == -0x7FFF, "-8000h going down: %d", out(9));
    CHECK(out(10) == 0x7FFF >> 3, "the index clamped to 88: %d", out(10));
    at(t1 + 13 * P);
    CHECK(out(8) == -0x7FFF, "clip low: %d", out(8));
}

static void test_adpcm_loop(void) {
    fresh();
    u8 nib[24];
    for (int i = 0; i < 24; i++) nib[i] = (u8)((i * 5 + 3) & 15);
    put_adpcm(0x6000, 0x100, 10, nib, 24);
    const u64 P = 64;
    at(0);
    setup(6, 0x02006000, P, 2, 2);              /* loop from word 2: sample 8 */
    start(6, CNT(127, 0, 64, 0, LOOP, ADPCM));
    s32 first[24];
    for (int k = 0; k < 24; k++) { at((11 + k) * P); first[k] = out(6); }
    /* at the loop end the decoder goes back to sample 8 with the PCM value and index it had there */
    int k = 24;
    for (int pass = 0; pass < 3; pass++)
        for (int i = 8; i < 24; i++, k++) {
            at((u64)(11 + k) * P);
            CHECK(out(6) == first[i], "pass %d, sample %d: %d, expected %d", pass, i, out(6), first[i]);
        }
    CHECK(busy(6), "a loop ended");
}

static void test_psg(void) {
    fresh();
    const u64 P = 64;
    for (u32 duty = 0; duty < 8; duty++) {
        u64 t0 = s->time + 1000;
        at(t0);
        setup(8 + duty % 6, 0, P, 0, 0);
        start(8 + duty % 6, CNT(127, 0, 64, duty, ONESHOT, PSG));     /* repeat and length don't apply */
        CHECK(out(8 + duty % 6) == 0, "duty %u: %d before the first sample", duty, out(8 + duty % 6));
        int high = 0;
        for (int k = 0; k < 64; k++) {
            at(t0 + (1 + k) * P);                                   /* PSG starts after 1 sample */
            s32 v = out(8 + duty % 6);
            /* each cycle starts with its LOW part: duty N is HIGH for the last N + 1 of 8 */
            s32 want = duty != 7 && (k & 7) >= 7 - (int)duty ? 0x7FFF : -0x7FFF;
            if (v != want) { CHECK(0, "duty %u, sample %d: %d, expected %d", duty, k, v, want); break; }
            high += v > 0;
        }
        CHECK(high == (duty == 7 ? 0 : 8 * ((int)duty + 1)), "duty %u: %d HIGH samples in 8 cycles", duty, high);
        CHECK(busy(8 + duty % 6), "duty %u: PSG ended", duty);
        w8(CH(8 + duty % 6, 3), 0);
    }
    /* no square waves on channels 0-7 */
    setup(3, 0, P, 0, 0);
    start(3, CNT(127, 0, 64, 3, LOOP, PSG));
    for (int k = 0; k < 16; k++) { run(P); CHECK(out(3) == 0, "PSG on channel 3: %d", out(3)); }
}

static void test_noise(void) {
    fresh();
    const u64 P = 40;
    at(100);
    setup(14, 0, P, 0, 0);
    setup(15, 0, 3 * P, 0, 0);
    start(14, CNT(127, 0, 64, 0, 0, PSG));
    start(15, CNT(127, 0, 64, 0, 0, PSG));
    /* GBATEK: X = 7FFFh at the start; X = X SHR 1, and on a carry Out = LOW, X = X XOR 6000h */
    u32 x = 0x7FFF, x15 = 0x7FFF;
    int lows = 0;
    for (int k = 0; k < 3000; k++) {
        int carry = x & 1;
        x >>= 1;
        if (carry) x ^= 0x6000;
        at(100 + (1 + (u64)k) * P);
        s32 want = carry ? -0x7FFF : 0x7FFF;
        if (out(14) != want) { CHECK(0, "noise sample %d: %d, expected %d", k, out(14), want); break; }
        if (k < 15) lows += out(14) < 0;     /* 7FFFh to 4001h shift out ones: 14 LOW, then HIGH */
        if (k % 3 == 2) {
            int c15 = x15 & 1;
            x15 >>= 1;
            if (c15) x15 ^= 0x6000;
            if (out(15) != (c15 ? -0x7FFF : 0x7FFF)) { CHECK(0, "channel 15, sample %d: %d", k / 3, out(15)); break; }
        }
    }
    CHECK(lows == 14, "%d of the first 15 noise samples are LOW", lows);
    /* a restart (start bit 0 to 1) starts the sequence over */
    w8(CH(14, 3), 0);
    start(14, CNT(127, 0, 64, 0, 0, PSG));
    u64 t = s->time;
    int restart_lows = 0;
    for (int k = 0; k < 15; k++) { at(t + (1 + (u64)k) * P); restart_lows += out(14) < 0; }
    CHECK(restart_lows == 14, "restart: %d LOW", restart_lows);
}

/* ---- the mixer ------------------------------------------------------------------------ */
static void test_mixer(void) {
    /* worked through GBATEK's bit widths: volume 16.11, panning 16.18 rounded down to 16.8,
     * mixer 20.8, master volume 14.21, then bias 200h, the 10-bit clip, and the bias removed */
    fresh();
    constant(0, 0x4000, 127, 0, 64);
    expect_frames(0x2000, 0x2000, "full volume, centre");
    fresh();
    constant(0, 0x4000, 64, 1, 0);
    expect_frames(0x1000, 0, "volume 64, divider 2, left");
    fresh();
    constant(0, 0x7FFF, 127, 3, 127);
    expect_frames(0, 0x7C0, "divider 16, right");
    fresh();
    constant(0, 0x4000, 126, 0, 0);
    expect_frames(0x3F00, 0, "volume 126");
    fresh();
    w16(SOUNDCNT, 0x8040);
    constant(0, 0x4000, 127, 0, 0);
    expect_frames(0x2000, 0, "master volume 64");
    fresh();
    constant(0, -1, 127, 0, 64);
    expect_frames(-64, -64, "-1 rounds down");
    fresh();
    constant(0, -1, 1, 0, 63);                  /* panning rounds down: -16 * 63 / 1024 is -1, not 0 */
    expect_frames(-64, -64, "panning rounds down");
    fresh();
    for (int n = 0; n < 4; n++) constant(n, 0x7FFF, 127, 0, 0);
    expect_frames(0x7FC0, 0, "clipped high");
    fresh();
    for (int n = 0; n < 4; n++) constant(n, -0x8000, 127, 0, 0);
    expect_frames(-0x8000, 0, "clipped low");
    fresh();
    w16(SOUNDBIAS, 0x100);
    constant(0, 0x4000, 127, 0, 0);
    expect_frames(0x4000, 0, "bias 100h");
    constant(1, -0x7000, 127, 0, 0);
    expect_frames(-0x3000, 0, "bias 100h, within");
    constant(1, -0x8000, 127, 0, 0);
    constant(2, -0x8000, 127, 0, 0);
    expect_frames(-0x4000, 0, "bias 100h, clipped at -100h");

    /* SOUNDCNT's output selection and channels 1 and 3 kept from the mixer */
    fresh();
    constant(1, 0x4000, 127, 0, 0);
    constant(3, 0x2000, 127, 0, 127);
    constant(5, 0x1000, 127, 0, 64);
    expect_frames(0x4800, 0x2800, "mixer");
    w16(SOUNDCNT, 0x807F | 1 << 8 | 2 << 10);
    expect_frames(0x4000, 0x2000, "left from ch1, right from ch3");
    w16(SOUNDCNT, 0x807F | 3 << 8 | 3 << 10);
    expect_frames(0x4000, 0x2000, "ch1 + ch3 on both");
    w16(SOUNDCNT, 0x807F | 0x1000);
    expect_frames(0x800, 0x2800, "ch1 not to the mixer");
    w16(SOUNDCNT, 0x807F | 0x3000);
    expect_frames(0x800, 0x800, "ch1, ch3 not to the mixer");
    w16(SOUNDCNT, 0x807F | 0x3000 | 1 << 8 | 2 << 10);
    expect_frames(0x4000, 0x2000, "ch1, ch3 to the speakers but not the mixer");
    w16(SOUNDCNT, 0x007F);
    expect_frames(0, 0, "master off");
    CHECK(r32(SOUNDCNT) == 0x007F, "SOUNDCNT reads %08x", r32(SOUNDCNT));
    CHECK(r32(SOUNDBIAS) == 0x200, "SOUNDBIAS reads %08x", r32(SOUNDBIAS));
}

/* ---- capture -------------------------------------------------------------------------- */
/* runs capture x (timer of channel 1 or 3, period P) once over len words at dad */
static void capture(int x, u32 cnt, u32 len, u32 dad, u32 P) {
    w32(CH(2 * x + 1, 8), 0x10000u - P / 2);
    w32(CAPDAD(x), dad);
    w16(CAPLEN(x), len);
    w8(CAPCNT(x), cnt | 0x80);
}
static u32 ram32(u32 a) { u32 v; memcpy(&v, (a >> 24) == 3 ? s->wram7 + (a & 0xFFFF) : s->ram + (a & 0x3FFFFF), 4); return v; }
static void fill(u32 a, u32 v, int words) { for (int i = 0; i < words; i++) w32(a + 4 * (u32)i, v); }

static void test_capture(void) {
    fresh();
    const u32 P = 256, dad = 0x03800200;
    constant(0, 0x1000, 127, 0, 0);
    constant(2, 0x0800, 127, 0, 64);
    fill(dad, 0xDEADBEEF, 4);
    u64 t1 = s->time;
    capture(0, 0, 2, dad, P);                   /* left mixer, PCM16, loop... */
    w8(CAPCNT(0), 0x84);                        /* ...made one-shot while running */
    CHECK(r8(CAPCNT(0)) == 0x84, "SNDCAP0CNT reads %02x", r8(CAPCNT(0)));
    CHECK(r32(CAPDAD(0)) == dad, "SNDCAP0DAD reads %08x", r32(CAPDAD(0)));
    at(t1 + 4 * P - 1);
    CHECK(r8(CAPCNT(0)) & 0x80, "capture ended early");
    at(t1 + 4 * P);
    CHECK(!(r8(CAPCNT(0)) & 0x80), "one-shot capture still busy");
    CHECK(ram32(dad) == 0x14001400u && ram32(dad + 4) == 0x14001400u, "captured %08x %08x", ram32(dad), ram32(dad + 4));
    CHECK(ram32(dad + 8) == 0xDEADBEEF, "wrote past its length: %08x", ram32(dad + 8));

    /* the mixer is clipped to 16 bits for capture */
    fresh();
    for (int n = 0; n < 3; n++) constant(n, 0x7FFF, 127, 0, 0);
    for (int n = 3; n < 6; n++) constant(n, -0x8000, 127, 0, 127);
    capture(0, 4, 1, dad, P);
    capture(1, 4, 1, dad + 4, P);
    run(2 * P);
    CHECK(ram32(dad) == 0x7FFF7FFFu && ram32(dad + 4) == 0x80008000u, "clipped capture %08x %08x", ram32(dad), ram32(dad + 4));

    /* PCM8: the 16.8 value / 100h, the fraction's top bit rounding toward zero */
    static const struct { s16 in; u8 want; } r8tab[] = {
        {0x01C0, 0x01}, {-0x0100, 0xFF}, {-0x0140, 0xFF}, {-0x0180, 0xFF}, {-0x01C0, 0xFE}, {-0x0040, 0x00},
    };
    for (unsigned i = 0; i < sizeof r8tab / sizeof r8tab[0]; i++) {
        fresh();
        constant(0, r8tab[i].in, 127, 0, 0);
        fill(dad, 0, 1);
        capture(0, 4 | 8, 1, dad, P);
        run(4 * P);
        CHECK(ram32(dad) == r8tab[i].want * 0x01010101u, "PCM8 capture of %d: %08x", r8tab[i].in, ram32(dad));
    }

    /* loop: wraps to DAD and keeps going */
    fresh();
    constant(0, 0x1234, 127, 0, 0);
    fill(dad, 0xDEADBEEF, 2);
    capture(0, 0, 1, dad, P);
    run(10 * P);
    CHECK(r8(CAPCNT(0)) & 0x80, "loop capture ended");
    CHECK(ram32(dad) == 0x12341234u && ram32(dad + 4) == 0xDEADBEEF, "loop capture %08x %08x", ram32(dad), ram32(dad + 4));
    w8(CAPCNT(0), 0);
    CHECK(!(r8(CAPCNT(0)) & 0x80), "capture didn't stop");

    /* channel 0 as the source: before its panning */
    fresh();
    constant(0, 0x1000, 127, 0, 127);
    capture(0, 2 | 4, 1, dad, P);
    run(2 * P);
    CHECK(ram32(dad) == 0x10001000u, "channel 0 capture %08x", ram32(dad));
    /* bug: -8000h when channels 0 and 1 are both negative, without addition */
    fresh();
    constant(0, -0x1000, 127, 0, 0);
    constant(1, -0x10, 127, 0, 0);
    capture(0, 2 | 4, 1, dad, P);
    run(2 * P);
    CHECK(ram32(dad) == 0x80008000u, "both negative %08x", ram32(dad));
    fresh();
    constant(0, -0x1000, 127, 0, 0);
    constant(1, 0x10, 127, 0, 0);
    capture(0, 2 | 4, 1, dad, P);
    run(2 * P);
    CHECK(ram32(dad) == 0xF000F000u, "one negative %08x", ram32(dad));
    /* bug: with addition, channel 0 + 1 overflows instead of clipping */
    fresh();
    constant(0, 0x6000, 127, 0, 0);
    constant(1, 0x6000, 127, 0, 127);
    capture(0, 1 | 2 | 4, 1, dad, P);
    run(2 * P);
    CHECK(ram32(dad) == 0xC000C000u, "addition overflow %08x", ram32(dad));
}

/* the format bit (SNDCAPxCNT bit 3) rewritten on a running capture, mid-word: GBATEK is silent,
 * so the partial word is dropped and the next one starts clean (it must not wedge or overshift) */
static void test_capture_format_change(void) {
    const u32 P = 256, dad = 0x03800200;
    /* PCM8 -> PCM16 after 3 of its 4 bytes: two PCM16 samples are then a whole word */
    fresh();
    constant(0, 0x1000, 127, 0, 0);
    fill(dad, 0xDEADBEEF, 2);
    u64 t1 = s->time;
    capture(0, 4 | 8, 1, dad, P);
    at(t1 + 3 * P);
    CHECK(ram32(dad) == 0xDEADBEEF, "wrote a partial word %08x", ram32(dad));
    w8(CAPCNT(0), 0x80 | 4);
    at(t1 + 5 * P - 1);
    CHECK(r8(CAPCNT(0)) & 0x80, "ended early after PCM8 to PCM16");
    at(t1 + 5 * P);
    CHECK(!(r8(CAPCNT(0)) & 0x80), "PCM8 to PCM16: the capture never finished its word");
    CHECK(ram32(dad) == 0x10001000u, "PCM8 to PCM16 captured %08x", ram32(dad));
    CHECK(ram32(dad + 4) == 0xDEADBEEF, "wrote past its length: %08x", ram32(dad + 4));

    /* PCM16 -> PCM8 after one of its two samples: four PCM8 samples, none of the old half */
    fresh();
    constant(0, 0x01C0, 127, 0, 0);
    fill(dad, 0xDEADBEEF, 2);
    t1 = s->time;
    capture(0, 4, 1, dad, P);
    at(t1 + P);
    w8(CAPCNT(0), 0x80 | 4 | 8);
    at(t1 + 5 * P - 1);
    CHECK(r8(CAPCNT(0)) & 0x80, "ended early after PCM16 to PCM8");
    at(t1 + 5 * P);
    CHECK(!(r8(CAPCNT(0)) & 0x80), "PCM16 to PCM8 still busy");
    CHECK(ram32(dad) == 0x01010101u, "PCM16 to PCM8 captured %08x", ram32(dad));

    /* the same format written again, and the other capture's register, keep the partial word */
    fresh();
    constant(0, 0x01C0, 127, 0, 0);
    fill(dad, 0xDEADBEEF, 2);
    t1 = s->time;
    capture(0, 4 | 8, 1, dad, P);
    at(t1 + 2 * P);
    w8(CAPCNT(0), 0x80 | 4 | 8);
    w8(CAPCNT(1), 0x04 | 8);                    /* capture 1 stays off */
    at(t1 + 4 * P);
    CHECK(!(r8(CAPCNT(0)) & 0x80), "same format dropped the partial word: still busy");
    CHECK(ram32(dad) == 0x01010101u, "same format captured %08x", ram32(dad));

    /* a looping capture with the format flipped at every sample, then left alone: it keeps
     * running inside its buffer and the next whole word is clean */
    fresh();
    constant(0, 0x1000, 127, 0, 0);
    fill(dad, 0xDEADBEEF, 2);
    capture(0, 0x88, 1, dad, P);
    for (int i = 0; i < 41; i++) { w8(CAPCNT(0), i & 1 ? 0x80 : 0x88); run(P); }
    CHECK(r8(CAPCNT(0)) & 0x80, "the loop capture stopped");
    w8(CAPCNT(0), 0x80);
    run(2 * P);
    CHECK(ram32(dad) == 0x10001000u, "after flipping the format %08x", ram32(dad));
    CHECK(ram32(dad + 4) == 0xDEADBEEF, "wrote past its length: %08x", ram32(dad + 4));
}

static void test_addition(void) {
    /* SNDCAP0CNT bits 0 and 7: channel 1 joins channel 0 before its panning */
    fresh();
    const u32 P = 256, dad = 0x02200000;
    constant(0, 0x1000, 127, 0, 0);
    constant(1, 0x0800, 127, 0, 127);
    expect_frames(0x1000, 0x800, "no addition");
    capture(0, 1, 1, dad, P);
    expect_frames(0x1800, 0, "addition");
    w16(SOUNDCNT, 0x807F | 0x1000);
    expect_frames(0x1000, 0, "addition, ch1 not to the mixer");
    w16(SOUNDCNT, 0x807F | 1 << 10);
    expect_frames(0x1800, 0, "addition: no ch1 for the right speaker");
    w16(SOUNDCNT, 0x807F);
    w8(CH(0, 3), 0);                            /* channel 0 off mutes channel 1 */
    expect_frames(0, 0, "addition, ch0 off");
    /* but the capture of channel 0 with addition is then plain channel 1 */
    w8(CAPCNT(0), 0);
    capture(0, 1 | 2 | 4, 1, dad, P);
    run(2 * P);
    CHECK(ram32(dad) == 0x08000800u, "addition with ch0 off %08x", ram32(dad));
    w8(CAPCNT(0), 1);                           /* bit 0 without bit 7: no addition */
    constant(0, 0x1000, 127, 0, 0);
    expect_frames(0x1000, 0x800, "bit 0 alone");
}

/* ---- the ARM7 BIOS's sound calls -------------------------------------------------------- */
static u32 swi7(u32 num, u32 r0, u32 r1) {
    s->arm7.r[0] = r0; s->arm7.r[1] = r1;
    CHECK(dsr_swi(&s->arm7, num, 0) == 0, "SWI %02x did not return to the caller", num);
    return s->arm7.r[0];
}

/* the divider the SDK's driver picks for a level in tenths of a dB */
static int driver_divider(int db) { return db < -240 ? 16 : db < -120 ? 4 : db < -60 ? 2 : 1; }

static void test_bios_tables(void) {
    fresh();
    /* SWI 1Bh, GetPitchTable: 2^(i/768) as 16.16 less 1.0, rounded to nearest. libm is the
     * independent check on the series the BIOS code sums. */
    u32 prev = 0, bad = 0;
    for (u32 i = 0; i < 0x300; i++) {
        u32 v = swi7(0x1B, i, 0), want = (u32)(65536.0 * pow(2.0, i / 768.0) + 0.5) - 65536u;
        if (v != want && !bad++) CHECK(0, "pitch table[%u] is %04x, expected %04x", i, v, want);
        if (i && v <= prev && !bad++) CHECK(0, "pitch table[%u] = %04x does not rise", i, v);
        prev = v;
    }
    CHECK(swi7(0x1B, 0, 0) == 0, "pitch table starts at %04x", swi7(0x1B, 0, 0));
    CHECK(swi7(0x1B, 0x2FF, 0) == 0xFF8A, "GBATEK: the largest entry is FF8Ah, got %04x", swi7(0x1B, 0x2FF, 0));
    CHECK(swi7(0x1B, 384, 0) == 0x6A0A, "half an octave is sqrt(2) = 1.6A0A: %04x", swi7(0x1B, 384, 0));
    CHECK(swi7(0x1B, 0x300, 0) == 0 && swi7(0x1B, 0xFFFFFFFFu, 0) == 0, "reads past the pitch table");
    /* SWI 1Ch, GetVolumeTable: index 723 + the level in tenths of a dB, as an amplitude of
     * 0-7Fh. The SDK's driver sets the channel's volume divider from the level (Div16 below
     * -240, Div4 below -120, Div2 below -60: 24, 12 and 6 dB of the hardware's steps) and
     * ORs it in, so the table holds the amplitude scaled up by that divider. */
    prev = 0; bad = 0;
    u32 zeros = 0;
    for (u32 i = 0; i < 0x2D4; i++) {
        int db = (int)i - 723, div = driver_divider(db);
        u32 v = swi7(0x1C, i, 0), want = (u32)(127.0 * div * pow(10.0, db / 20.0 / 10.0) + 0.5);
        if (v != want && !bad++) CHECK(0, "volume table[%u] is %02x, expected %02x", i, v, want);
        if (v > 127 && !bad++) CHECK(0, "volume table[%u] = %02x is not 7-bit", i, v);
        /* with the divider the channel applies, the level is the amplitude to within a step */
        double eff = (double)v / div, amp = 127.0 * pow(10.0, db / 200.0);
        if (fabs(eff - amp) > 0.5 / div + 1e-9 && !bad++) CHECK(0, "volume table[%u]: %02x / %d is %.3f, expected %.3f", i, v, div, eff, amp);
        if (i && v < prev && div == driver_divider(db - 1) && !bad++) CHECK(0, "volume table[%u] = %02x falls", i, v);
        zeros += v == 0;
        prev = v;
    }
    CHECK(swi7(0x1C, 0x2D3, 0) == 0x7F, "0 dB is %02x", swi7(0x1C, 0x2D3, 0));
    CHECK(swi7(0x1C, 0, 0) == 0, "-72.3 dB is %02x", swi7(0x1C, 0, 0));
    CHECK(swi7(0x1C, 723 - 60, 0) == 64, "-6 dB is half of 127: %02x", swi7(0x1C, 723 - 60, 0));
    CHECK(swi7(0x1C, 723 - 200, 0) == 51, "-20 dB is a tenth, times the divider 4: %02x", swi7(0x1C, 723 - 200, 0));
    CHECK(swi7(0x1C, 723 - 136, 0) == 106, "-13.6 dB, times 4: %02x", swi7(0x1C, 723 - 136, 0));
    CHECK(swi7(0x1C, 723 - 241, 0) == 127 && swi7(0x1C, 723 - 240, 0) == 32, "the range edge at -24 dB: %02x, %02x",
          swi7(0x1C, 723 - 241, 0), swi7(0x1C, 723 - 240, 0));
    CHECK(zeros == 2, "%u silent entries", zeros);
    CHECK(swi7(0x1C, 0x2D4, 0) == 0, "reads past the volume table");
}

static void test_bios_bias(void) {
    fresh();
    /* SWI 08h, SoundBias: r0 = 0 goes to level 0, any other value to 200h */
    w16(SOUNDBIAS, 0x123);
    swi7(0x08, 1, 0);
    CHECK(r32(SOUNDBIAS) == 0x200, "SoundBias(1) left %03x", r32(SOUNDBIAS));
    swi7(0x08, 0, 0);
    CHECK(r32(SOUNDBIAS) == 0, "SoundBias(0) left %03x", r32(SOUNDBIAS));
    swi7(0x08, 0x40, 0);
    CHECK(r32(SOUNDBIAS) == 0x200, "SoundBias(40h) left %03x", r32(SOUNDBIAS));
    /* the steps cost time (the BIOS call itself 8 cycles): more delay, more cycles; none
     * when it is at the level already */
    s32 b = s->arm7.budget;
    swi7(0x08, 1, 0);
    CHECK(b - s->arm7.budget == 8, "SoundBias to the level it is at cost %d cycles", b - s->arm7.budget);
    b = s->arm7.budget;
    swi7(0x08, 0, 1);
    s32 short_cost = b - s->arm7.budget;
    swi7(0x08, 1, 0);
    b = s->arm7.budget;
    swi7(0x08, 0, 100);
    s32 long_cost = b - s->arm7.budget;
    CHECK(short_cost > 8 && long_cost > 10 * short_cost, "a delay of 100 cost %d cycles, of 1 cost %d", long_cost, short_cost);
    /* and the sound hears it: at bias 0 the 10-bit output can't go below 0 */
    fresh();
    constant(0, -0x4000, 127, 0, 0);
    expect_frames(-0x4000, 0, "bias 200h");
    swi7(0x08, 0, 0);
    expect_frames(0, 0, "bias 0, clipped low");
    swi7(0x08, 1, 0);
    expect_frames(-0x4000, 0, "bias 200h again");
}

/* ---- frames, ring and WAV ------------------------------------------------------------- */
static void test_ring(void) {
    fresh();
    /* 32768 frames per second of bus time, none lost when taken every video frame */
    s16 buf[1024 * 2];
    u32 total = 0;
    u64 t = 0;
    while (t < 33513982u) {
        t += 560190;
        if (t > 33513982u) t = 33513982u;
        at(t);
        u32 n;
        while ((n = nds_audio_take(s, buf, 1024))) total += n;
    }
    CHECK(total == 32768, "%u frames in a second", total);
    /* frames nobody takes are dropped, oldest first */
    run(33513982u);
    u32 n = 0, k;
    while ((k = nds_audio_take(s, buf, 1024))) n += k;
    CHECK(n == SOUND_RING, "%u frames kept, expected %d", n, SOUND_RING);
}

static u32 le32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static u32 le16(const u8 *p) { return p[0] | p[1] << 8; }

static void test_wav(void) {
    fresh();
    CHECK(nds_option(s, "--wav", "test_sound.wav"), "--wav not taken");
    const u32 P = 2 * 2095;                     /* about 1000 Hz: 16756991 / (8 * 2095) */
    at(0);
    setup(8, 0, P, 0, 0);
    start(8, CNT(127, 0, 64, 3, 0, PSG));
    for (int f = 0; f < 30; f++) at(s->time + 560190);
    u64 frames = s->sound.frames;
    CHECK(frames + 1 >= s->time * SOUND_RATE / 33513982u && frames <= s->time * SOUND_RATE / 33513982u + 1,
          "%llu frames mixed in %llu bus cycles", (unsigned long long)frames, (unsigned long long)s->time);
    nds_shutdown(s);
    FILE *f = fopen("test_sound.wav", "rb");
    CHECK(f, "no WAV written");
    if (!f) return;
    static u8 b[1 << 20];
    size_t len = fread(b, 1, sizeof b, f);
    fclose(f);
    CHECK(len >= 44 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WAVEfmt ", 8) && !memcmp(b + 36, "data", 4), "not a WAV");
    CHECK(le32(b + 4) == len - 8, "RIFF size %u for %zu bytes", le32(b + 4), len);
    CHECK(le16(b + 20) == 1 && le16(b + 22) == 2 && le32(b + 24) == SOUND_RATE && le32(b + 28) == 4 * SOUND_RATE &&
          le16(b + 32) == 4 && le16(b + 34) == 16, "format: not 32768 Hz stereo s16");
    CHECK(le32(b + 40) == len - 44 && le32(b + 40) == 4 * frames, "data size %u for %llu frames", le32(b + 40), (unsigned long long)frames);
    /* a 50% square wave at volume 127, centre: +-3FFFh, which the 10-bit output makes 3FC0h and -4000h */
    int changes = 0, odd = 0;
    s16 prev = 0;
    for (size_t i = 44; i + 4 <= len; i += 4) {
        s16 l = (s16)le16(b + i), r = (s16)le16(b + i + 2);
        if (l != r || (l != 0 && l != 0x3FC0 && l != -0x4000)) odd++;
        if (prev && l && (l > 0) != (prev > 0)) changes++;
        if (l) prev = l;
    }
    double secs = 30 * 560190 / 33513982.0, want = 2 * 16756991.0 / (8 * 2095) * secs;
    CHECK(!odd, "%d frames not at the square wave's levels", odd);
    CHECK(changes > want - 3 && changes < want + 3, "%d level changes, expected %.0f", changes, want);
}

int main(void) {
    test_pcm16_timing();
    test_pcm8_and_wram();
    test_loop();
    test_hold();
    test_short();
    test_adpcm();
    test_adpcm_loop();
    test_psg();
    test_noise();
    test_mixer();
    test_capture();
    test_addition();
    test_capture_format_change();
    test_bios_tables();
    test_bios_bias();
    test_ring();
    test_wav();
    if (fails) printf("%d sound checks failed\n", fails);
    return fails != 0;
}
