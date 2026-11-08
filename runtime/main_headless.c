/* dsnative: a headless driver. Runs a recompiled game for a number of frames, prints the
 * game's debug output (no$gba port) and writes both screens to a PNG.
 *
 *   dsn_headless <game.nds> <arm9.bin> [--frames N] [--png out.png] [--keys F:KEYS,...]
 *                [--touch F:X:Y,...] [--stats] [--no-interp | --interp-only]
 *
 * arm9.bin is the decompressed ARM9 image the recompiler wrote. --keys holds the buttons
 * pressed from frame F on (letters a b l r x y s=start e=select and the arrows u d L R as
 * U D W E; an empty set releases them). Code with no compiled entry (modified at run time, or
 * missed by discovery) runs in the interpreter and is logged once to missing.txt; --no-interp
 * stops at it instead, to find the gaps; --interp-only ignores the compiled code and
 * interprets everything (a check of the interpreter against the whole machine);
 * --stats prints how many instructions were interpreted.
 */
#include <stdio.h>
#include <stdlib.h>
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
                        "       [--<subsystem option> value ...]\n", argv[0]);
        return 2;
    }
    int frames = 60, stats = 0, no_interp = 0, interp_only = 0;
    const char *png = 0, *keys = "", *touch = "";
    const char *opts[64]; int nopts = 0;     /* the subsystems' options, as name, value pairs */
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--png") && i + 1 < argc) png = argv[++i];
        else if (!strcmp(argv[i], "--keys") && i + 1 < argc) keys = argv[++i];
        else if (!strcmp(argv[i], "--touch") && i + 1 < argc) touch = argv[++i];
        else if (!strcmp(argv[i], "--stats")) stats = 1;
        else if (!strcmp(argv[i], "--no-interp")) no_interp = 1;
        else if (!strcmp(argv[i], "--interp-only")) interp_only = 1;
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
        nds_run_frame(s);
        if (s->arm9.stop && s->arm7.stop) break;
    }
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
    }
    nds_shutdown(s);
    return (s->arm9.stop || s->arm7.stop) ? 1 : 0;
}
