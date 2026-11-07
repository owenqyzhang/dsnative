/* dsnative runtime: the cartridge's save memory on the AUXSPI bus. References: GBATEK "DS
 * Cartridge Backup" (chips and commands), "DS Cartridge I/O Ports" (AUXSPICNT, AUXSPIDATA),
 * "DS Firmware Serial Flash Memory" (the FLASH commands, which cartridge FLASH shares) and
 * "DS Cart Infrared Cartridge SPI Commands" (the 00h prefix of infrared cartridges).
 *
 * Transfers finish as soon as AUXSPIDATA is written, and writes and erases as soon as chip
 * select rises, so AUXSPICNT's busy bit and the status register's WIP bit always read 0.
 * Games poll both until they clear, which costs them nothing here. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include "nds.h"

enum { K_NONE, K_EEPROM, K_FRAM, K_FLASH };

static const struct {
    const char *name;
    u32 size, page;             /* page: what one write may cover (it wraps inside) */
    u8 kind, abytes;            /* address bytes after the command */
    u8 id[3];                   /* FLASH: the JEDEC ID (GBATEK's list of cartridge chips) */
} chips[SAVE_TYPES] = {
    [SAVE_NONE] = {"none", 0, 1, K_NONE, 0, {0}},
    [SAVE_EEPROM512] = {"eeprom512", 0x200, 16, K_EEPROM, 1, {0}},
    [SAVE_EEPROM8K] = {"eeprom8k", 0x2000, 32, K_EEPROM, 2, {0}},
    [SAVE_EEPROM64K] = {"eeprom64k", 0x10000, 128, K_EEPROM, 2, {0}},
    [SAVE_EEPROM128K] = {"eeprom128k", 0x20000, 256, K_EEPROM, 3, {0}},   /* page size unknown to GBATEK: ST M95M01's */
    [SAVE_FLASH256K] = {"flash256k", 0x40000, 256, K_FLASH, 3, {0x20, 0x40, 0x12}},   /* ST M45PE20 */
    [SAVE_FLASH512K] = {"flash512k", 0x80000, 256, K_FLASH, 3, {0x20, 0x40, 0x13}},   /* ST 45PE40 */
    [SAVE_FLASH1M] = {"flash1m", 0x100000, 256, K_FLASH, 3, {0x20, 0x40, 0x14}},      /* ST 45PE80 */
    [SAVE_FLASH8M] = {"flash8m", 0x800000, 256, K_FLASH, 3, {0xC2, 0x20, 0x17}},      /* Macronix MX25L6445E */
    [SAVE_FRAM32K] = {"fram32k", 0x8000, 0x8000, K_FRAM, 2, {0}},                     /* no page limit */
};

/* The chip isn't in the ROM, so games whose chip is known are listed here, by the first three
 * letters of the game code (the fourth is the region). */
static const struct { char code[4]; u8 type; } known[] = {
    {"IRE", SAVE_FLASH512K}, {"IRD", SAVE_FLASH512K},     /* Pokemon Black 2, White 2 */
    {"IRB", SAVE_FLASH512K}, {"IRA", SAVE_FLASH512K},     /* Pokemon Black, White */
    {"IPK", SAVE_FLASH512K}, {"IPG", SAVE_FLASH512K},     /* Pokemon HeartGold, SoulSilver */
    {"CPU", SAVE_FLASH512K}, {"ADA", SAVE_FLASH512K}, {"APA", SAVE_FLASH512K},   /* Platinum, Diamond, Pearl */
    {"AMC", SAVE_FLASH256K},    /* Mario Kart DS (GBATEK's examples from here on) */
    {"AZE", SAVE_FLASH512K},    /* Zelda: Phantom Hourglass */
    {"BKI", SAVE_FLASH1M},      /* Zelda: Spirit Tracks */
    {"VAA", SAVE_FLASH8M},      /* Art Academy */
    {"ARF", SAVE_EEPROM64K},    /* Rune Factory, hard to detect from its accesses */
};

/* Neither the table, --save-type nor a save file's size says: what the Pokemon games use. */
#define SAVE_DEFAULT SAVE_FLASH512K

/* ---- the save file -------------------------------------------------------------------- */
/* the file's size: -1 when there is none, -2 when it is there but can't be read */
static long file_size(const char *path) {
    if (!path) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return errno == ENOENT ? -1 : -2;
    long n = fseek(f, 0, SEEK_END) ? -1 : ftell(f);
    fclose(f);
    return n < 0 ? -2 : n;
}

/* Picks the chip and reads the file, at the first access: options can come in any order. */
static Save *ready(Nds *s) {
    Save *sv = &s->save;
    if (sv->ready) return sv;
    sv->ready = 1;
    char code[4] = {0};
    if (s->rom && s->rom_size >= 0x10) memcpy(code, s->rom + 0x0C, 3);
    sv->ir = code[0] == 'I';    /* GBATEK: infrared cartridges' game codes start with I */
    long n = file_size(sv->path);
    if (!sv->type_set) {
        int t = -1;
        for (size_t i = 0; i < sizeof known / sizeof known[0]; i++)
            if (!memcmp(code, known[i].code, 3)) t = known[i].type;
        for (int k = SAVE_EEPROM512; t < 0 && n > 0 && k < SAVE_TYPES; k++)
            if ((long)chips[k].size == n) t = k;
        sv->type = t < 0 ? SAVE_DEFAULT : t;
    }
    sv->size = chips[sv->type].size;
    sv->mem = malloc(sv->size ? sv->size : 1);
    memset(sv->mem, 0xFF, sv->size);    /* erased */
    sv->dirty = 0;
    int ok = n != -2;
    if (n >= 0 && sv->size) {
        /* a longer file (another chip's, or an emulator's footer) keeps its tail */
        size_t want = (size_t)n < sv->size ? (size_t)n : sv->size;
        sv->tail_len = (size_t)n > sv->size ? (u32)(n - sv->size) : 0;
        sv->tail = malloc(sv->tail_len ? sv->tail_len : 1);
        FILE *f = fopen(sv->path, "rb");
        ok = f && fread(sv->mem, 1, want, f) == want && fread(sv->tail, 1, sv->tail_len, f) == sv->tail_len;
        if (f) fclose(f);
        if (ok && (u32)n != sv->size)
            fprintf(stderr, "save: %s holds %ld bytes, the %s chip %u\n", sv->path, n, chips[sv->type].name, sv->size);
    }
    if (!ok) {                      /* never write over a save that couldn't be read */
        fprintf(stderr, "save: cannot read %s; this run's saves will not be written\n", sv->path);
        free(sv->path); sv->path = 0;
    }
    return sv;
}

int save_flush(Nds *s) {
    Save *sv = &s->save;
    if (!sv->dirty || !sv->path || !sv->size) return 0;
    /* write a copy, then replace the file with it, so a failed write leaves the old save */
    size_t len = strlen(sv->path);
    char *tmp = malloc(len + 5);
    memcpy(tmp, sv->path, len); memcpy(tmp + len, ".tmp", 5);
    FILE *f = fopen(tmp, "wb");
    int ok = f && fwrite(sv->mem, 1, sv->size, f) == sv->size &&
             (!sv->tail_len || fwrite(sv->tail, 1, sv->tail_len, f) == sv->tail_len);
    if (f && fclose(f)) ok = 0;
    if (ok && rename(tmp, sv->path)) ok = 0;
    if (!ok) { fprintf(stderr, "save: cannot write %s\n", sv->path); remove(tmp); }
    free(tmp);
    if (!ok) return -1;
    sv->dirty = 0;
    return 0;
}

/* Options take effect at the next access: one given after the chip is in use writes the file
 * back and sets the chip up again. */
static void unready(Nds *s) {
    Save *sv = &s->save;
    if (!sv->ready) return;
    save_flush(s);
    free(sv->mem); sv->mem = 0;
    free(sv->tail); sv->tail = 0; sv->tail_len = 0;
    sv->ready = 0;
}

/* ---- the chip -------------------------------------------------------------------------- */
static void poke(Save *sv, u32 a, u8 v) {
    if (sv->mem[a] != v) { sv->mem[a] = v; sv->dirty = 1; }
}

static void erase(Save *sv, u32 a, u32 len) {
    for (u32 i = 0; i < len; i++) poke(sv, a + i, 0xFF);
}

/* EEPROM and FRAM status bits 2-3: none, the upper quarter, the upper half or all of it is read-only */
static int writable(Save *sv, u32 a) {
    static const u8 shift[4] = {0, 2, 1, 0};
    u32 wp = (sv->status >> 2) & 3;
    return !wp || a < sv->size - (sv->size >> shift[wp]);
}

static u8 rdsr(Save *sv) {
    u8 st = (u8)(sv->wel << 1);     /* WIP (bit 0) stays clear */
    if (chips[sv->type].kind == K_FLASH) return st;
    st |= sv->status;               /* WP (bits 2-3), SRWD (bit 7) */
    return sv->type == SAVE_EEPROM512 ? (u8)(st | 0xF0) : st;
}

/* The 0.5 KB EEPROM's read and write commands carry address bit 8 in bit 3 (03h/0Bh, 02h/0Ah). */
static u8 op_of(Save *sv, u8 c) {
    return sv->type == SAVE_EEPROM512 && (c == 0x0A || c == 0x0B) ? (u8)(c & ~8) : c;
}

static int has_addr(Save *sv, u8 op) {
    if (chips[sv->type].kind != K_FLASH) return op == 0x03 || op == 0x02;
    return op == 0x03 || op == 0x0B || op == 0x02 || op == 0x0A || op == 0xDB || op == 0xD8 ||
           (op == 0x20 && sv->type == SAVE_FLASH8M);   /* the Macronix chip's 4 KB sector erase */
}

/* Writes the latched page: replacing bytes, or for FLASH page program clearing bits only. */
static void commit(Save *sv, int program) {
    for (u32 o = 0; o < chips[sv->type].page; o++) {
        if (!sv->latched[o]) continue;
        u32 a = (sv->latch_base + o) & (sv->size - 1);
        if (writable(sv, a)) poke(sv, a, program ? (u8)(sv->mem[a] & sv->latch[o]) : sv->latch[o]);
    }
}

/* One byte of a command while chip select is low: returns what the chip sends back. Nothing
 * drives the line during the command and address bytes, which then read FFh. */
static u8 chip_byte(Save *sv, u8 v) {
    int kind = chips[sv->type].kind;
    u32 p = sv->pos++, ab = chips[sv->type].abytes;
    if (kind == K_NONE) return 0xFF;
    if (p == 0) { sv->cmd = v; sv->addr = 0; return 0xFF; }
    if (sv->asleep) return 0xFF;    /* deep power-down: only ABh, at chip select rising */
    u8 op = op_of(sv, sv->cmd);
    if (op == 0x05) return rdsr(sv);
    if (op == 0x9F) return kind == K_FLASH && p <= 3 ? chips[sv->type].id[p - 1] : 0xFF;
    if (op == 0x01 && kind != K_FLASH) { if (p == 1) sv->addr = v; return 0xFF; }   /* WRSR's value */
    if (!has_addr(sv, op)) return 0xFF;
    if (p <= ab) {
        sv->addr = ab == 1 ? (u32)(sv->cmd & 8) << 5 | v : sv->addr << 8 | v;
        return 0xFF;
    }
    u32 d = p - ab - 1;             /* data byte */
    if (op == 0x03 || op == 0x0B) {
        if (op == 0x0B && d == 0) return 0xFF;    /* FAST READ's dummy byte */
        return sv->mem[sv->addr++ & (sv->size - 1)];   /* reads run on through the whole chip */
    }
    if ((op == 0x02 || op == 0x0A) && sv->wel) {
        if (kind == K_FRAM) {       /* written at once, on through the whole chip */
            u32 a = sv->addr++ & (sv->size - 1);
            if (writable(sv, a)) poke(sv, a, v);
            return 0xFF;
        }
        u32 page = chips[sv->type].page;
        if (d == 0) { memset(sv->latched, 0, page); sv->latch_base = sv->addr & ~(page - 1); }
        u32 o = sv->addr & (page - 1);
        sv->latch[o] = v; sv->latched[o] = 1;
        sv->addr = sv->latch_base | ((o + 1) & (page - 1));     /* wraps inside the page */
    }
    return 0xFF;
}

/* Chip select rises: the command takes effect once its instruction and address are in. */
static void chip_release(Save *sv) {
    int kind = chips[sv->type].kind;
    u32 p = sv->pos, ab = chips[sv->type].abytes;
    sv->pos = 0;
    if (!p || kind == K_NONE) return;
    u8 op = op_of(sv, sv->cmd);
    if (sv->asleep) { if (kind == K_FLASH && op == 0xAB) sv->asleep = 0; return; }
    if (op == 0x06) { sv->wel = 1; return; }
    if (op == 0x04) { sv->wel = 0; return; }
    if (kind == K_FLASH) {
        u32 len = op == 0xDB ? 0x100 : op == 0xD8 ? 0x10000 : op == 0x20 && has_addr(sv, op) ? 0x1000 : 0;
        if (op == 0xB9) sv->asleep = 1;
        else if (op == 0xC7 || op == 0x60) { if (sv->wel) erase(sv, 0, sv->size); sv->wel = 0; }   /* chip erase */
        else if (len && p > ab) { if (sv->wel) erase(sv, sv->addr & (sv->size - 1) & ~(len - 1), len); sv->wel = 0; }
        else if ((op == 0x02 || op == 0x0A) && p > ab + 1) { if (sv->wel) commit(sv, op == 0x02); sv->wel = 0; }
        return;
    }
    if (op == 0x01 && p > 1) {      /* WRSR: only the write-protect bits and SRWD change */
        if (sv->wel) sv->status = (u8)(sv->addr & (sv->type == SAVE_EEPROM512 ? 0x0C : 0x8C));
        sv->wel = 0;
    } else if (op == 0x02 && p > ab + 1) {
        if (sv->wel && kind == K_EEPROM) commit(sv, 0);
        sv->wel = 0;
    }
}

/* ---- the bus ---------------------------------------------------------------------------- */
static void release(Save *sv) {
    if (!sv->ir || sv->ir_cmd == 0x00) chip_release(sv);
    sv->selected = 0;
}

/* An infrared cartridge's chip takes the first byte as its own command. 00h passes the rest
 * to the save chip; with no infrared traffic, receive (01h) answers length 0, version (08h)
 * answers AAh (the newer firmware) and the rest read 0. */
static u8 bus_byte(Save *sv, u8 v, int first) {
    if (!sv->ir) return chip_byte(sv, v);
    if (first) { sv->ir_cmd = v; sv->ir_pos = 0; return 0; }
    sv->ir_pos++;
    if (sv->ir_cmd == 0x00) return chip_byte(sv, v);
    return sv->ir_cmd == 0x08 && sv->ir_pos == 1 ? 0xAA : 0;
}

static void cnt_write(Nds *s, u16 v) {
    Save *sv = &s->save;
    sv->cnt = v & 0xE043;           /* baud rate, hold, slot mode, IRQ, enable; busy is read-only */
    s->auxspicnt = sv->cnt;         /* the card ROM code reads its IRQ enable */
    /* chip select rises with the slot or serial mode off; a cleared hold bit only takes
     * effect after the next byte, which is how the last byte of a command is sent */
    if (sv->selected && (sv->cnt & 0xA000) != 0xA000) release(sv);
}

static void data_write(Nds *s, u8 v) {
    Save *sv = ready(s);
    if ((sv->cnt & 0xA000) != 0xA000) return;      /* ROM mode or slot off: no SPI transfer */
    int first = !sv->selected;
    sv->selected = 1;
    sv->data = bus_byte(sv, v, first);
    if (!(sv->cnt & 0x40)) release(sv);
}

/* ---- hooks -------------------------------------------------------------------------------- */
void save_init(Nds *s) { (void)s; }

/* The NDS slot belongs to one CPU at a time (EXMEMCNT bit 11); the other reads 0 and its
 * writes do nothing. */
static int owner(Nds *s, int cpu) { return (int)((s->exmemcnt >> 11) & 1) == cpu; }

int save_io_read(Nds *s, int cpu, u32 off, int size, u32 *v) {
    if (off < 0x1A0 || off > 0x1A3) return 0;
    Save *sv = &s->save;
    u32 r = owner(s, cpu) ? sv->cnt | (u32)sv->data << 16 : 0;     /* the four bytes from 0x1A0 */
    *v = size == 4 ? r : (r >> ((off & (size == 2 ? 2 : 3)) * 8)) & (size == 2 ? 0xFFFF : 0xFF);
    return 1;
}

int save_io_write(Nds *s, int cpu, u32 off, int size, u32 v) {
    if (off < 0x1A0 || off > 0x1A3) return 0;
    if (!owner(s, cpu)) return 1;
    Save *sv = &s->save;
    if (size == 4) { cnt_write(s, (u16)v); data_write(s, (u8)(v >> 16)); }
    else if (off == 0x1A0) cnt_write(s, size == 2 ? (u16)v : (u16)((sv->cnt & 0xFF00) | (v & 0xFF)));
    else if (off == 0x1A1) cnt_write(s, (u16)((sv->cnt & 0x00FF) | (v & 0xFF) << 8));
    else if (off == 0x1A2) data_write(s, (u8)v);
    return 1;                       /* 0x1A3: AUXSPIDATA's unused upper byte */
}

int save_option(Nds *s, const char *name, const char *value) {
    Save *sv = &s->save;
    if (!strcmp(name, "--save-type")) {
        int t = 0;
        while (t < SAVE_TYPES && strcmp(value, chips[t].name)) t++;
        if (t == SAVE_TYPES) {      /* refused, so the driver stops instead of guessing */
            fprintf(stderr, "save: no save type %s; one of", value);
            for (int k = 0; k < SAVE_TYPES; k++) fprintf(stderr, " %s", chips[k].name);
            fprintf(stderr, "\n");
            return 0;
        }
        unready(s);
        sv->type = t; sv->type_set = 1;
        return 1;
    }
    if (!strcmp(name, "--save")) {
        unready(s);
        size_t len = strlen(value);
        free(sv->path);
        sv->path = malloc(len + 1);
        memcpy(sv->path, value, len + 1);
        return 1;
    }
    return 0;
}

void save_shutdown(Nds *s) { save_flush(s); }
