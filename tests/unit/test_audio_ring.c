/* The app core's audio ring (app/Sources/DSNCore/dsn_core.c): what the frame loop pushes comes
 * out of dsn_core_audio_read in order, split into left and right and scaled to floats, across
 * the ring's wrap and the 32-bit counters', and a full ring drops the newest frames without
 * touching the unread ones. The ring is static and the core opaque, so this test includes the
 * file; the game's code tables it names are stood in for here. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../app/Sources/DSNCore/dsn_core.c"

const DispatchEntry g9_table[1] = {{0}}, g7_table[1] = {{0}};
const u32 g9_table_len = 0, g7_table_len = 0;
const PicEntry g9_pic[1] = {{0}}, g7_pic[1] = {{0}};
const u32 g9_pic_len = 0, g7_pic_len = 0;

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* a ramp per channel, distinct from each other and covering both signs; the frame's value is a
 * function of its index so a read can tell what it should be without keeping a copy */
static s16 left_of(u32 i) { return (s16)(i * 3 - 10000); }
static s16 right_of(u32 i) { return (s16)(7000 - i * 5); }
static float scaled(s16 v) { return (float)v / 32768.0f; }     /* exact: a power of two */

static void push_ramp(DsnCore *c, u32 first, u32 n) {
    s16 *f = malloc(n * 2 * sizeof *f);
    for (u32 i = 0; i < n; i++) { f[2 * i] = left_of(first + i); f[2 * i + 1] = right_of(first + i); }
    ring_push(c, f, n);
    free(f);
}

/* reads `frames` and checks that the real ones are frames first.. of the ramp and the rest silence */
static u32 read_ramp(DsnCore *c, u32 first, u32 frames, const char *what) {
    float *l = malloc(frames * sizeof *l), *r = malloc(frames * sizeof *r);
    for (u32 i = 0; i < frames; i++) l[i] = r[i] = 9.0f;
    u32 n = dsn_core_audio_read(c, l, r, frames);
    CHECK(n <= frames, "%s: %u real frames out of %u", what, n, frames);
    for (u32 i = 0; i < frames && i < n; i++) {
        if (l[i] != scaled(left_of(first + i)) || r[i] != scaled(right_of(first + i))) {
            CHECK(0, "%s: frame %u is %g, %g; expected %g, %g", what, first + i, l[i], r[i],
                  scaled(left_of(first + i)), scaled(right_of(first + i)));
            break;
        }
    }
    for (u32 i = n; i < frames; i++) {
        if (l[i] != 0.0f || r[i] != 0.0f) { CHECK(0, "%s: padding frame %u is %g, %g", what, i, l[i], r[i]); break; }
    }
    free(l); free(r);
    return n;
}

static u32 queued(DsnCore *c) { return atomic_load(&c->ring_w) - atomic_load(&c->ring_r); }

int main(void) {
    DsnCore *c = calloc(1, sizeof *c);

    /* the scale: s16 over 32768, so -32768 is -1.0, 16384 is 0.5, and nothing reaches +1.0 */
    {
        static const s16 v[] = {-32768, -16384, -1, 0, 1, 16384, 32767};
        s16 f[2 * sizeof v / sizeof *v];
        for (unsigned i = 0; i < sizeof v / sizeof *v; i++) { f[2 * i] = v[i]; f[2 * i + 1] = (s16)-(v[i] + (v[i] == -32768)); }
        ring_push(c, f, sizeof v / sizeof *v);
        float l[7], r[7];
        u32 n = dsn_core_audio_read(c, l, r, 7);
        CHECK(n == 7, "scale: %u frames read", n);
        CHECK(l[0] == -1.0f && l[1] == -0.5f && l[3] == 0.0f && l[5] == 0.5f && l[6] == 32767.0f / 32768.0f && l[6] < 1.0f,
              "scale: left %g %g %g %g %g", l[0], l[1], l[3], l[5], l[6]);
        CHECK(r[0] == 32767.0f / 32768.0f && r[5] == -0.5f && r[6] == -32767.0f / 32768.0f, "scale: right %g %g %g", r[0], r[5], r[6]);
    }

    /* order and the channel split over many wraps of the 4096-frame ring: pushes of 512 (a
     * frame's worth of the mixer), reads of 300 (the audio thread's own chunk), never more than
     * the ring holds queued */
    memset(c, 0, sizeof *c);
    {
        u32 pushed = 0, read = 0, total = 5 * RING_FRAMES + 123;
        while (read < total) {
            if (pushed < total && queued(c) + 512 <= RING_FRAMES) { u32 n = total - pushed < 512 ? total - pushed : 512; push_ramp(c, pushed, n); pushed += n; }
            u32 got = read_ramp(c, read, 300, "ramp");
            read += got;
            if (!got && pushed >= total) break;
        }
        CHECK(read == total && queued(c) == 0, "ramp: read %u of %u frames, %u left", read, total, queued(c));
        CHECK(atomic_load(&c->ring_w) == total && atomic_load(&c->ring_r) == total, "ramp: counters %u, %u", atomic_load(&c->ring_w), atomic_load(&c->ring_r));
    }

    /* the frame counters are 32 bits and wrap; 2^32 is a multiple of the ring size, so the slots
     * carry on where they were */
    memset(c, 0, sizeof *c);
    atomic_store(&c->ring_w, 0xFFFFFF00u);
    atomic_store(&c->ring_r, 0xFFFFFF00u);
    push_ramp(c, 0, 1000);
    CHECK(queued(c) == 1000 && atomic_load(&c->ring_w) == 0x2E8u, "counter wrap: %u queued, write counter %08x", queued(c), atomic_load(&c->ring_w));
    CHECK(read_ramp(c, 0, 1000, "counter wrap") == 1000, "counter wrap: short read");
    push_ramp(c, 1000, 600);
    CHECK(read_ramp(c, 1000, 1000, "after the counter wrap") == 600, "after the counter wrap: short read");

    /* a full ring takes what fits and drops the rest: the unread frames stay as they were */
    memset(c, 0, sizeof *c);
    push_ramp(c, 0, 5000);
    CHECK(queued(c) == RING_FRAMES, "overfull: %u queued, expected %u", queued(c), RING_FRAMES);
    push_ramp(c, 5000, 100);
    CHECK(queued(c) == RING_FRAMES, "full: another push grew the queue to %u", queued(c));
    CHECK(read_ramp(c, 0, 100, "overfull") == 100, "overfull: short read");
    push_ramp(c, 7000, 300);        /* 100 slots free: the first 100 of these fit (as frames 0.. of this push) */
    CHECK(queued(c) == RING_FRAMES, "refill: %u queued", queued(c));
    CHECK(read_ramp(c, 100, RING_FRAMES - 100, "the kept frames") == RING_FRAMES - 100, "the kept frames: short read");
    {
        float l[100], r[100];
        u32 n = dsn_core_audio_read(c, l, r, 100);
        CHECK(n == 100, "refill: %u frames read", n);
        int ok = 1;
        for (u32 i = 0; i < n; i++) ok &= l[i] == scaled(left_of(7000 + i)) && r[i] == scaled(right_of(7000 + i));
        CHECK(ok, "refill: the frames that fit are not the first of the push");
    }
    CHECK(queued(c) == 0, "drained: %u queued", queued(c));

    /* a read for more than is queued returns what there is and silence after it; an empty ring
     * is all silence */
    push_ramp(c, 200, 10);
    CHECK(read_ramp(c, 200, 64, "short") == 10, "short: expected 10 real frames");
    CHECK(read_ramp(c, 0, 64, "empty") == 0, "empty: expected no real frames");
    push_ramp(c, 0, 0);
    CHECK(queued(c) == 0, "empty push queued %u", queued(c));

    free(c);
    printf(failures ? "audio ring: %d failures\n" : "audio ring ok\n", failures);
    return failures != 0;
}
