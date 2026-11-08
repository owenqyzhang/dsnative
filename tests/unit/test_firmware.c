/* The firmware flash: what a never-configured console holds (checked as the SDK's start-up check
 * reads it: CRC-16 of each access point slot, status byte, user settings with their update
 * counters) and the SPI instruction set (read, fast read, JEDEC ID, status, write enable, page
 * write and program, page and sector erase, deep power-down), driven through SPICNT/SPIDATA on
 * the ARM7 the way the SDK's code does. */
#include <stdio.h>
#include <stdlib.h>
#include "nds.h"

static Nds *s;
static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* An independent CRC-16 (reflected polynomial A001h): table driven, where firmware.c shifts bits */
static u16 crc_table[256];
static void crc_init(void) {
    for (u32 i = 0; i < 256; i++) {
        u32 c = i;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ ((c & 1) ? 0xA001u : 0);
        crc_table[i] = (u16)c;
    }
}
static u16 crc16(u16 crc, const u8 *p, u32 n) {
    while (n--) crc = (u16)((crc >> 8) ^ crc_table[(crc ^ *p++) & 0xFF]);
    return crc;
}
static u16 le16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }

/* ---- the contents ------------------------------------------------------------------------- */
static void test_header_and_wifi(const u8 *f) {
    CHECK(f[0x1D] == 0xFF, "console type %02x, not 'DS'", f[0x1D]);
    CHECK(le16(f + 0x20) * 8 == FW_USER_SETTINGS, "user settings offset %05x", le16(f + 0x20) * 8);
    CHECK(le16(f + 0x2C) == 0x138, "Wi-Fi settings length %04x", le16(f + 0x2C));
    CHECK(le16(f + 0x2A) == crc16(0, f + 0x2C, le16(f + 0x2C)), "Wi-Fi settings CRC %04x, expected %04x",
          le16(f + 0x2A), crc16(0, f + 0x2C, le16(f + 0x2C)));
    CHECK((le16(f + 0x3C) & ~0x7FFE) == 0 && le16(f + 0x3C) != 0, "channels %04x", le16(f + 0x3C));
    CHECK(f[0x36] & 2 && !(f[0x36] & 1), "the MAC address %02x:... is not a locally administered unicast one", f[0x36]);
    CHECK(f[0x40] == 2 && f[0x41] == 0x18 && f[0x42] == 12, "RF chip type %u, %u bits, %u entries", f[0x40], f[0x41], f[0x42]);
    /* the RF table: 24-bit entries, index << 18 | data, for RF[0,4,5,6,7,8,9,0Ah,0Bh,1,2,3] */
    static const int order[12] = {0, 4, 5, 6, 7, 8, 9, 10, 11, 1, 2, 3};
    for (int i = 0; i < 12; i++) {
        u32 e = f[0xCE + 3 * i] | f[0xCF + 3 * i] << 8 | f[0xD0 + 3 * i] << 16;
        CHECK((e >> 18) == (u32)order[i], "RF entry %d has index %u, expected %d", i, e >> 18, order[i]);
    }
    /* channels 1-14: RF[5] and RF[6] words, 24 bits each (index << 18 | data) per channel. Channel 1 is
     * GBATEK's example (141728h, 1AE8BAh); the others tune a 22 MHz reference to the channel's centre
     * frequency: N + 17 + fraction / 2^24 times 22 MHz, 2412 + 5 * (ch - 1) MHz, and 2484 for 14 */
    for (int ch = 1; ch <= 14; ch++) {
        const u8 *e = f + 0xF2 + 6 * (ch - 1);
        u32 w5 = e[0] | e[1] << 8 | e[2] << 16, w6 = e[3] | e[4] << 8 | e[5] << 16;
        CHECK((w5 >> 18) == 5 && (w6 >> 18) == 6, "channel %d: RF indices %u, %u", ch, w5 >> 18, w6 >> 18);
        double n = (double)((w5 >> 6) & 0xFFF), frac = (double)((w5 & 0x3F) << 18 | (w6 & 0x3FFFF));
        double mhz = 22.0 * (n + 17 + frac / 16777216.0), want = ch == 14 ? 2484 : 2412 + 5 * (ch - 1);
        CHECK(mhz > want - 0.001 && mhz < want + 0.001, "channel %d tunes to %.4f MHz, expected %.0f", ch, mhz, want);
        if (ch == 1) CHECK(w5 == 0x141728 && w6 == 0x1AE8BA, "channel 1 words %06x %06x, GBATEK's example is 141728 1AE8BA", w5, w6);
    }
    CHECK(f[0x1FD] == 0xFF && f[0x1FE] == 0xFF && f[0x1FF] == 0xFF, "DSi Wi-Fi board bytes %02x %02x %02x", f[0x1FD], f[0x1FE], f[0x1FF]);
}

/* What the Nitro SDK's check makes of the four slots: the CRC over 00h..FDh must match, and a
 * slot is sound if it is marked not configured (FFh) or a configured type (up to 2). */
static int slot_sound(const u8 *slot) {
    if (crc16(0, slot, 0xFE) != le16(slot + 0xFE)) return 0;
    return slot[0xE7] == 0xFF || slot[0xE7] <= 2;
}

static void test_access_points(const u8 *f) {
    for (int i = 0; i < 4; i++) {
        const u8 *slot = f + FW_AP_SLOTS + 0x100 * i;
        CHECK(slot_sound(slot), "access point slot %d fails the SDK's check", i);
        if (i < 3) {
            CHECK(slot[0xE7] == 0xFF, "slot %d status %02x: not marked unconfigured", i, slot[0xE7]);
            CHECK(slot[0xEF] == 0, "slot %d has connections marked configured (%02x)", i, slot[0xEF]);
        }
        int used = 0;
        for (int k = 0; k < 0xFE; k++) if (k != 0xE7 && slot[k]) used++;
        CHECK(!used, "slot %d holds %d bytes of network data (SSID, keys, addresses, user ID)", i, used);
    }
    /* the slots sit where the header says: user settings minus 400h, 300h, 200h and 100h */
    CHECK(FW_AP_SLOTS == le16(f + 0x20) * 8 - 0x400, "slots are not at the header's offset minus 400h");
}

static void test_user_settings(const u8 *f) {
    const u8 *a = f + FW_USER_SETTINGS, *b = a + 0x100;
    for (int i = 0; i < 2; i++) {
        const u8 *u = a + 0x100 * i;
        CHECK(crc16(0xFFFF, u, 0x70) == le16(u + 0x72), "user settings area %d: CRC %04x, expected %04x", i, le16(u + 0x72), crc16(0xFFFF, u, 0x70));
        CHECK(le16(u) == 5, "user settings area %d version %u", i, le16(u));
        CHECK((le16(u + 0x64) & 7) == 1, "user settings area %d language %u", i, le16(u + 0x64) & 7);
        /* bits 10, 11, 13, 14, 15 set and 9 clear: the settings have been entered and not lost */
        CHECK((le16(u + 0x64) & 0xEE00) == 0xEC00, "user settings area %d flags %04x", i, le16(u + 0x64));
    }
    CHECK(le16(b + 0x70) == ((le16(a + 0x70) + 1) & 0x7F), "area 1 is not the newer one: counters %u, %u", le16(a + 0x70), le16(b + 0x70));
}

/* ---- the chip -------------------------------------------------------------------------------- */
static u8 spi(u8 b, int hold) {
    wr16(&s->arm7, 0x040001C0, (u32)(0x8000 | 0x0100 | (hold ? 0x0800 : 0)));      /* enabled, device 1 (firmware) */
    wr16(&s->arm7, 0x040001C2, b);
    return (u8)rd16(&s->arm7, 0x040001C2);
}

/* one instruction: out[] are the bytes sent after the opcode, in[] receives the replies to them */
static void cmd(u8 op, const u8 *out, u8 *in, int n) {
    spi(op, n > 0);
    for (int i = 0; i < n; i++) { u8 r = spi(out ? out[i] : 0, i != n - 1); if (in) in[i] = r; }
}
static void cmd_addr(u8 op, u32 addr, const u8 *out, u8 *in, int n) {
    u8 buf[300];
    buf[0] = (u8)(addr >> 16); buf[1] = (u8)(addr >> 8); buf[2] = (u8)addr;
    if (out) memcpy(buf + 3, out, (size_t)n);
    u8 rep[300];
    cmd(op, buf, rep, 3 + n);
    if (in) memcpy(in, rep + 3, (size_t)n);
}
static u8 status(void) { u8 v; cmd(0x05, 0, &v, 1); return v; }
static void wren(void) { cmd(0x06, 0, 0, 0); }
static void read(u32 addr, u8 *out, int n) { cmd_addr(0x03, addr, 0, out, n); }

static void test_reads(void) {
    u8 got[0x100], id[3];
    read(FW_AP_SLOTS, got, 0x100);
    CHECK(!memcmp(got, s->fw.data + FW_AP_SLOTS, 0x100), "READ of the first access point slot");
    read(0x3FE00, got, 0x100);
    CHECK(!memcmp(got, s->fw.data + FW_USER_SETTINGS, 0x100), "READ of the user settings");
    /* FAST READ: three address bytes, a dummy byte, then data */
    u8 buf[4] = {0x03, 0xFA, 0x00, 0x00}, two[2];
    spi(0x0B, 1);
    for (int i = 0; i < 4; i++) spi(buf[i], 1);
    two[0] = spi(0, 1); two[1] = spi(0, 0);
    CHECK(two[0] == s->fw.data[FW_AP_SLOTS] && two[1] == s->fw.data[FW_AP_SLOTS + 1], "FAST READ returned %02x %02x", two[0], two[1]);
    cmd(0x9F, 0, id, 3);
    CHECK(id[0] == 0x20 && id[1] == 0x40 && id[2] == 0x12, "JEDEC ID %02x %02x %02x, expected 20 40 12 (M45PE20)", id[0], id[1], id[2]);
    /* reading goes on across the end of the chip: the address wraps */
    u8 last[4];
    read(FW_SIZE - 2, last, 4);
    CHECK(last[0] == s->fw.data[FW_SIZE - 2] && last[2] == s->fw.data[0], "the address does not wrap at the end of the chip");
}

static void test_writes(void) {
    u8 got[0x100], data[0x100];
    for (int i = 0; i < 0x100; i++) data[i] = (u8)(0xA5 ^ i);
    CHECK(status() == 0, "status %02x before anything: WEL or WIP set", status());
    /* a page program needs the write enable latch: without it nothing changes */
    cmd_addr(0x02, 0x10000, data, 0, 4);
    read(0x10000, got, 4);
    CHECK(got[0] == 0xFF && got[1] == 0xFF, "a program without WREN changed the flash");
    wren();
    CHECK(status() == 2, "status %02x after WREN, expected WEL", status());
    cmd(0x04, 0, 0, 0);
    CHECK(status() == 0, "WRDI did not clear WEL");
    wren();
    cmd_addr(0x02, 0x10000, data, 0, 8);
    CHECK(status() == 0, "WEL did not clear when the program ended");
    read(0x10000, got, 8);
    CHECK(!memcmp(got, data, 8), "PP: bytes %02x %02x ..., expected %02x %02x", got[0], got[1], data[0], data[1]);
    /* PP only clears bits: programming 0xFF over the written bytes keeps them, other values are ANDed */
    u8 ff[4] = {0xFF, 0xFF, 0x0F, 0xF0};
    wren();
    cmd_addr(0x02, 0x10000, ff, 0, 4);
    read(0x10000, got, 4);
    CHECK(got[0] == data[0] && got[1] == data[1] && got[2] == (data[2] & 0x0F) && got[3] == (data[3] & 0xF0), "PP did not AND: %02x %02x %02x %02x", got[0], got[1], got[2], got[3]);
    /* PW writes whatever it is given (it erases the page first), the rest of the page keeps its bytes */
    u8 pw[3] = {0xFF, 0x12, 0x34};
    wren();
    cmd_addr(0x0A, 0x10000, pw, 0, 3);
    read(0x10000, got, 8);
    CHECK(got[0] == 0xFF && got[1] == 0x12 && got[2] == 0x34 && got[3] == (data[3] & 0xF0) && got[4] == data[4], "PW: %02x %02x %02x %02x %02x", got[0], got[1], got[2], got[3], got[4]);
    /* a program stays in its page: sending past the end wraps to the start of the same page */
    u8 over[4] = {0x11, 0x22, 0x33, 0x44};
    wren();
    cmd_addr(0x0A, 0x100FE, over, 0, 4);
    read(0x100FE, got, 2);
    CHECK(got[0] == 0x11 && got[1] == 0x22, "PW at the end of the page: %02x %02x", got[0], got[1]);
    read(0x10000, got, 2);
    CHECK(got[0] == 0x33 && got[1] == 0x44, "PW did not wrap within the page: %02x %02x", got[0], got[1]);
    read(0x10100, got, 2);
    CHECK(got[0] == 0xFF && got[1] == 0xFF, "PW ran into the next page");
    /* page erase: 100h bytes only */
    u8 p2[1] = {0x00};
    wren();
    cmd_addr(0x0A, 0x10100, p2, 0, 1);
    wren();
    cmd_addr(0xDB, 0x10000, 0, 0, 0);
    read(0x10000, got, 0x100);
    int clean = 1;
    for (int i = 0; i < 0x100; i++) clean &= got[i] == 0xFF;
    CHECK(clean, "PE did not erase its page");
    read(0x10100, got, 1);
    CHECK(got[0] == 0x00, "PE erased the next page too");
    CHECK(status() == 0, "WEL stayed set after PE");
    /* sector erase: 10000h bytes, only with WEL */
    cmd_addr(0xD8, 0x10000, 0, 0, 0);
    read(0x10100, got, 1);
    CHECK(got[0] == 0x00, "SE without WREN erased the sector");
    wren();
    cmd_addr(0xD8, 0x1FFFF, 0, 0, 0);
    read(0x10100, got, 1);
    CHECK(got[0] == 0xFF, "SE did not erase the sector holding 1FFFFh");
    /* the other sectors are untouched */
    read(FW_AP_SLOTS, got, 0x100);
    CHECK(slot_sound(got), "an erase of sector 1 changed the access point slots");
    /* deep power-down: everything but RDP is ignored */
    cmd(0xB9, 0, 0, 0);
    u8 id[3];
    cmd(0x9F, 0, id, 3);
    CHECK(id[0] == 0 && id[1] == 0, "the chip answered an RDID in deep power-down");
    cmd(0xAB, 0, 0, 0);
    cmd(0x9F, 0, id, 3);
    CHECK(id[0] == 0x20, "RDP did not wake the chip");
}

/* the settings can be rewritten as the SDK does when it repairs a slot: PW to a slot, then the
 * check passes on what a read returns */
static void test_repair(void) {
    u8 slot[0x100] = {0}, got[0x100];
    slot[0xE7] = 0xFF;
    slot[0xEF] = 0;
    u16 c = crc16(0, slot, 0xFE);
    slot[0xFE] = (u8)c; slot[0xFF] = (u8)(c >> 8);
    wren();
    cmd_addr(0x0A, FW_AP_SLOTS + 0x100, slot, 0, 0x100);
    read(FW_AP_SLOTS + 0x100, got, 0x100);
    CHECK(!memcmp(got, slot, 0x100) && slot_sound(got), "a slot written back does not read back");
}

int main(void) {
    static u8 rom[0x1000];
    crc_init();
    s = nds_create(rom, sizeof rom);
    const u8 *f = s->fw.data;
    test_header_and_wifi(f);
    test_access_points(f);
    test_user_settings(f);
    test_reads();
    test_writes();
    test_repair();
    nds_shutdown(s);
    if (failures) { printf("%d checks failed\n", failures); return 1; }
    printf("firmware ok\n");
    return 0;
}
