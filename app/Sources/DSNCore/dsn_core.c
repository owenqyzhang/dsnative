/* dsnative app core (see include/dsn_core.h). Links against libdsn.a: the runtime and the
 * game's generated code, which exports the dispatch tables. */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "include/dsn_core.h"
/* by path: SwiftPM keeps header search paths inside the package */
#include "../../../runtime/nds.h"

extern const DispatchEntry g9_table[];
extern const u32 g9_table_len;
extern const DispatchEntry g7_table[];
extern const u32 g7_table_len;
extern const PicEntry g9_pic[];
extern const u32 g9_pic_len;
extern const PicEntry g7_pic[];
extern const u32 g7_pic_len;

_Static_assert(DSN_KEY_A == KEY_A && DSN_KEY_B == KEY_B && DSN_KEY_SELECT == KEY_SELECT && DSN_KEY_START == KEY_START &&
               DSN_KEY_RIGHT == KEY_RIGHT && DSN_KEY_LEFT == KEY_LEFT && DSN_KEY_UP == KEY_UP && DSN_KEY_DOWN == KEY_DOWN &&
               DSN_KEY_R == KEY_R && DSN_KEY_L == KEY_L && DSN_KEY_X == KEY_X && DSN_KEY_Y == KEY_Y, "DSN_KEY_* must match KEY_*");
_Static_assert(DSN_SCREEN_W == SCREEN_W && DSN_SCREEN_H == SCREEN_H, "screen size");

/* 125 ms of sound: enough to ride out a late frame, little enough to keep latency down. */
#define RING_FRAMES 4096u

struct DsnCore {
    Nds *s;
    u8 *rom, *arm9;
    u32 rom_len, arm9_len;
    int shut;
    /* single producer (the frame loop), single consumer (the audio thread) */
    s16 ring[RING_FRAMES][2];
    _Atomic u32 ring_w, ring_r;     /* frames written and read, wrapping */
};

static u8 *load(const char *path, u32 *len, char *err, int err_len) {
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, (size_t)err_len, "cannot open %s", path); return 0; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *b = n > 0 ? malloc((size_t)n + 4) : 0;
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) {
        snprintf(err, (size_t)err_len, "cannot read %s", path);
        free(b); fclose(f);
        return 0;
    }
    fclose(f);
    *len = (u32)n;
    return b;
}

/* The ROM, mapped read-only: the runtime only reads it, and a game's ROM can be 512 MB, which on an
 * iPhone or Apple TV would count against the app's memory limit if it were read into the heap. Mapped
 * pages are clean and the system drops and refetches them as it needs. A zero page after the end
 * keeps a read that runs a few bytes past it harmless, as the heap copy's padding did. */
static size_t mapped_size(size_t n) { size_t page = (size_t)getpagesize(); return (n + page - 1) / page * page + page; }

static u8 *map_rom(const char *path, u32 *len, char *err, int err_len) {
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > 0x7FFFFFFF) {
        snprintf(err, (size_t)err_len, "cannot open %s", path);
        if (fd >= 0) close(fd);
        return 0;
    }
    size_t n = (size_t)st.st_size, total = mapped_size(n);
    u8 *b = mmap(0, total, PROT_READ, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (b == MAP_FAILED || mmap(b, n, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, 0) == MAP_FAILED) {
        snprintf(err, (size_t)err_len, "cannot read %s", path);
        if (b != MAP_FAILED) munmap(b, total);
        close(fd);
        return 0;
    }
    close(fd);
    *len = (u32)n;
    return b;
}

DsnCore *dsn_core_create(const char *rom_path, const char *arm9_path, char *err, int err_len) {
    DsnCore *c = calloc(1, sizeof *c);
    c->rom = map_rom(rom_path, &c->rom_len, err, err_len);
    if (c->rom) c->arm9 = load(arm9_path, &c->arm9_len, err, err_len);
    if (c->rom && c->rom_len < 0x200) snprintf(err, (size_t)err_len, "%s is too small to be a DS ROM", rom_path);
    if (!c->rom || !c->arm9 || c->rom_len < 0x200) { if (c->rom) munmap(c->rom, mapped_size(c->rom_len)); free(c->arm9); free(c); return 0; }
    c->s = nds_create(c->rom, c->rom_len);
    return c;
}

int dsn_core_option(DsnCore *c, const char *name, const char *value) { return nds_option(c->s, name, value); }

void dsn_core_boot(DsnCore *c) {
    c->s->arm9.table = g9_table; c->s->arm9.table_len = g9_table_len;
    c->s->arm7.table = g7_table; c->s->arm7.table_len = g7_table_len;
    c->s->arm9.pic = g9_pic; c->s->arm9.pic_len = g9_pic_len;
    c->s->arm7.pic = g7_pic; c->s->arm7.pic_len = g7_pic_len;
    nds_direct_boot(c->s, c->arm9, c->arm9_len);
}

void dsn_core_set_input(DsnCore *c, uint16_t keys, int touch_down, int touch_x, int touch_y) {
    c->s->keys = keys & 0x0FFF;
    c->s->touch_down = touch_down != 0;
    if (touch_down) {
        c->s->touch_x = touch_x < 0 ? 0 : touch_x > SCREEN_W - 1 ? SCREEN_W - 1 : touch_x;
        c->s->touch_y = touch_y < 0 ? 0 : touch_y > SCREEN_H - 1 ? SCREEN_H - 1 : touch_y;
    }
}

/* Queues sound for the audio thread; when it has fallen behind, what doesn't fit is dropped. */
static void ring_push(DsnCore *c, const s16 *frames, u32 n) {
    u32 w = atomic_load_explicit(&c->ring_w, memory_order_relaxed);
    u32 r = atomic_load_explicit(&c->ring_r, memory_order_acquire);
    u32 room = RING_FRAMES - (w - r);
    if (n > room) n = room;
    for (u32 i = 0; i < n; i++) {
        c->ring[(w + i) % RING_FRAMES][0] = frames[2 * i];
        c->ring[(w + i) % RING_FRAMES][1] = frames[2 * i + 1];
    }
    atomic_store_explicit(&c->ring_w, w + n, memory_order_release);
}

void dsn_core_run_frame(DsnCore *c) {
    if (c->shut) return;
    nds_run_frame(c->s);
    /* about 548 frames of sound per video frame; drain the mixer even when the ring is full */
    s16 buf[512 * 2];
    for (int i = 0; i < 64; i++) {
        u32 n = nds_audio_take(c->s, buf, 512);
        ring_push(c, buf, n);
        if (n < 512) break;
    }
}

void dsn_core_copy_screens(const DsnCore *c, uint32_t *out) {
    memcpy(out, c->s->fb[0], sizeof c->s->fb[0]);
    memcpy(out + SCREEN_W * SCREEN_H, c->s->fb[1], sizeof c->s->fb[1]);
}

uint32_t dsn_core_frame_count(const DsnCore *c) { return c->s->frame; }

int dsn_core_stopped(const DsnCore *c) { return (c->s->arm9.stop ? 1 : 0) | (c->s->arm7.stop ? 2 : 0); }

void dsn_core_describe(const DsnCore *c, char *buf, int len) {
    const Nds *s = c->s;
    snprintf(buf, (size_t)len, "frames %u, ARM9: %llu cycles, %llu dispatches, pc %08x%s; ARM7: %llu cycles, %llu dispatches, pc %08x%s",
             s->frame, (unsigned long long)s->arm9.cycles, (unsigned long long)s->arm9.dispatches, s->arm9.r[15],
             s->arm9.stop ? " (stopped)" : "", (unsigned long long)s->arm7.cycles,
             (unsigned long long)s->arm7.dispatches, s->arm7.r[15], s->arm7.stop ? " (stopped)" : "");
}

uint32_t dsn_core_audio_read(DsnCore *c, float *left, float *right, uint32_t frames) {
    u32 r = atomic_load_explicit(&c->ring_r, memory_order_relaxed);
    u32 w = atomic_load_explicit(&c->ring_w, memory_order_acquire);
    u32 n = w - r < frames ? w - r : frames;
    for (u32 i = 0; i < n; i++) {
        left[i] = c->ring[(r + i) % RING_FRAMES][0] * (1.0f / 32768.0f);
        right[i] = c->ring[(r + i) % RING_FRAMES][1] * (1.0f / 32768.0f);
    }
    atomic_store_explicit(&c->ring_r, r + n, memory_order_release);
    for (u32 i = n; i < frames; i++) left[i] = right[i] = 0.0f;
    return n;
}

int dsn_core_flush_save(DsnCore *c) { return c->shut ? 0 : save_flush(c->s); }

void dsn_core_shutdown(DsnCore *c) {
    if (c->shut) return;
    c->shut = 1;
    nds_shutdown(c->s);
}
