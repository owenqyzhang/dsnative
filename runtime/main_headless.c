/* dsnative: a headless driver. Runs a recompiled game for a number of frames, prints the
 * game's debug output (no$gba port) and writes both screens to a PNG.
 *
 *   dsn_headless <game.nds> <arm9.bin> [--frames N] [--png out.png] [--keys F:KEYS,...]
 *                [--touch F:X:Y,...] [--stats] [--no-interp | --interp-only]
 *                [--hash F,F,...|every:K] [--bench K] [--prof A:B:FILE]
 *
 * arm9.bin is the decompressed ARM9 image the recompiler wrote. --keys holds the buttons
 * pressed from frame F on (letters a b l r x y s=start e=select and the arrows u d L R as
 * U D W E; an empty set releases them). Code with no compiled entry (modified at run time, or
 * missed by discovery) runs in the interpreter and is logged once to missing.txt; --no-interp
 * stops at it instead, to find the gaps; --interp-only ignores the compiled code and
 * interprets everything (a check of the interpreter against the whole machine);
 * --stats prints how many instructions were interpreted. --hash prints, after the given frames
 * have run, hashes of both screens, of the machine's memory and CPU state (registers and cycle
 * counts) and of the audio so far: a change of the generated code or the runtime that must not
 * change the emulation shows as an identical line. --bench K prints the wall time every K
 * frames. --prof samples where the time goes while frames A to B-1 run (macOS: about 1 kHz, the
 * program counter and its callers), and writes the addresses to FILE. tools/bench.py drives
 * all three and turns the samples into a table.
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "nds.h"

extern const DispatchEntry g9_table[];
extern const u32 g9_table_len;
extern const DispatchEntry g7_table[];
extern const u32 g7_table_len;
extern const PicEntry g9_pic[];
extern const u32 g9_pic_len;
extern const PicEntry g7_pic[];
extern const u32 g7_pic_len;

static u8 *read_file(const char *path, u32 *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    u8 *b = malloc((size_t)n + 4);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "cannot read %s\n", path); exit(1); }
    fclose(f);
    *len = (u32)n;
    return b;
}

/* ---- PNG (stored deflate blocks: no compression library needed) ---- */
static u32 crc_table[256];
static void crc_init(void) {
    for (u32 n = 0; n < 256; n++) { u32 c = n; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; crc_table[n] = c; }
}
static u32 crc32b(u32 crc, const u8 *p, size_t n) { crc ^= 0xFFFFFFFFu; while (n--) crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8); return crc ^ 0xFFFFFFFFu; }
static void be32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
static void chunk(FILE *f, const char *type, const u8 *data, u32 len) {
    u8 h[8]; be32(h, len); memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f); if (len) fwrite(data, 1, len, f);
    u32 c = crc32b(0, (const u8 *)type, 4); c = crc32b(c, data, len);
    u8 t[4]; be32(t, c); fwrite(t, 1, 4, f);
}
static void write_png(const char *path, const u32 *px, int w, int h) {
    crc_init();
    size_t raw_len = (size_t)h * (1 + (size_t)w * 3);
    u8 *raw = malloc(raw_len);
    for (int y = 0; y < h; y++) {
        u8 *r = raw + (size_t)y * (1 + (size_t)w * 3);
        r[0] = 0;
        for (int x = 0; x < w; x++) { u32 c = px[y * w + x]; r[1 + x * 3] = (u8)(c >> 16); r[2 + x * 3] = (u8)(c >> 8); r[3 + x * 3] = (u8)c; }
    }
    size_t nblk = (raw_len + 65534) / 65535;
    size_t zlen = 2 + raw_len + nblk * 5 + 4;
    u8 *z = malloc(zlen), *p = z;
    *p++ = 0x78; *p++ = 0x01;
    u32 a = 1, b = 0;
    for (size_t off = 0; off < raw_len; off += 65535) {
        size_t n = raw_len - off < 65535 ? raw_len - off : 65535;
        *p++ = off + n == raw_len; *p++ = (u8)n; *p++ = (u8)(n >> 8); *p++ = (u8)~n; *p++ = (u8)(~n >> 8);
        memcpy(p, raw + off, n); p += n;
    }
    for (size_t i = 0; i < raw_len; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    be32(p, (b << 16) | a); p += 4;
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return; }
    static const u8 sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    u8 ihdr[13]; be32(ihdr, (u32)w); be32(ihdr + 4, (u32)h); ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, (u32)(p - z));
    chunk(f, "IEND", 0, 0);
    fclose(f);
    free(raw); free(z);
}

/* ---- hashes of what the emulation produced (--hash) ---- */
static u64 fnv(u64 h, const void *p, size_t n) {
    const u8 *b = p;
    while (n--) h = (h ^ *b++) * 0x100000001B3ull;
    return h;
}
#define FNV0 0xCBF29CE484222325ull

static u64 state_hash(const Nds *s) {
    u64 h = FNV0;
    h = fnv(h, s->ram, 0x400000); h = fnv(h, s->vram, 0xA4000);
    h = fnv(h, s->pal, sizeof s->pal); h = fnv(h, s->oam, sizeof s->oam);
    h = fnv(h, s->itcm, sizeof s->itcm); h = fnv(h, s->dtcm, sizeof s->dtcm);
    h = fnv(h, s->swram, sizeof s->swram); h = fnv(h, s->wram7, sizeof s->wram7);
    for (int k = 0; k < 2; k++) {
        const Cpu *c = k ? &s->arm7 : &s->arm9;
        h = fnv(h, c->r, sizeof c->r);
        u32 st[8] = {c->fn, c->fz, c->fc, c->fv, c->fq, c->t, c->mode, c->irq_off};
        h = fnv(h, st, sizeof st);
        h = fnv(h, &c->cycles, sizeof c->cycles);
    }
    return h;
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
static double cpu_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int hash_wanted(const char *spec, int frame) {
    if (!strncmp(spec, "every:", 6)) { int k = atoi(spec + 6); return k > 0 && frame % k == 0; }
    for (const char *p = spec; *p;) {
        if (atoi(p) == frame) return 1;
        const char *comma = strchr(p, ',');
        if (!comma) break;
        p = comma + 1;
    }
    return 0;
}

/* ---- a sampling profiler for a window of frames (--prof; macOS): a thread wakes every millisecond,
 * suspends the emulation thread and reads its registers, as `sample` does. A sample is the program
 * counter, the link register and the frame-pointer chain (the callers of a leaf in libc);
 * tools/bench.py maps the addresses to functions. (A SIGPROF timer would not do: XNU only checks it
 * when a thread enters or leaves the kernel, which skews the samples towards system calls.) ---- */
#ifdef __APPLE__
#include <mach/mach.h>
#include <pthread.h>
#include <dlfcn.h>
#include <unistd.h>

#define PROF_CAP (1u << 21)
#define PROF_DEPTH 8            /* words a sample takes: pc, lr, six return addresses */
int main(int argc, char **argv);
static uintptr_t *prof_pcs;
static u32 prof_n;
static mach_port_t prof_target;
static volatile int prof_on, prof_quit;
static pthread_t prof_thread;

static void *prof_sampler(void *arg) {
    (void)arg;
    while (!prof_quit) {
        usleep(1000);
        if (!prof_on || prof_n + PROF_DEPTH > PROF_CAP) continue;
        if (thread_suspend(prof_target) != KERN_SUCCESS) continue;
        uintptr_t pc = 0, lr = 0, fp = 0, sp = 0;
        mach_msg_type_number_t n;
        int ok;
#if defined(__aarch64__)
        arm_thread_state64_t st; n = ARM_THREAD_STATE64_COUNT;
        ok = thread_get_state(prof_target, ARM_THREAD_STATE64, (thread_state_t)&st, &n) == KERN_SUCCESS;
        if (ok) { pc = (uintptr_t)arm_thread_state64_get_pc(st); lr = (uintptr_t)arm_thread_state64_get_lr(st);
                  fp = (uintptr_t)arm_thread_state64_get_fp(st); sp = (uintptr_t)arm_thread_state64_get_sp(st); }
#else
        x86_thread_state64_t st; n = x86_THREAD_STATE64_COUNT;
        ok = thread_get_state(prof_target, x86_THREAD_STATE64, (thread_state_t)&st, &n) == KERN_SUCCESS;
        if (ok) { pc = (uintptr_t)st.__rip; fp = (uintptr_t)st.__rbp; sp = (uintptr_t)st.__rsp; }
#endif
        if (ok) {
            uintptr_t *w = prof_pcs + prof_n;
            prof_n += PROF_DEPTH;
            w[0] = pc; w[1] = lr;
            for (int i = 2; i < PROF_DEPTH; i++) {      /* each frame record: the caller's fp, then the return address */
                w[i] = 0;
                if (!fp || (fp & 15) || fp < sp || fp - sp > (1u << 24)) continue;
                w[i] = ((uintptr_t *)fp)[1];
                uintptr_t next = ((uintptr_t *)fp)[0];
                fp = next > fp ? next : 0;
            }
        }
        thread_resume(prof_target);
    }
    return 0;
}

static void prof_run(int on) {
    if (on && !prof_thread) {
        prof_pcs = malloc(PROF_CAP * sizeof *prof_pcs);
        prof_target = mach_thread_self();
        pthread_create(&prof_thread, 0, prof_sampler, 0);
    }
    prof_on = on;
}

/* The samples as text: the address of main (the load slide), then one line a sample. A pc outside this
 * executable (libc, the kernel) is followed by its symbol, which dladdr finds. */
static void prof_write(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return; }
    fprintf(f, "main %llx\n", (unsigned long long)(uintptr_t)&main);
    Dl_info self; dladdr((void *)&main, &self);
    for (u32 i = 0; i + PROF_DEPTH <= prof_n; i += PROF_DEPTH) {
        for (int k = 0; k < PROF_DEPTH; k++) fprintf(f, "%llx ", (unsigned long long)prof_pcs[i + k]);
        Dl_info di;
        if (dladdr((void *)prof_pcs[i], &di) && di.dli_fbase != self.dli_fbase && di.dli_sname) fprintf(f, "%s", di.dli_sname);
        fprintf(f, "\n");
    }
    fclose(f);
}
#else
static void prof_run(int on) { (void)on; }
static void prof_write(const char *path) { fprintf(stderr, "--prof %s: only on macOS\n", path); }
#endif

static u16 parse_keys(const char *s, const char *end) {
    u16 k = 0;
    for (; s < end; s++) switch (*s) {
        case 'a': k |= KEY_A; break; case 'b': k |= KEY_B; break; case 'x': k |= KEY_X; break; case 'y': k |= KEY_Y; break;
        case 'l': k |= KEY_L; break; case 'r': k |= KEY_R; break; case 's': k |= KEY_START; break; case 'e': k |= KEY_SELECT; break;
        case 'U': k |= KEY_UP; break; case 'D': k |= KEY_DOWN; break; case 'W': k |= KEY_LEFT; break; case 'E': k |= KEY_RIGHT; break;
    }
    return k;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s game.nds arm9.bin [--frames N] [--png out.png] [--keys F:KEYS,...] [--touch F:X:Y,...] [--stats] [--no-interp | --interp-only]\n"
                        "       [--hash F,F,...|every:K] [--bench K] [--prof A:B:FILE]\n"
                        "       [--<subsystem option> value ...]\n", argv[0]);
        return 2;
    }
    int frames = 60, stats = 0, no_interp = 0, interp_only = 0, bench = 0, prof_a = -1, prof_b = -1;
    const char *png = 0, *keys = "", *touch = "", *hash = "", *prof_path = 0;
    const char *opts[64]; int nopts = 0;     /* the subsystems' options, as name, value pairs */
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--png") && i + 1 < argc) png = argv[++i];
        else if (!strcmp(argv[i], "--keys") && i + 1 < argc) keys = argv[++i];
        else if (!strcmp(argv[i], "--touch") && i + 1 < argc) touch = argv[++i];
        else if (!strcmp(argv[i], "--stats")) stats = 1;
        else if (!strcmp(argv[i], "--no-interp")) no_interp = 1;
        else if (!strcmp(argv[i], "--interp-only")) interp_only = 1;
        else if (!strcmp(argv[i], "--hash") && i + 1 < argc) hash = argv[++i];
        else if (!strcmp(argv[i], "--bench") && i + 1 < argc) bench = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--prof") && i + 1 < argc) {
            char path[1024];
            if (sscanf(argv[++i], "%d:%d:%1023s", &prof_a, &prof_b, path) != 3) { fprintf(stderr, "--prof A:B:FILE\n"); return 2; }
            prof_path = strdup(path);
        }
        else if (!strncmp(argv[i], "--", 2) && i + 1 < argc && nopts < 64) { opts[nopts++] = argv[i]; opts[nopts++] = argv[++i]; }
        else { fprintf(stderr, "unknown argument %s\n", argv[i]); return 2; }
    }
    u32 rom_len, a9_len;
    u8 *rom = read_file(argv[1], &rom_len);
    u8 *a9 = read_file(argv[2], &a9_len);
    Nds *s = nds_create(rom, rom_len);
    for (int i = 0; i < nopts; i += 2)
        if (!nds_option(s, opts[i], opts[i + 1])) { fprintf(stderr, "unknown option %s\n", opts[i]); return 2; }
    s->arm9.table = g9_table; s->arm9.table_len = g9_table_len;
    s->arm7.table = g7_table; s->arm7.table_len = g7_table_len;
    s->arm9.no_interp = s->arm7.no_interp = (u32)no_interp;
    s->arm9.pic = g9_pic; s->arm9.pic_len = g9_pic_len;
    s->arm7.pic = g7_pic; s->arm7.pic_len = g7_pic_len;
    if (interp_only) s->arm9.table_len = s->arm7.table_len = s->arm9.pic_len = s->arm7.pic_len = 0;
    nds_direct_boot(s, a9, a9_len);
    u64 audio_hash = FNV0;
    u32 audio_frames = 0;
    double t_start = now_s(), c_start = cpu_s(), t_mark = t_start, c_mark = c_start, skipped_wall = 0, skipped_cpu = 0;
    for (int f = 0; f < frames; f++) {
        /* input script */
        for (const char *p = keys; *p;) {
            const char *colon = strchr(p, ':'), *comma = strchr(p, ',');
            if (!comma) comma = p + strlen(p);
            if (colon && colon < comma && atoi(p) == f) s->keys = parse_keys(colon + 1, comma);
            p = *comma ? comma + 1 : comma;
        }
        for (const char *p = touch; *p;) {
            const char *comma = strchr(p, ','); if (!comma) comma = p + strlen(p);
            int tf, tx, ty;
            if (sscanf(p, "%d:%d:%d", &tf, &tx, &ty) == 3 && tf == f) { s->touch_down = tx >= 0; s->touch_x = tx; s->touch_y = ty; }
            p = *comma ? comma + 1 : comma;
        }
        if (prof_path && f == prof_a) prof_run(1);
        nds_run_frame(s);
        if (prof_path && f + 1 == prof_b) prof_run(0);
        double t_frame = now_s(), c_frame = cpu_s();    /* the hashes below are not part of the frame's time */
        if (*hash) {
            s16 buf[2048];
            for (u32 n; (n = nds_audio_take(s, buf, 1024)) != 0;) { audio_hash = fnv(audio_hash, buf, n * 4); audio_frames += n; }
            if (hash_wanted(hash, f + 1))
                printf("hash %d top %016llx bottom %016llx state %016llx audio %016llx (%u)\n", f + 1,
                       (unsigned long long)fnv(FNV0, s->fb[0], sizeof s->fb[0]), (unsigned long long)fnv(FNV0, s->fb[1], sizeof s->fb[1]),
                       (unsigned long long)state_hash(s), (unsigned long long)audio_hash, audio_frames);
        }
        if (bench && (f + 1) % bench == 0) {
            fprintf(stderr, "bench frame %d wall %.5f cpu %.5f interval %.5f cpu_interval %.5f\n", f + 1, t_frame - t_start - skipped_wall,
                    c_frame - c_start - skipped_cpu, t_frame - t_mark, c_frame - c_mark);
        }
        double t_after = now_s(), c_after = cpu_s();
        skipped_wall += t_after - t_frame; skipped_cpu += c_after - c_frame;
        if (bench && (f + 1) % bench == 0) { t_mark = t_after; c_mark = c_after; }
        if (s->arm9.stop && s->arm7.stop) break;
    }
    if (prof_path) { prof_run(0); prof_write(prof_path); }
    if (png) {
        static u32 both[SCREEN_W * SCREEN_H * 2];
        memcpy(both, s->fb[0], sizeof s->fb[0]);
        memcpy(both + SCREEN_W * SCREEN_H, s->fb[1], sizeof s->fb[1]);
        write_png(png, both, SCREEN_W, SCREEN_H * 2);
    }
    if (stats) {
        fprintf(stderr, "frames %u, ARM9: %llu cycles, %llu dispatches, pc %08x%s; ARM7: %llu cycles, %llu dispatches, pc %08x%s\n",
                s->frame, (unsigned long long)s->arm9.cycles, (unsigned long long)s->arm9.dispatches, s->arm9.r[15], s->arm9.stop ? " (stopped)" : "",
                (unsigned long long)s->arm7.cycles, (unsigned long long)s->arm7.dispatches, s->arm7.r[15], s->arm7.stop ? " (stopped)" : "");
        fprintf(stderr, "interpreted: ARM9 %llu instructions in %llu entries, ARM7 %llu instructions in %llu entries\n",
                (unsigned long long)s->arm9.interp_insns, (unsigned long long)s->arm9.interp_calls,
                (unsigned long long)s->arm7.interp_insns, (unsigned long long)s->arm7.interp_calls);
        for (int k = 0; k < 2; k++) {
            const Cpu *c = k ? &s->arm7 : &s->arm9;
            fprintf(stderr, "stats ARM%d dispatches %llu slow_lookups %llu slow_reads %llu slow_writes %llu\n", k ? 7 : 9,
                    (unsigned long long)c->dispatches, (unsigned long long)c->lookups_slow,
                    (unsigned long long)c->slow_reads, (unsigned long long)c->slow_writes);
        }
    }
    nds_shutdown(s);
    return (s->arm9.stop || s->arm7.stop) ? 1 : 0;
}
