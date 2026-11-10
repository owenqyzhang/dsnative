/* Cartridge reads (runtime/nds.c): a B7 block read through the data port, and through DMA in
 * card timing, which moves a word for each word the card has ready. The SDK's CARD library
 * sets up a channel of one word with repeat for some reads (Pokemon Black), and one of a whole
 * block for others (Black 2); both must move the block, and a channel of the CPU that doesn't
 * own the slot must not. Layout per GBATEK "DS Cartridge I/O Ports": ROMCTRL 040001A4h bit 31
 * start, bit 23 word ready, bits 24-26 block size (1 = 200h bytes, 7 = 4 bytes); the command
 * bytes at 040001A8h; the data port 04100010h; AUXSPICNT bit 14 enables the transfer-done IRQ. */
#include <stdio.h>
#include <stdlib.h>
#include "nds.h"

static Nds *s;
static int failures;
static u8 rom[0x10000];

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

enum { ROMCTRL = 0x040001A4, CMD = 0x040001A8, PORT = 0x04100010, DST = 0x02000000 };
#define BLOCK_200 (1u << 24)

static u32 want(u32 addr, u32 i) { u32 v; memcpy(&v, rom + addr + 4 * i, 4); return v; }

static void reset(void) {
    for (int k = 0; k < 2; k++)
        for (int ch = 0; ch < 4; ch++) wr32(k ? &s->arm7 : &s->arm9, 0x040000B8 + 12 * ch, 0);
    memset(s->ram, 0, 0x2000);
    s->ifl[0] = s->ifl[1] = 0;
    s->exmemcnt = 0;                    /* the ARM9 owns the slot */
    s->card_left = 0;                   /* a failed test must not leave the next one mid-transfer */
    s->romctrl = 0;
    wr16(&s->arm9, 0x040001A0, 0xC000); /* enabled, IRQ on completion */
}

static void command_b7(Cpu *c, u32 addr, u32 block) {
    wr32(c, CMD, 0xB7u | (addr >> 24) << 8 | ((addr >> 16) & 0xFF) << 16 | ((addr >> 8) & 0xFF) << 24);
    wr32(c, CMD + 4, addr & 0xFF);
    wr32(c, ROMCTRL, 0x80000000u | block);
}

/* DMA channel `ch` of the ARM9: the data port to RAM, 32-bit, card timing; repeat as given */
static void arm9_dma(int ch, u32 count, int repeat) {
    Cpu *c = &s->arm9;
    wr32(c, 0x040000B0 + 12 * ch, PORT);
    wr32(c, 0x040000B4 + 12 * ch, DST);
    wr32(c, 0x040000B8 + 12 * ch, 0x80000000u | 5u << 27 | 1u << 26 | (repeat ? 1u << 25 : 0) | 2u << 23 | count);
}

static int block_is_at(u32 dst, u32 addr, u32 words) {
    for (u32 i = 0; i < words; i++) if (rd32(&s->arm9, dst + 4 * i) != want(addr, i)) return 0;
    return 1;
}

static void test_port(void) {
    reset();
    command_b7(&s->arm9, 0x8000, BLOCK_200);
    CHECK(rd32(&s->arm9, ROMCTRL) & 1u << 23, "no word ready after the command: %08x", rd32(&s->arm9, ROMCTRL));
    for (u32 i = 0; i < 128; i++) {
        u32 v = rd32(&s->arm9, PORT);
        CHECK(v == want(0x8000, i), "port word %u: %08x, want %08x", i, v, want(0x8000, i));
        if (v != want(0x8000, i)) break;
    }
    CHECK(!(rd32(&s->arm9, ROMCTRL) & (1u << 31 | 1u << 23)), "busy or ready after the block: %08x", rd32(&s->arm9, ROMCTRL));
    CHECK(s->ifl[0] & IRQ_CARD_DONE, "no transfer-done interrupt after the block");
}

static void test_dma_word_repeat_before(void) {
    reset();
    arm9_dma(1, 1, 1);                  /* enabled first, as the SDK does: nothing to move yet */
    CHECK(rd32(&s->arm9, DST) == 0, "the DMA moved a word with no card transfer");
    command_b7(&s->arm9, 0x8200, BLOCK_200);
    CHECK(block_is_at(DST, 0x8200, 128), "one-word repeating DMA enabled before the command moved the block wrongly");
    CHECK(rd32(&s->arm9, DST + 0x200) == 0, "the DMA moved more than the block");
    CHECK(!(rd32(&s->arm9, ROMCTRL) & (1u << 31 | 1u << 23)), "busy or ready after the block: %08x", rd32(&s->arm9, ROMCTRL));
    CHECK(s->ifl[0] & IRQ_CARD_DONE, "no transfer-done interrupt");
    CHECK(rd32(&s->arm9, 0x040000C4) & 0x80000000u, "a repeating channel is disabled after a block");
}

static void test_dma_word_repeat_after(void) {
    reset();
    command_b7(&s->arm9, 0x8400, BLOCK_200);
    arm9_dma(2, 1, 1);                  /* enabled once the transfer is running */
    CHECK(block_is_at(DST, 0x8400, 128), "one-word repeating DMA enabled after the command did not move the block");
    CHECK(s->ifl[0] & IRQ_CARD_DONE, "no transfer-done interrupt");
}

static void test_dma_block(void) {
    reset();
    arm9_dma(0, 128, 1);                /* Black 2: the whole block in one request */
    command_b7(&s->arm9, 0x8600, BLOCK_200);
    CHECK(block_is_at(DST, 0x8600, 128), "block-sized DMA did not move the block");
    CHECK(s->ifl[0] & IRQ_CARD_DONE, "no transfer-done interrupt");
}

static void test_dma_without_repeat(void) {
    reset();
    arm9_dma(1, 4, 0);                  /* four words and done: the CPU reads the rest */
    command_b7(&s->arm9, 0x8800, BLOCK_200);
    CHECK(block_is_at(DST, 0x8800, 4), "the first four words are wrong");
    CHECK(!(rd32(&s->arm9, 0x040000C4) & 0x80000000u), "the channel is still enabled after its count");
    for (u32 i = 4; i < 128; i++) {
        u32 v = rd32(&s->arm9, PORT);
        CHECK(v == want(0x8800, i), "port word %u after the DMA: %08x, want %08x", i, v, want(0x8800, i));
        if (v != want(0x8800, i)) break;
    }
    CHECK(s->ifl[0] & IRQ_CARD_DONE, "no transfer-done interrupt");
}

static void test_other_cpu(void) {
    reset();
    s->exmemcnt = 1u << 11;             /* the ARM7 owns the slot: the ARM9's channel gets no requests */
    arm9_dma(1, 1, 1);
    command_b7(&s->arm7, 0x8A00, BLOCK_200);
    CHECK(rd32(&s->arm9, DST) == 0, "the ARM9's DMA took card words of a slot the ARM7 owns");
    CHECK(rd32(&s->arm9, 0x040000C4) & 0x80000000u, "the idle channel was disabled");
    /* and the ARM7's own channel (timing 2 is the cartridge) does */
    Cpu *c7 = &s->arm7;
    wr32(c7, 0x040000BC, PORT); wr32(c7, 0x040000C0, DST + 0x1000);
    wr32(c7, 0x040000C4, 0x80000000u | 2u << 28 | 1u << 26 | 1u << 25 | 2u << 23 | 1);
    CHECK(block_is_at(DST + 0x1000, 0x8A00, 128), "the ARM7's one-word DMA did not move the block");
    CHECK(s->ifl[1] & IRQ_CARD_DONE, "no transfer-done interrupt on the ARM7");
}

int main(void) {
    for (u32 i = 0; i < sizeof rom / 4; i++) { u32 v = (i + 1) * 2654435761u; memcpy(rom + 4 * i, &v, 4); }
    s = nds_create(rom, sizeof rom);
    test_port();
    test_dma_word_repeat_before();
    test_dma_word_repeat_after();
    test_dma_block();
    test_dma_without_repeat();
    test_other_cpu();
    if (!failures) printf("card: all checks passed\n");
    return failures != 0;
}
