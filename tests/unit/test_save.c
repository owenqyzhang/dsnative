/* Save memory on the AUXSPI bus (save.c): each chip type's commands driven through the I/O
 * registers as a game drives them, chip select, EXMEMCNT, the infrared prefix, the choice of
 * chip and the save file across two runs. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#define access unistd_access    /* this file's access() sends a command */
#include <unistd.h>
#undef access
#include "nds.h"

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("%s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* What the runtime prints to stderr between err_start and err_end (the save's own lines). */
static int err_saved;
static void err_start(void) {
    fflush(stderr);
    err_saved = dup(2);
    int fd = open("test_save.err", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    dup2(fd, 2);
    close(fd);
}
static const char *err_end(void) {
    static char buf[1024];
    fflush(stderr);
    dup2(err_saved, 2);
    close(err_saved);
    FILE *f = fopen("test_save.err", "rb");
    size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    if (f) fclose(f);
    buf[n] = 0;
    remove("test_save.err");
    return buf;
}
static int lines(const char *t) { int n = 0; for (; *t; t++) n += *t == '\n'; return n; }

static Nds *machine(const char *code, const char *type, const char *path) {
    u8 *rom = calloc(1, 0x1000);
    memcpy(rom + 0x0C, code, 4);
    Nds *s = nds_create(rom, 0x1000);
    if (type && !nds_option(s, "--save-type", type)) { printf("--save-type %s refused\n", type); exit(1); }
    if (path && !nds_option(s, "--save", path)) { printf("--save refused\n"); exit(1); }
    return s;
}

static Cpu *owner(Nds *s) { return (s->exmemcnt >> 11) & 1 ? &s->arm7 : &s->arm9; }

static int ir_prefix;           /* infrared cartridge: every command starts with 00h */

/* One command as the Nitro SDK sends it: chip select held (bit 6) until the last byte, the
 * busy bit polled after each. Returns the bytes read back in in[]. */
static void spi(Nds *s, const u8 *out, u8 *in, int n) {
    Cpu *c = owner(s);
    if (ir_prefix) { wr16(c, 0x040001A0, 0xA042); wr8(c, 0x040001A2, 0); }
    for (int i = 0; i < n; i++) {
        wr16(c, 0x040001A0, 0xA000 | (i < n - 1 ? 0x40 : 0));
        wr16(c, 0x040001A2, out[i]);
        while (rd16(c, 0x040001A0) & 0x80) ;
        u8 r = (u8)rd16(c, 0x040001A2);
        if (in) in[i] = r;
    }
}

static void cmd(Nds *s, u8 c) { spi(s, &c, 0, 1); }
static u8 status(Nds *s) { u8 o[2] = {0x05, 0}, i[2]; spi(s, o, i, 2); return i[1]; }

static u8 buf_out[0x10400], buf_in[0x10400];

/* command, address in abytes bytes (0.5 KB EEPROM: bit 8 goes into the command), then n data
 * bytes (data, or zeros to read); returns the data bytes read back */
static u8 *access(Nds *s, u8 c, int abytes, u32 a, const u8 *data, int n) {
    int h = 0;
    buf_out[h++] = abytes == 1 ? (u8)(c | ((a >> 8 & 1) << 3)) : c;
    for (int k = abytes - 1; k >= 0; k--) buf_out[h++] = (u8)(a >> (8 * k));
    for (int i = 0; i < n; i++) buf_out[h + i] = data ? data[i] : 0;
    spi(s, buf_out, buf_in, h + n);
    return buf_in + h;
}
static u8 peek(Nds *s, int abytes, u32 a) { return access(s, 0x03, abytes, a, 0, 1)[0]; }
static void put(Nds *s, u8 c, int abytes, u32 a, const u8 *data, int n) { cmd(s, 0x06); access(s, c, abytes, a, data, n); }

/* ---- FLASH ---- */
static void test_flash(const char *name, u32 size, u8 id0, u8 id1, u8 id2) {
    Nds *s = machine("DSNT", name, 0);
    u8 o[4] = {0x9F, 0, 0, 0}, in[4];
    spi(s, o, in, 4);
    CHECK(in[1] == id0 && in[2] == id1 && in[3] == id2, "%s: RDID %02x %02x %02x", name, in[1], in[2], in[3]);
    CHECK(s->save.size == size, "%s: size %x", name, s->save.size);
    CHECK(status(s) == 0, "%s: status after power-up %02x", name, status(s));
    cmd(s, 0x06);
    CHECK(status(s) == 0x02, "%s: WREN: status %02x", name, status(s));
    CHECK(status(s) == 0x02, "%s: RDSR changes nothing", name);
    cmd(s, 0x04);
    CHECK(status(s) == 0, "%s: WRDI: status %02x", name, status(s));
    CHECK(peek(s, 3, 0x100) == 0xFF && peek(s, 3, size - 1) == 0xFF, "%s: starts erased", name);

    static const u8 d4[4] = {1, 2, 3, 4}, f0[2] = {0xF0, 0xF0};
    access(s, 0x02, 3, 0x100, d4, 4);               /* no WREN */
    CHECK(peek(s, 3, 0x100) == 0xFF, "%s: PP without WREN wrote", name);
    put(s, 0x02, 3, 0x100, d4, 4);
    u8 *r = access(s, 0x03, 3, 0x100, 0, 5);
    CHECK(r[0] == 1 && r[1] == 2 && r[2] == 3 && r[3] == 4 && r[4] == 0xFF, "%s: PP: %02x %02x %02x %02x %02x", name, r[0], r[1], r[2], r[3], r[4]);
    CHECK(status(s) == 0, "%s: WEL stays set after PP", name);
    put(s, 0x02, 3, 0x100, f0, 2);                  /* program only clears bits */
    r = access(s, 0x03, 3, 0x100, 0, 2);
    CHECK(r[0] == 0 && r[1] == 0, "%s: PP over data: %02x %02x", name, r[0], r[1]);
    put(s, 0x0A, 3, 0x100, f0, 1);                  /* page write replaces */
    r = access(s, 0x03, 3, 0x100, 0, 2);
    CHECK(r[0] == 0xF0 && r[1] == 0, "%s: PW over data: %02x %02x", name, r[0], r[1]);
    CHECK(status(s) == 0, "%s: WEL stays set after PW", name);

    /* page boundaries: a write wraps inside its page */
    static const u8 w4[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    put(s, 0x0A, 3, 0x2FE, w4, 4);
    r = access(s, 0x03, 3, 0x200, 0, 0x101);
    CHECK(r[0xFE] == 0xAA && r[0xFF] == 0xBB && r[0] == 0xCC && r[1] == 0xDD && r[2] == 0xFF && r[0x100] == 0xFF,
          "%s: PW across a page end: %02x %02x %02x %02x %02x", name, r[0xFE], r[0xFF], r[0], r[1], r[0x100]);
    /* more than a page: only the last 256 bytes are programmed */
    static u8 big[257];
    memset(big, 0xFF, sizeof big); big[0] = 0x00; big[1] = 0x12; big[256] = 0x5A;
    put(s, 0x02, 3, 0x400, big, 257);
    r = access(s, 0x03, 3, 0x400, 0, 2);
    CHECK(r[0] == 0x5A && r[1] == 0x12, "%s: PP of 257 bytes: %02x %02x", name, r[0], r[1]);

    /* reads run on through the chip and wrap at its end; addresses past it mirror */
    static const u8 e2[2] = {0x11, 0x22}, z1[1] = {0x33};
    put(s, 0x0A, 3, size - 2, e2, 2);
    put(s, 0x0A, 3, 0, z1, 1);
    r = access(s, 0x03, 3, size - 2, 0, 3);
    CHECK(r[0] == 0x11 && r[1] == 0x22 && r[2] == 0x33, "%s: read across the end: %02x %02x %02x", name, r[0], r[1], r[2]);
    CHECK(peek(s, 3, (size | (size - 2)) & 0xFFFFFF) == 0x11, "%s: address mirror", name);
    r = access(s, 0x0B, 3, size - 2, 0, 3);         /* FAST READ: a dummy byte first */
    CHECK(r[1] == 0x11 && r[2] == 0x22, "%s: FAST READ: %02x %02x", name, r[1], r[2]);

    /* page erase: the page holding the address, nothing around it */
    static const u8 x1[1] = {0x77};
    put(s, 0x0A, 3, 0x1FF, x1, 1);
    put(s, 0x0A, 3, 0x300, x1, 1);
    access(s, 0xDB, 3, 0x2FF, 0, 0);                /* no WREN */
    CHECK(peek(s, 3, 0x200) == 0xCC, "%s: PE without WREN erased", name);
    cmd(s, 0x06); access(s, 0xDB, 3, 0x2FF, 0, 0);
    r = access(s, 0x03, 3, 0x1FF, 0, 0x102);
    CHECK(r[0] == 0x77 && r[1] == 0xFF && r[2] == 0xFF && r[0x100] == 0xFF && r[0x101] == 0x77,
          "%s: PE: %02x %02x %02x %02x", name, r[0], r[1], r[0x100], r[0x101]);
    CHECK(status(s) == 0, "%s: WEL stays set after PE", name);
    /* an erase whose address isn't complete doesn't run, and leaves WEL */
    cmd(s, 0x06);
    u8 pe[3] = {0xDB, 0x00, 0x01};
    spi(s, pe, 0, 3);
    CHECK(peek(s, 3, 0x100) == 0xF0 && status(s) == 0x02, "%s: PE with a short address", name);
    cmd(s, 0x04);

    /* sector erase: 64 KB */
    put(s, 0x0A, 3, 0xFFFF, x1, 1);
    put(s, 0x0A, 3, 0x10000, x1, 1);
    put(s, 0x0A, 3, 0x1FFFF, x1, 1);
    put(s, 0x0A, 3, 0x20000, x1, 1);
    put(s, 0xD8, 3, 0x12345, 0, 0);
    CHECK(peek(s, 3, 0xFFFF) == 0x77 && peek(s, 3, 0x10000) == 0xFF && peek(s, 3, 0x1FFFF) == 0xFF && peek(s, 3, 0x20000) == 0x77,
          "%s: SE", name);
    /* chip erase */
    put(s, 0xC7, 0, 0, 0, 0);
    CHECK(peek(s, 3, 0xFFFF) == 0xFF && peek(s, 3, 0x100) == 0xFF && peek(s, 3, size - 2) == 0xFF && status(s) == 0, "%s: chip erase", name);

    /* deep power-down: everything but ABh is ignored */
    put(s, 0x0A, 3, 0x10, x1, 1);
    cmd(s, 0xB9);
    spi(s, o, in, 4);
    CHECK(in[1] == 0xFF && in[2] == 0xFF, "%s: RDID while powered down", name);
    CHECK(peek(s, 3, 0x10) == 0xFF && status(s) == 0xFF, "%s: read while powered down", name);
    cmd(s, 0x06);
    cmd(s, 0xAB);
    CHECK(status(s) == 0 && peek(s, 3, 0x10) == 0x77, "%s: after release from power-down: %02x", name, status(s));
    CHECK(s->save.dirty, "%s: not dirty after writes", name);
}

/* ---- EEPROM and FRAM ---- */
static void test_eeprom512(void) {
    Nds *s = machine("DSNT", "eeprom512", 0);
    CHECK(status(s) == 0xF0, "status %02x", status(s));
    cmd(s, 0x06);
    CHECK(status(s) == 0xF2, "WREN: status %02x", status(s));
    cmd(s, 0x04);
    CHECK(status(s) == 0xF0, "WRDI: status %02x", status(s));
    u8 o[4] = {0x9F, 0, 0, 0}, in[4];
    spi(s, o, in, 4);
    CHECK(in[1] == 0xFF && in[2] == 0xFF && in[3] == 0xFF, "RDID %02x", in[1]);

    static const u8 ab[2] = {'a', 'b'}, c1[1] = {'c'}, d4[4] = {1, 2, 3, 4};
    put(s, 0x02, 1, 0x010, ab, 2);                  /* WRLO */
    put(s, 0x02, 1, 0x110, c1, 1);                  /* WRHI (0Ah) */
    CHECK(status(s) == 0xF0, "WEL after a write");
    u8 *r = access(s, 0x03, 1, 0x010, 0, 2);
    CHECK(r[0] == 'a' && r[1] == 'b', "RDLO %02x %02x", r[0], r[1]);
    CHECK(peek(s, 1, 0x110) == 'c' && peek(s, 1, 0x111) == 0xFF, "RDHI");
    access(s, 0x02, 1, 0x020, ab, 1);               /* no WREN */
    CHECK(peek(s, 1, 0x020) == 0xFF, "write without WREN");
    /* 16-byte pages: writes wrap inside the page, reads run on through all 512 bytes */
    put(s, 0x02, 1, 0x01E, d4, 4);
    r = access(s, 0x03, 1, 0x010, 0, 0x10);
    CHECK(r[0xE] == 1 && r[0xF] == 2 && r[0] == 3 && r[1] == 4 && r[2] == 0xFF, "page wrap %02x %02x %02x %02x", r[0xE], r[0xF], r[0], r[1]);
    CHECK(peek(s, 1, 0x020) == 0xFF, "page wrap wrote the next page");
    static const u8 x1[1] = {0x77}, y1[1] = {0x88};
    put(s, 0x02, 1, 0x1FF, x1, 1);
    put(s, 0x02, 1, 0x000, y1, 1);
    put(s, 0x02, 1, 0x100, y1, 1);
    r = access(s, 0x03, 1, 0x0FF, 0, 2);            /* RDLO runs on into the upper half */
    CHECK(r[0] == 0xFF && r[1] == 0x88, "RDLO into 100h: %02x %02x", r[0], r[1]);
    r = access(s, 0x03, 1, 0x1FF, 0, 2);
    CHECK(r[0] == 0x77 && r[1] == 0x88, "read wraps at 1FFh: %02x %02x", r[0], r[1]);
    /* write protection: WRSR sets the WP bits only, and needs WREN */
    u8 ws[2] = {0x01, 0xFF};
    spi(s, ws, 0, 2);
    CHECK(status(s) == 0xF0, "WRSR without WREN: %02x", status(s));
    cmd(s, 0x06); spi(s, ws, 0, 2);
    CHECK(status(s) == 0xFC, "WRSR: %02x", status(s));
    put(s, 0x02, 1, 0x000, x1, 1);
    CHECK(peek(s, 1, 0x000) == 0x88 && status(s) == 0xFC, "write with all of it protected");
    ws[1] = 0x04; cmd(s, 0x06); spi(s, ws, 0, 2);   /* upper quarter */
    put(s, 0x02, 1, 0x17F, x1, 1);
    put(s, 0x02, 1, 0x180, x1, 1);
    CHECK(peek(s, 1, 0x17F) == 0x77 && peek(s, 1, 0x180) == 0xFF, "upper quarter protected");
}

static void test_eeprom(const char *name, u32 size, u32 page, int abytes, int fram) {
    Nds *s = machine("DSNT", name, 0);
    CHECK(status(s) == 0, "%s: status %02x", name, status(s));
    cmd(s, 0x06);
    CHECK(status(s) == 0x02, "%s: WREN: status %02x", name, status(s));
    cmd(s, 0x04);
    static const u8 d4[4] = {1, 2, 3, 4};
    access(s, 0x02, abytes, 0x40, d4, 1);
    CHECK(peek(s, abytes, 0x40) == 0xFF, "%s: write without WREN", name);
    put(s, 0x02, abytes, 0x40, d4, 4);
    u8 *r = access(s, 0x03, abytes, 0x40, 0, 4);
    CHECK(r[0] == 1 && r[3] == 4, "%s: write %02x %02x", name, r[0], r[3]);
    CHECK(status(s) == 0, "%s: WEL after a write", name);
    CHECK(s->save.size == size, "%s: size %x", name, s->save.size);
    /* a write wraps inside its page (FRAM has none: it runs on through the chip) */
    put(s, 0x02, abytes, page - 2, d4, 4);
    r = access(s, 0x03, abytes, page - 2, 0, 4);
    if (fram) CHECK(r[0] == 1 && r[1] == 2 && r[2] == 3 && r[3] == 4, "%s: FRAM write across 'pages'", name);
    else {
        CHECK(r[0] == 1 && r[1] == 2 && r[2] == 0xFF, "%s: write past the page end went on: %02x %02x %02x", name, r[0], r[1], r[2]);
        CHECK(peek(s, abytes, 0) == 3 && peek(s, abytes, 1) == 4, "%s: write didn't wrap in its page", name);
    }
    /* reads (and FRAM writes) wrap at the end of the chip; the address bits past it mirror */
    put(s, 0x02, abytes, size - 1, d4 + 3, 1);
    r = access(s, 0x03, abytes, size - 1, 0, 2);
    CHECK(r[0] == 4 && r[1] == (fram ? 0xFF : 3), "%s: read across the end: %02x %02x", name, r[0], r[1]);
    if (fram) {
        put(s, 0x02, abytes, size - 1, d4, 2);
        CHECK(peek(s, abytes, size - 1) == 1 && peek(s, abytes, 0) == 2, "%s: FRAM write across the end", name);
    }
    u32 room = abytes == 2 ? 0x10000 : 0x1000000;
    if (room > size) CHECK(peek(s, abytes, size + 0x40) == 1, "%s: address mirror", name);
    /* status register: write protect and SRWD (bit 7) change, the rest don't */
    u8 ws[2] = {0x01, 0xFF};
    cmd(s, 0x06); spi(s, ws, 0, 2);
    CHECK(status(s) == 0x8C, "%s: WRSR: %02x", name, status(s));
    static const u8 x1[1] = {0x77};
    put(s, 0x02, abytes, 0x80, x1, 1);
    CHECK(peek(s, abytes, 0x80) == 0xFF, "%s: write with all of it protected", name);
    ws[1] = 0x08; cmd(s, 0x06); spi(s, ws, 0, 2);   /* upper half */
    put(s, 0x02, abytes, size / 2 - 1, x1, 1);
    put(s, 0x02, abytes, size / 2, x1, 1);
    CHECK(peek(s, abytes, size / 2 - 1) == 0x77 && peek(s, abytes, size / 2) == 0xFF, "%s: upper half protected", name);
    u8 o[2] = {0x9F, 0}, in[2];
    spi(s, o, in, 2);
    CHECK(in[1] == 0xFF, "%s: RDID %02x", name, in[1]);
}

/* ---- the bus ---- */
static void test_chip_select(void) {
    Nds *s = machine("DSNT", "flash512k", 0);
    Cpu *c = &s->arm9;
    static const u8 xy[2] = {'x', 'y'};
    put(s, 0x0A, 3, 0x40, xy, 2);
    /* held: one command; released after the address: the next byte is a new command */
    u8 *r = access(s, 0x03, 3, 0x40, 0, 2);
    CHECK(r[0] == 'x' && r[1] == 'y', "held: %02x %02x", r[0], r[1]);
    u8 rd[4] = {0x03, 0, 0, 0x40}, in[1];
    spi(s, rd, 0, 4);
    spi(s, rd, in, 1);
    CHECK(in[0] == 0xFF, "released: a read went on (%02x)", in[0]);
    /* clearing the hold bit takes effect after the next byte, not at once */
    wr16(c, 0x040001A0, 0xA040);
    for (int i = 0; i < 4; i++) wr8(c, 0x040001A2, rd[i]);
    wr16(c, 0x040001A0, 0xA000);
    wr8(c, 0x040001A2, 0);
    CHECK(rd8(c, 0x040001A2) == 'x', "hold cleared between bytes: %02x", rd8(c, 0x040001A2));
    wr8(c, 0x040001A2, 0);
    CHECK(rd8(c, 0x040001A2) == 0xFF, "the byte after the last went on reading");
    /* libnds holds every byte and ends the command by turning the slot off */
    wr16(c, 0x040001A0, 0xA040); wr8(c, 0x040001A2, 0x06);
    wr16(c, 0x040001A0, 0x0040);
    CHECK(status(s) == 0x02, "WREN ended by turning the slot off: %02x", status(s));
    wr16(c, 0x040001A0, 0xA040); wr8(c, 0x040001A2, 0x04);
    wr16(c, 0x040001A0, 0x8040);   /* serial mode off */
    CHECK(status(s) == 0, "WRDI ended by ROM mode: %02x", status(s));
    /* in ROM mode, AUXSPIDATA starts no transfer */
    wr16(c, 0x040001A0, 0x8000);
    wr8(c, 0x040001A2, 0x06);
    CHECK(status(s) == 0, "a ROM-mode write reached the chip");
    /* the 0.5 KB EEPROM's address bit 8 lives in the command, which counts it once */
    Nds *e = machine("DSNT", "eeprom512", 0);
    static const u8 hi[1] = {0x5A};
    put(e, 0x02, 1, 0x1C0, hi, 1);
    CHECK(peek(e, 1, 0x1C0) == 0x5A && peek(e, 1, 0x0C0) == 0xFF, "0.5 KB EEPROM bit 8");
}

static void test_registers(void) {
    Nds *s = machine("DSNT", "flash512k", 0);
    Cpu *c = &s->arm9;
    wr16(c, 0x040001A0, 0xFFFF);
    CHECK(rd16(c, 0x040001A0) == 0xE043, "AUXSPICNT reads %04x", rd16(c, 0x040001A0));
    CHECK(s->auxspicnt == 0xE043, "card ROM code sees %04x", s->auxspicnt);
    wr8(c, 0x040001A1, 0x40);       /* libnds' AUXSPICNTH: the card IRQ enable */
    CHECK(rd16(c, 0x040001A0) == 0x4043 && rd8(c, 0x040001A1) == 0x40 && rd8(c, 0x040001A0) == 0x43,
          "8-bit AUXSPICNT: %04x", rd16(c, 0x040001A0));
    wr8(c, 0x040001A0, 0x02);
    CHECK(rd16(c, 0x040001A0) == 0x4002, "8-bit AUXSPICNT low: %04x", rd16(c, 0x040001A0));
    /* a 32-bit write sets AUXSPICNT and sends a byte */
    wr32(c, 0x040001A0, 0x009F0000u | 0xA040);
    wr16(c, 0x040001A2, 0);
    CHECK(rd32(c, 0x040001A0) == (0xA040u | 0x20u << 16), "32-bit read: %08x", rd32(c, 0x040001A0));
    CHECK(rd8(c, 0x040001A3) == 0 && rd8(c, 0x040001A2) == 0x20, "AUXSPIDATA bytes");
    wr16(c, 0x040001A0, 0x8000);
    /* the card ROM interrupt still comes when bit 14 is set */
    wr16(c, 0x040001A0, 0xC000);
    s->card_cmd[0] = 0xB8;
    s->ie[0] = IRQ_CARD_DONE;
    wr32(c, 0x040001A4, 0x80000000u | (7u << 24));
    (void)rd32(c, 0x04100010);
    CHECK(s->ifl[0] & IRQ_CARD_DONE, "card ROM transfer IRQ");
}

static void test_exmemcnt(void) {
    Nds *s = machine("DSNT", "flash512k", 0);
    /* the slot is the ARM9's: the ARM7 reads 0 and writes nothing */
    wr16(&s->arm9, 0x040001A0, 0xA000);
    CHECK(rd16(&s->arm7, 0x040001A0) == 0, "ARM7 reads AUXSPICNT %04x", rd16(&s->arm7, 0x040001A0));
    wr16(&s->arm7, 0x040001A0, 0xA000); wr16(&s->arm7, 0x040001A2, 0x06);
    CHECK(status(s) == 0, "the ARM7 reached the ARM9's chip");
    /* given to the ARM7 (EXMEMCNT bit 11, written by the ARM9) */
    wr16(&s->arm9, 0x04000204, 0x6800);
    CHECK(owner(s) == &s->arm7, "EXMEMCNT");
    u8 o[4] = {0x9F, 0, 0, 0}, in[4];
    spi(s, o, in, 4);
    CHECK(in[1] == 0x20 && in[3] == 0x13, "RDID from the ARM7: %02x %02x %02x", in[1], in[2], in[3]);
    cmd(s, 0x06);
    CHECK(status(s) == 0x02, "WREN from the ARM7");
    CHECK(rd16(&s->arm9, 0x040001A0) == 0 && rd8(&s->arm9, 0x040001A2) == 0, "the ARM9 reads the ARM7's slot");
    wr16(&s->arm9, 0x040001A0, 0xA000); wr16(&s->arm9, 0x040001A2, 0x04);
    CHECK(status(s) == 0x02, "the ARM9 reached the ARM7's chip");
    wr16(&s->arm9, 0x04000204, 0x6000);
    CHECK(status(s) == 0x02, "back on the ARM9");
}

static void test_infrared(void) {
    Nds *s = machine("IREO", 0, 0);     /* Pokemon Black 2: FLASH behind an infrared chip */
    Cpu *c = &s->arm9;
    u8 o[4] = {0x9F, 0, 0, 0}, in[4];
    spi(s, o, in, 4);
    CHECK(s->save.type == SAVE_FLASH512K && s->save.ir, "IREO: type %d, ir %d", s->save.type, s->save.ir);
    CHECK(in[1] != 0x20, "RDID without the prefix reached the FLASH");
    u8 ver[2] = {0x08, 0};
    spi(s, ver, in, 2);
    CHECK(in[1] == 0xAA, "IR version %02x", in[1]);
    u8 rx[3] = {0x01, 0, 0};
    spi(s, rx, in, 3);
    CHECK(in[1] == 0, "IR receive length %02x", in[1]);
    ir_prefix = 1;
    spi(s, o, in, 4);
    CHECK(in[1] == 0x20 && in[2] == 0x40 && in[3] == 0x13, "RDID after 00h: %02x %02x %02x", in[1], in[2], in[3]);
    static const u8 d2[2] = {0x12, 0x34};
    put(s, 0x0A, 3, 0x7FF00, d2, 2);
    u8 *r = access(s, 0x03, 3, 0x7FF00, 0, 2);
    CHECK(r[0] == 0x12 && r[1] == 0x34 && status(s) == 0, "write and read after 00h: %02x %02x", r[0], r[1]);
    ir_prefix = 0;
    /* chip select held through an infrared command doesn't reach the FLASH */
    wr16(c, 0x040001A0, 0xA040); wr8(c, 0x040001A2, 0x02); wr8(c, 0x040001A2, 0x06);
    wr16(c, 0x040001A0, 0xA000); wr8(c, 0x040001A2, 0x00);
    ir_prefix = 1;
    CHECK(status(s) == 0, "an infrared transmit set WEL");
    ir_prefix = 0;
}

static void test_choice(void) {
    struct { const char *code, *type; int want, ir, guessed; } t[] = {
        {"DSNT", 0, SAVE_FLASH512K, 0, 1},  /* the default */
        {"IRDO", 0, SAVE_FLASH512K, 1, 0},  /* White 2 */
        {"AMCE", 0, SAVE_FLASH256K, 0, 0},
        {"BKIP", 0, SAVE_FLASH1M, 0, 0},
        {"ARFE", 0, SAVE_EEPROM64K, 0, 0},
        {"VAAV", 0, SAVE_FLASH8M, 0, 0},
        {"CPUE", 0, SAVE_FLASH512K, 0, 0},
        {"IREO", "eeprom64k", SAVE_EEPROM64K, 1, 0},    /* the option beats the table */
        {"DSNT", "none", SAVE_NONE, 0, 0},
        {"DSNT", "flash512k", SAVE_FLASH512K, 0, 0},    /* said, even when it is the default */
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        err_start();
        Nds *s = machine(t[i].code, t[i].type, 0);
        (void)status(s);
        const char *e = err_end();
        CHECK(s->save.type == t[i].want && s->save.ir == t[i].ir, "%s %s: type %d ir %d", t[i].code, t[i].type ? t[i].type : "-", s->save.type, s->save.ir);
        /* a guess is announced (the code and the chip it assumed); what the table, an option or a file said isn't */
        char say[64];
        snprintf(say, sizeof say, "save: game code %s is not known; assuming flash512k", t[i].code);
        CHECK(s->save.guessed == t[i].guessed && (t[i].guessed ? !strncmp(e, say, strlen(say)) && lines(e) == 1 : !*e),
              "%s %s: guessed %d, stderr \"%s\"", t[i].code, t[i].type ? t[i].type : "-", s->save.guessed, e);
    }
    static const char *names[] = {"none", "eeprom512", "eeprom8k", "eeprom64k", "eeprom128k", "flash256k", "flash512k", "flash1m", "flash8m", "fram32k"};
    Nds *s = machine("DSNT", 0, 0);
    for (int k = 0; k < SAVE_TYPES; k++) CHECK(nds_option(s, "--save-type", names[k]), "--save-type %s refused", names[k]);
    CHECK(!nds_option(s, "--save-type", "flash2m"), "--save-type flash2m taken");
    /* no chip: every byte reads FFh */
    Nds *n = machine("DSNT", "none", 0);
    CHECK(status(n) == 0xFF && peek(n, 3, 0) == 0xFF, "no chip");
    static const u8 x1[1] = {0};
    put(n, 0x0A, 3, 0, x1, 1);
    CHECK(!n->save.dirty, "no chip: dirty");
}

/* A game that isn't known, with nothing else to go by: the one line on stderr says what was assumed. */
static void test_guess(void) {
    /* once per chip, however many commands follow; a header byte that isn't printable shows as a dot */
    err_start();
    Nds *s = machine("\0\1A~", 0, 0);
    (void)status(s); (void)status(s); cmd(s, 0x06);
    const char *e = err_end();
    CHECK(!strncmp(e, "save: game code ..A~ is not known; assuming flash512k (", 54) && strstr(e, "--save-type TYPE") && lines(e) == 1,
          "guess line: \"%s\"", e);
    CHECK(s->save.guessed && s->save.type == SAVE_FLASH512K, "guessed %d type %d", s->save.guessed, s->save.type);
    /* the chip is on until the game is done: nothing is said at the first access of a chip set by hand */
    err_start();
    CHECK(nds_option(s, "--save-type", "eeprom64k"), "--save-type refused");
    (void)status(s);
    e = err_end();
    CHECK(!*e && !s->save.guessed && s->save.type == SAVE_EEPROM64K, "option after a guess: \"%s\", guessed %d", e, s->save.guessed);
    /* a save file whose size fits no chip still leaves the type to the default, with its own line too */
    const char *path = "test_guess.sav";
    FILE *f = fopen(path, "wb");
    for (int i = 0; i < 1234; i++) fputc(0xFF, f);
    fclose(f);
    err_start();
    s = machine("DSNT", 0, path);
    (void)status(s);
    e = err_end();
    CHECK(s->save.guessed && strstr(e, "assuming flash512k") && strstr(e, "holds 1234 bytes") && lines(e) == 2, "odd file: \"%s\"", e);
    remove(path);
    /* a file that does fit one gives the type: no guess */
    f = fopen(path, "wb");
    for (int i = 0; i < 0x2000; i++) fputc(0xFF, f);
    fclose(f);
    err_start();
    s = machine("DSNT", 0, path);
    (void)status(s);
    e = err_end();
    CHECK(!s->save.guessed && s->save.type == SAVE_EEPROM8K && !*e, "8 KB file: guessed %d, stderr \"%s\"", s->save.guessed, e);
    remove(path);
}

/* ---- the save file across two runs ---- */
static long size_of(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fclose(f);
    return n;
}

static void test_file(void) {
    const char *path = "test_save.sav";
    remove(path);
    /* run 1: no file yet, so the chip starts erased; the file appears at shutdown */
    Nds *s = machine("DSNT", "flash512k", path);
    CHECK(peek(s, 3, 0x1234) == 0xFF, "a missing file isn't erased memory");
    static const u8 pat[5] = {'d', 's', 'n', 0, 0x5A};
    put(s, 0x0A, 3, 0x1234, pat, 5);
    CHECK(size_of(path) == -1, "written before shutdown");
    nds_shutdown(s);
    CHECK(size_of(path) == 0x80000, "file size %ld", size_of(path));
    FILE *f = fopen(path, "rb");
    static u8 back[0x80000];
    size_t got = f ? fread(back, 1, sizeof back, f) : 0;
    if (f) fclose(f);
    CHECK(got == sizeof back && !memcmp(back + 0x1234, pat, 5) && back[0x1233] == 0xFF && back[0x1239] == 0xFF, "file contents");
    /* run 2 reads it; with nothing changed, shutdown leaves the file alone */
    s = machine("DSNT", "flash512k", path);
    u8 *r = access(s, 0x03, 3, 0x1233, 0, 7);
    CHECK(r[0] == 0xFF && !memcmp(r + 1, pat, 5) && r[6] == 0xFF, "run 2 reads %02x %02x", r[1], r[5]);
    put(s, 0x0A, 3, 0x1234, pat, 1);                /* the same byte again: no change */
    f = fopen(path, "wb"); fputs("marker", f); fclose(f);
    nds_shutdown(s);
    CHECK(size_of(path) == 6, "an unchanged save was written back");
    /* the type comes from the file's size when the game isn't known */
    f = fopen(path, "wb");
    for (int i = 0; i < 0x2000; i++) fputc(i & 0xFF, f);
    fclose(f);
    s = machine("DSNT", 0, path);
    CHECK(peek(s, 2, 0x1FFF) == 0xFF && s->save.type == SAVE_EEPROM8K && !s->save.guessed, "8 KB file: type %d, guessed %d", s->save.type, s->save.guessed);
    /* a file shorter than the chip: the rest is erased, and the whole chip is written back */
    s = machine("DSNT", "eeprom64k", path);
    CHECK(peek(s, 2, 0x1FFF) == 0xFF && peek(s, 2, 0x10) == 0x10 && peek(s, 2, 0x2000) == 0xFF, "short file");
    static const u8 x1[1] = {0x99};
    put(s, 0x02, 2, 0x8000, x1, 1);
    CHECK(save_flush(s) == 0 && size_of(path) == 0x10000, "flush: %ld", size_of(path));
    CHECK(!s->save.dirty, "dirty after flush");
    remove(path);
    /* nothing written: no file */
    s = machine("DSNT", "flash512k", path);
    (void)peek(s, 3, 0);
    nds_shutdown(s);
    CHECK(size_of(path) == -1, "a file appeared with nothing written");
    /* an option after the chip is in use writes it back and starts again */
    s = machine("DSNT", "flash256k", path);
    put(s, 0x0A, 3, 0, x1, 1);
    CHECK(nds_option(s, "--save-type", "flash256k") && size_of(path) == 0x40000, "option mid-run");
    CHECK(peek(s, 3, 0) == 0x99, "re-read after an option");
    remove(path);
    /* a file longer than the chip (an emulator's footer): the tail survives a write-back */
    f = fopen(path, "wb");
    for (int i = 0; i < 0x2000 + 122; i++) fputc(i < 0x2000 ? 0xFF : i & 0x7F, f);
    fclose(f);
    s = machine("DSNT", "eeprom8k", path);
    put(s, 0x02, 2, 0x1FFF, x1, 1);
    nds_shutdown(s);
    f = fopen(path, "rb");
    got = f ? fread(back, 1, sizeof back, f) : 0;
    if (f) fclose(f);
    int tail_ok = got == 0x2000 + 122 && back[0x1FFF] == 0x99 && back[0x1FFE] == 0xFF;
    for (int i = 0x2000; tail_ok && i < 0x2000 + 122; i++) tail_ok = back[i] == (i & 0x7F);
    CHECK(tail_ok, "long file: %zu bytes, %02x", got, back[0x1FFF]);
    /* a file that is there but can't be read is never written over */
    f = fopen(path, "wb"); fputs("unreadable", f); fclose(f);
    chmod(path, 0);
    FILE *probe = fopen(path, "rb");
    if (probe) fclose(probe);       /* root reads it anyway: nothing to test */
    else {
        s = machine("DSNT", "flash512k", path);
        put(s, 0x0A, 3, 0, x1, 1);
        CHECK(peek(s, 3, 0) == 0x99, "unreadable file: the chip still works");
        nds_shutdown(s);
        chmod(path, 0644);
        CHECK(size_of(path) == 10, "an unreadable save was written over");
    }
    chmod(path, 0644);
    remove(path);
}

int main(void) {
    test_flash("flash256k", 0x40000, 0x20, 0x40, 0x12);
    test_flash("flash512k", 0x80000, 0x20, 0x40, 0x13);
    test_flash("flash1m", 0x100000, 0x20, 0x40, 0x14);
    test_flash("flash8m", 0x800000, 0xC2, 0x20, 0x17);
    test_eeprom512();
    test_eeprom("eeprom8k", 0x2000, 32, 2, 0);
    test_eeprom("eeprom64k", 0x10000, 128, 2, 0);
    test_eeprom("eeprom128k", 0x20000, 256, 3, 0);
    test_eeprom("fram32k", 0x8000, 0x100, 2, 1);   /* no pages: 100h is any boundary */
    test_chip_select();
    test_registers();
    test_exmemcnt();
    test_infrared();
    test_choice();
    test_guess();
    test_file();
    if (fails) { printf("%d checks failed\n", fails); return 1; }
    return 0;
}
