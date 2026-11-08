/* The DSi-only registers as a DS-mode game sees them (runtime/dsi.c): the SCFG and MBK range and
 * every other new block read 0 on both CPUs and take no writes; the ARM7's SNDEXCNT and GPIO
 * registers work. The expected values follow GBATEK's "DSi Control Registers (SCFG)": SCFG_EXT is
 * 03000000h (ARM9) and 12A03000h (ARM7) for a DS cartridge, so bit 31 is clear and the SCFG range
 * is locked, and the new blocks' access bits are clear. */
#include <stdio.h>
#include <stdlib.h>
#include "nds.h"

static Nds *s;
static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void test_locked(Cpu *c, const char *who) {
    /* the registers the SDK probes first, each way it reads them */
    CHECK(rd8(c, 0x04004000) == 0, "%s: SCFG_A9ROM (byte) %02x", who, rd8(c, 0x04004000));
    CHECK(rd8(c, 0x04004001) == 0, "%s: SCFG_A7ROM (byte) %02x", who, rd8(c, 0x04004001));
    CHECK(rd16(c, 0x04004004) == 0, "%s: SCFG_CLK %04x", who, rd16(c, 0x04004004));
    CHECK(rd16(c, 0x04004006) == 0, "%s: SCFG_RST / SCFG_JTAG %04x", who, rd16(c, 0x04004006));
    CHECK(rd32(c, 0x04004008) == 0, "%s: SCFG_EXT %08x", who, rd32(c, 0x04004008));
    CHECK(rd16(c, 0x04004010) == 0, "%s: SCFG_MC (0: a cartridge inserted, slot powered off) %04x", who, rd16(c, 0x04004010));
    CHECK(rd16(c, 0x04004012) == 0 && rd16(c, 0x04004014) == 0, "%s: SCFG card delays", who);
    CHECK(rd16(c, 0x04004020) == 0 && rd16(c, 0x04004024) == 0, "%s: SCFG_WL / SCFG_OP", who);
    CHECK(rd32(c, 0x04004040) == 0 && rd32(c, 0x04004060) == 0, "%s: MBK registers", who);
    /* the blocks SCFG_EXT keeps off: new DMA, camera, DSP, AES, I2C, microphone, SD/MMC, SDIO */
    static const u32 blocks[] = {0x04004100, 0x04004200, 0x04004300, 0x04004400, 0x04004500, 0x04004600, 0x04004800, 0x04004A00};
    for (size_t i = 0; i < sizeof blocks / sizeof blocks[0]; i++)
        CHECK(rd32(c, blocks[i]) == 0 && rd16(c, blocks[i]) == 0 && rd8(c, blocks[i]) == 0, "%s: %08x reads nonzero", who, blocks[i]);
    /* writes don't stick, however they are made */
    wr32(c, 0x04004008, 0x8307F100u); wr16(c, 0x04004004, 0x0001); wr8(c, 0x04004000, 1); wr32(c, 0x04004040, 0xFFFFFFFFu);
    wr32(c, 0x04004100, 0x12345678u); wr32(c, 0x04004400, 0x80000000u);
    CHECK(rd32(c, 0x04004008) == 0 && rd16(c, 0x04004004) == 0 && rd8(c, 0x04004000) == 0 && rd32(c, 0x04004040) == 0 &&
          rd32(c, 0x04004100) == 0 && rd32(c, 0x04004400) == 0, "%s: a write to a locked register stuck", who);
    /* IE2 / IF2 and the second cartridge slot's ports */
    wr32(c, 0x04000218, 0xFFFFFFFFu); wr32(c, 0x0400021C, 0xFFFFFFFFu); wr32(c, 0x040021A0, 0xFFFFFFFFu);
    CHECK(rd32(c, 0x04000218) == 0 && rd32(c, 0x0400021C) == 0 && rd32(c, 0x040021A0) == 0 && rd32(c, 0x040021A4) == 0,
          "%s: IE2/IF2 or the second slot's ports hold something", who);
}

static void test_sndexcnt(void) {
    Cpu *c = &s->arm7;
    CHECK(rd16(c, 0x04004700) == 0, "SNDEXCNT after reset: %04x", rd16(c, 0x04004700));
    wr16(c, 0x04004700, 0x3FFF);                        /* the NITRO/DSP ratio (bits 0-3), nothing in 4-12, bit 13 */
    CHECK(rd16(c, 0x04004700) == 0x200F, "SNDEXCNT after writing 3FFFh: %04x (ratio, and bit 13 while 15 was clear)", rd16(c, 0x04004700));
    wr16(c, 0x04004700, 0xC008);                        /* mute and enable; bit 13 clear: allowed (bit 15 was 0) */
    CHECK(rd16(c, 0x04004700) == 0xC008, "SNDEXCNT after writing C008h: %04x", rd16(c, 0x04004700));
    wr16(c, 0x04004700, 0xE008);                        /* bit 13 only writable while bit 15 was 0 */
    CHECK(rd16(c, 0x04004700) == 0xC008, "SNDEXCNT bit 13 changed with the enable bit set: %04x", rd16(c, 0x04004700));
    wr8(c, 0x04004701, 0x00);
    CHECK(rd16(c, 0x04004700) == 0x0008, "SNDEXCNT after clearing its high byte (bit 13 held while 15 was set): %04x", rd16(c, 0x04004700));
    CHECK(rd16(&s->arm9, 0x04004700) == 0, "the ARM9 reads SNDEXCNT: %04x", rd16(&s->arm9, 0x04004700));
}

static void test_gpio(void) {
    Cpu *c = &s->arm7;
    /* data in: pins 0-2 and 4 high, 5 (headphones) low, 6 (power button) idle, 7 (sound enable) high */
    CHECK(rd8(c, 0x04004C00) == 0xD7, "GPIO data in: %02x", rd8(c, 0x04004C00));
    wr8(c, 0x04004C01, 0x80);                           /* pin 7 an output, which the SDK sets */
    wr8(c, 0x04004C00, 0x00);
    CHECK(rd8(c, 0x04004C00) == 0x57, "GPIO data in with pin 7 driven low: %02x", rd8(c, 0x04004C00));
    wr8(c, 0x04004C00, 0x80);
    CHECK(rd8(c, 0x04004C00) == 0xD7, "GPIO data in with pin 7 driven high: %02x", rd8(c, 0x04004C00));
    wr8(c, 0x04004C02, 0xFF); wr8(c, 0x04004C03, 0xFF);
    CHECK(rd8(c, 0x04004C01) == 0x80 && rd8(c, 0x04004C02) == 0xF7 && rd8(c, 0x04004C03) == 0xF7, "GPIO direction, edge and enable: %02x %02x %02x",
          rd8(c, 0x04004C01), rd8(c, 0x04004C02), rd8(c, 0x04004C03));
    CHECK(rd16(c, 0x04004C04) == 0x0100, "GPIO_WIFI after reset: %04x (old NDS Wi-Fi mode)", rd16(c, 0x04004C04));
    wr16(c, 0x04004C04, 0x0000);
    CHECK(rd16(c, 0x04004C04) == 0x0000, "GPIO_WIFI after clearing: %04x", rd16(c, 0x04004C04));
    wr16(c, 0x04004C04, 0xFFFF);
    CHECK(rd16(c, 0x04004C04) == 0x0101, "GPIO_WIFI after writing FFFFh: %04x (bits 0 and 8 only)", rd16(c, 0x04004C04));
    CHECK(rd32(c, 0x04004C00) == (0xD7u | 0x80u << 8 | 0xF7u << 16 | 0xF7u << 24) && rd32(c, 0x04004C08) == 0, "GPIO read as a word: %08x", rd32(c, 0x04004C00));
    CHECK(rd8(&s->arm9, 0x04004C00) == 0, "the ARM9 reads the GPIO data: %02x", rd8(&s->arm9, 0x04004C00));
}

int main(void) {
    static u8 rom[0x1000];
    s = nds_create(rom, sizeof rom);
    test_locked(&s->arm9, "ARM9");
    test_locked(&s->arm7, "ARM7");
    test_sndexcnt();
    test_gpio();
    nds_shutdown(s);
    if (failures) { printf("%d checks failed\n", failures); return 1; }
    printf("dsi ok\n");
    return 0;
}
