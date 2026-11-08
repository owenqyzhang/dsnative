/* The Wi-Fi stub (runtime/wifi.c), driven through the ARM7's bus the way the SDK's wireless code
 * drives it: ports with their reset values and writable bits, the interrupt flags and their one
 * edge into the ARM7's IF, the power states, the baseband and RF chips behind their serial ports,
 * the RAM and the buffer ports, the microsecond counter and the random generator. Expected values
 * come from GBATEK's "DS Wireless Communications", and the random sequence from its recurrence,
 * iterated here. */
#include <stdio.h>
#include <stdlib.h>
#include "nds.h"

static Nds *s;
static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define PORT(o) (0x04808000u + (o))
#define RAM(o) (0x04804000u + (o))
static u32 rd(u32 a) { return rd16(&s->arm7, a); }
static void wr(u32 a, u32 v) { wr16(&s->arm7, a, v); }
static void expect(u32 off, u32 want, const char *name) {
    u32 got = rd(PORT(off));
    CHECK(got == want, "%s (%03x): %04x, expected %04x", name, off, got, want);
}

static void test_reset_values(void) {
    expect(0x000, 0x1440, "W_ID");
    CHECK(rd(0x04800000) == 0x1440, "W_ID through the WS0 mirror: %04x", rd(0x04800000));
    CHECK(rd32(&s->arm7, PORT(0)) == 0x1440, "32-bit read of W_ID: %08x", rd32(&s->arm7, PORT(0)));
    expect(0x004, 0x0000, "W_MODE_RST"); expect(0x02C, 0x0707, "W_TX_RETRYLIMIT"); expect(0x036, 0x0001, "W_POWER_US");
    expect(0x038, 0x0003, "W_POWER_TX"); expect(0x03C, 0x0200, "W_POWERSTATE"); expect(0x050, 0x4000, "W_RXBUF_BEGIN");
    expect(0x052, 0x4800, "W_RXBUF_END"); expect(0x08C, 0x0064, "W_BEACONINT"); expect(0x0B0, 0x0010, "W_TXREQ_READ");
    expect(0x0D0, 0x0401, "W_RXFILTER"); expect(0x0DA, 0x0602, "W_RX_LEN_CROP"); expect(0x0EC, 0x3F03, "W_CONFIG_0ECh");
    expect(0x0F0, 0xFC00, "W_US_COMPARE0"); expect(0x0F2, 0xFFFF, "W_US_COMPARE1"); expect(0x120, 0x0048, "W_CONFIG_120h");
    expect(0x122, 0x4840, "W_CONFIG_122h"); expect(0x142, 0x2443, "W_CONFIG_142h"); expect(0x14C, 0x162C, "W_CONFIG_14Ch");
    expect(0x150, 0x0204, "W_CONFIG_150h"); expect(0x160, 0x0100, "W_BB_MODE"); expect(0x168, 0x800D, "W_BB_POWER");
    expect(0x184, 0x0018, "W_RF_CNT"); expect(0x19C, 0x0004, "W_RF_PINS"); expect(0x214, 0x0009, "W_RF_STATUS");
    expect(0x254, 0xFFFF, "W_CONFIG_254h (FFFFh on a DS)");
}

static void test_access(void) {
    /* only the writable bits take: W_MODE_WEP bits 0-6, W_AID_FULL 0-10, W_AID_LOW 0-3 */
    wr(PORT(0x006), 0xFFFF); expect(0x006, 0x007F, "W_MODE_WEP");
    wr(PORT(0x02A), 0xFFFF); expect(0x02A, 0x07FF, "W_AID_FULL");
    wr(PORT(0x028), 0xFFFF); expect(0x028, 0x000F, "W_AID_LOW");
    /* read-only ports ignore writes */
    wr(PORT(0x000), 0x1234); expect(0x000, 0x1440, "W_ID after a write");
    /* a 32-bit write goes to two ports; a byte write is ignored */
    wr32(&s->arm7, PORT(0x018), 0x22221111u);
    expect(0x018, 0x1111, "W_MACADDR_0"); expect(0x01A, 0x2222, "W_MACADDR_1");
    wr8(&s->arm7, PORT(0x018), 0x77);
    expect(0x018, 0x1111, "W_MACADDR_0 after STRB");
    CHECK(rd8(&s->arm7, PORT(0x019)) == 0x11, "LDRB of the high byte: %02x", rd8(&s->arm7, PORT(0x019)));
    /* unused ports read 0 and stay 0 */
    wr(PORT(0x300), 0xFFFF); expect(0x300, 0, "an unused port");
    wr(PORT(0x1000), 0xFFFF); expect(0x1000, 0, "beyond the ports");
    /* the ARM9 doesn't see the chip */
    CHECK(rd16(&s->arm9, PORT(0x000)) == 0, "the ARM9 read W_ID: %04x", rd16(&s->arm9, PORT(0x000)));
}

static void test_interrupts(void) {
    s->ifl[1] = 0;
    wr(PORT(0x012), 0x0008);            /* W_IE: transmit error increment */
    wr(PORT(0x21C), 0x0408);            /* W_IF_SET: IRQ3 and IRQ10, which doesn't exist */
    expect(0x010, 0x0008, "W_IF after W_IF_SET");
    CHECK(s->ifl[1] & (1u << 24), "no ARM7 interrupt (IF bit 24) when W_IF & W_IE rose");
    s->ifl[1] = 0;
    wr(PORT(0x21C), 0x0008);
    CHECK(!(s->ifl[1] & (1u << 24)), "IF bit 24 came again while W_IF & W_IE was already set");
    wr(PORT(0x010), 0x0008);            /* write 1 to clear */
    expect(0x010, 0x0000, "W_IF after the acknowledge");
    wr(PORT(0x21C), 0x0008);
    CHECK(s->ifl[1] & (1u << 24), "IF bit 24 did not come after W_IF & W_IE went through zero");
    wr(PORT(0x010), 0xFFFF);
    wr(PORT(0x012), 0xFFFF);
    s->ifl[1] = 0;
    wr(PORT(0x21C), 0xFFFF);
    expect(0x010, 0xFBFF, "W_IF after setting every flag");
    wr(PORT(0x010), 0xFFFF); wr(PORT(0x012), 0);
}

static void test_reset_bits(void) {
    wr(PORT(0x018), 0x1234); wr(PORT(0x02C), 0x0000); wr(PORT(0x056), 0x0123);
    wr(PORT(0x004), 0x4000);            /* bit 14: reset the MAC address, retry limit, RX buffer limits... */
    expect(0x018, 0, "W_MACADDR_0 after a bit 14 reset"); expect(0x02C, 0x0707, "W_TX_RETRYLIMIT after it");
    expect(0x056, 0x0123, "W_RXBUF_WR_ADDR, which only bit 13 resets");
    wr(PORT(0x004), 0x2000);
    expect(0x056, 0, "W_RXBUF_WR_ADDR after a bit 13 reset");
    expect(0x004, 0, "W_MODE_RST: the reset bits don't stay");
    /* 0 then 1 on bit 0 sets W_RF_STATUS and friends */
    wr(PORT(0x214), 0x1234); wr(PORT(0x19C), 0x1234);
    wr(PORT(0x004), 0x0001);
    expect(0x004, 0x0001, "W_MODE_RST"); expect(0x19C, 0x0046, "W_RF_PINS after 0-then-1"); expect(0x214, 0x0009, "W_RF_STATUS after it");
    wr(PORT(0x004), 0x0000);
    expect(0x27C, 0x000A, "port 27Ch after 1-then-0");
}

static void test_rx_latch(void) {
    wr(PORT(0x056), 0x0C00 / 2);
    wr(PORT(0x030), 0x8000);
    expect(0x054, 0, "W_RXBUF_WRCSR before the latch");
    wr(PORT(0x030), 0x8001);
    expect(0x054, 0x0600, "W_RXBUF_WRCSR after W_RXCNT bit 0");
    CHECK(rd(PORT(0x030)) == 0x8000, "W_RXCNT: %04x, the latch bit stays", rd(PORT(0x030)));
    wr(PORT(0x094), 0x5555); wr(PORT(0x030), 0x8080);
    expect(0x098, 0x5555, "W_TXBUF_REPLY2 after W_RXCNT bit 7"); expect(0x094, 0, "W_TXBUF_REPLY1 after it");
    wr(PORT(0x004), 0x6000);
}

static void test_power(void) {
    /* a queued power-up needs W_POWER_US clear and hardware mode 1 */
    wr(PORT(0x03C), 0x0002);
    expect(0x03C, 0x0202, "W_POWERSTATE: the request stays queued while the chip is powered down");
    wr(PORT(0x036), 0x0000);
    wr(PORT(0x004), 0x0001);
    s->ifl[1] = 0;
    wr(PORT(0x012), 1 << 11);
    wr(PORT(0x03C), 0x0002);
    expect(0x03C, 0x0000, "W_POWERSTATE after a power-up request: enabled");
    expect(0x010, 1 << 11, "W_IF: the RF wakeup (IRQ11)");
    CHECK(s->ifl[1] & (1u << 24), "the wakeup did not reach the ARM7");
    wr(PORT(0x010), 0xFFFF); wr(PORT(0x012), 0);
    /* W_POWERFORCE 8001h powers down at once */
    wr(PORT(0x19C), 0);
    wr(PORT(0x040), 0x8001);
    expect(0x03C, 0x0200, "W_POWERSTATE after W_POWERFORCE 8001h");
    expect(0x19C, 0x0046, "W_RF_PINS after it");
    expect(0x040, 0x8001, "W_POWERFORCE");
}

/* the baseband chip: ports off until W_POWER_US and W_BB_POWER say otherwise */
static u32 bb_read(u32 idx) { wr(PORT(0x158), 0x6000 | idx); return rd(PORT(0x15C)); }
static void bb_write(u32 idx, u32 v) { wr(PORT(0x15A), v); wr(PORT(0x158), 0x5000 | idx); }

static void test_baseband(void) {
    wr(PORT(0x036), 0x0001); wr(PORT(0x168), 0x800D);
    bb_write(0x13, 0x77);
    wr(PORT(0x036), 0); wr(PORT(0x168), 0);
    CHECK(bb_read(0x13) == 0x00, "BB[13h] took a write while the ports were disabled: %02x", bb_read(0x13));
    CHECK(rd(PORT(0x15E)) == 0, "W_BB_BUSY: %04x, not ready", rd(PORT(0x15E)));
    CHECK(bb_read(0x00) == 0x6D, "BB[00h] (chip ID): %02x", bb_read(0x00));
    CHECK(bb_read(0x5D) == 0x01, "BB[5Dh]: %02x", bb_read(0x5D));
    CHECK(bb_read(0x69) == 0 && bb_read(0xFF) == 0, "BB[69h..FFh] don't read 0");
    /* the SDK's self-test: patterns over every R/W register, read back */
    static const u8 rw[] = {1,2,3,4,5,6,7,8,9,0xA,0xB,0xC,0x13,0x14,0x15,0x1B,0x1C,0x1D,0x1E,0x1F,0x20,0x21,0x22,0x23,0x24,0x25,0x26,
                            0x28,0x29,0x2A,0x2B,0x2C,0x2D,0x2E,0x2F,0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x38,0x39,0x3A,0x3B,0x3C,
                            0x3D,0x3E,0x3F,0x40,0x41,0x42,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4A,0x4B,0x4C,0x4E,0x4F,0x50,0x51,0x52,
                            0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5A,0x5B,0x5C,0x62,0x63,0x65,0x67,0x68};
    int bad = 0;
    for (int pat = 0; pat < 3; pat++)
        for (size_t i = 0; i < sizeof rw; i++) {
            u32 v = pat == 0 ? 0xFF : pat == 1 ? 0x55 : (u32)(0xFF - i);
            bb_write(rw[i], v);
            bad += bb_read(rw[i]) != v;
        }
    CHECK(!bad, "%d baseband registers did not read back what was written", bad);
    /* registers that aren't writable ignore it */
    bb_write(0x0D, 0xEE); bb_write(0x00, 0x03); bb_write(0x69, 0x11);
    CHECK(bb_read(0x0D) == 0 && bb_read(0x00) == 0x6D && bb_read(0x69) == 0, "a read-only baseband register changed");
}

/* the RF chip: 24-bit transfers, index in bits 2-6 of DATA2, bit 7 reads */
static u32 rf_read(u32 idx) {
    wr(PORT(0x17C), 0x80 | idx << 2);
    return (rd(PORT(0x17C)) & 3) << 16 | rd(PORT(0x17E));
}
static void rf_write(u32 idx, u32 v) { wr(PORT(0x17E), v & 0xFFFF); wr(PORT(0x17C), idx << 2 | (v >> 16 & 3)); }

static void test_rf(void) {
    CHECK(rd(PORT(0x180)) == 0, "W_RF_BUSY: %04x", rd(PORT(0x180)));
    CHECK(rf_read(0x00) == 0x00007 && rf_read(0x05) == 0x01780 && rf_read(0x09) == 0x00120 && rf_read(0x1B) == 0x0000F,
          "RF power-on values: %05x %05x %05x %05x", rf_read(0), rf_read(5), rf_read(9), rf_read(0x1B));
    /* the firmware's RF table (GBATEK's example entries, which fw[0CEh] holds as index << 18 | data) */
    const u8 *f = s->fw.data;
    for (int i = 0; i < 12; i++) {
        u32 e = f[0xCE + 3 * i] | f[0xCF + 3 * i] << 8 | f[0xD0 + 3 * i] << 16;
        rf_write(e >> 18, e & 0x3FFFF);
        CHECK(rf_read(e >> 18) == (e & 0x3FFFF), "RF[%u] read back %05x, wrote %05x", e >> 18, rf_read(e >> 18), e & 0x3FFFF);
    }
    rf_write(0x1F, 0x2AAAA);            /* any write to the reset register resets the chip */
    CHECK(rf_read(0x09) == 0x00120 && rf_read(0x00) == 0x00007, "RF registers after the reset: %05x %05x", rf_read(9), rf_read(0));
}

static u32 lfsr_step(u32 x) { return (x & 1) ^ (((x << 1) | (x >> 10)) & 0x7FF); }

static void test_random(void) {
    /* x = (x & 1) xor (x rol 1) in 11 bits; 5FDh values before it repeats. A read returns the value
     * latched by the one before it (0 for the first) and latches the current one. The phase of the
     * sequence against the bus clock is arbitrary: the model starts it at 7FFh at time 0. */
    u32 x = 0x7FF, n = 0;
    do { x = lfsr_step(x); n++; } while (x != 0x7FF && n < 4000);
    CHECK(n == 0x5FD, "the generator's period is %u, GBATEK says 5FDh", n);
    u32 latched = 0;
    for (u32 k = 1; k <= 20; k++) {
        u64 t = 100000 + k * 37;
        s->time = t;
        u32 got = rd(PORT(0x044));
        CHECK(got == latched, "read %u of W_RANDOM at %llu: %03x, expected %03x", k, (unsigned long long)t, got, latched);
        CHECK(got < 0x800, "W_RANDOM %04x has bits above 10", got);
        latched = 0x7FF;
        for (u64 i = 0; i < t % 0x5FD; i++) latched = lfsr_step(latched);
    }
}

static void test_ram(void) {
    /* the SDK's RAM test: a descending pattern, then A5A5h / 5A5Ah alternating */
    for (u32 i = 0; i < 0x1000; i++) wr(RAM(i * 2), (0xFFFF - i) & 0xFFFF);
    int bad = 0;
    for (u32 i = 0; i < 0x1000; i++) bad += rd(RAM(i * 2)) != ((0xFFFF - i) & 0xFFFF);
    CHECK(!bad, "%d halfwords of the Wi-Fi RAM did not read back", bad);
    for (u32 i = 0; i < 0x1000; i++) wr(RAM(i * 2), i & 1 ? 0x5A5A : 0xA5A5);
    CHECK(rd(RAM(0)) == 0xA5A5 && rd(RAM(0x1FFE)) == 0x5A5A && rd(RAM(0x1000)) == 0xA5A5, "the pattern: %04x %04x", rd(RAM(0)), rd(RAM(0x1FFE)));
    /* 32-bit and byte access, a byte write ignored, and the region at 04800000h repeats it */
    wr32(&s->arm7, RAM(0x100), 0xDEADBEEFu);
    CHECK(rd32(&s->arm7, RAM(0x100)) == 0xDEADBEEFu && rd(RAM(0x102)) == 0xDEAD, "32-bit RAM access: %08x", rd32(&s->arm7, RAM(0x100)));
    wr8(&s->arm7, RAM(0x100), 0x11);
    CHECK(rd8(&s->arm7, RAM(0x100)) == 0xEF, "STRB to the RAM took: %02x", rd8(&s->arm7, RAM(0x100)));
    CHECK(rd32(&s->arm7, 0x04800000u + 0x4100) == 0xDEADBEEFu, "the WS0 region doesn't repeat the RAM");
    CHECK(rd(0x04804000u + 0x2000) == 0, "RAM above 5FFFh reads %04x", rd(0x04806000u));
}

static void test_buffers(void) {
    for (u32 i = 0; i < 0x40; i++) wr(RAM(0x100 + 2 * i), 0x1000 + i);
    /* W_TXBUF_WR_DATA writes at W_TXBUF_WR_ADDR and steps it; at W_TXBUF_GAP it jumps by GAPDISP * 2 */
    wr(PORT(0x068), 0x0100); wr(PORT(0x074), 0x0106); wr(PORT(0x076), 0x0010); wr(PORT(0x06C), 0);
    for (u32 i = 0; i < 4; i++) wr(PORT(0x070), 0xA000 + i);
    CHECK(rd(RAM(0x100)) == 0xA000 && rd(RAM(0x102)) == 0xA001 && rd(RAM(0x104)) == 0xA002, "TX data written to %04x %04x %04x", rd(RAM(0x100)), rd(RAM(0x102)), rd(RAM(0x104)));
    CHECK(rd(RAM(0x106)) == 0x1003, "the halfword at the gap was written (%04x): the address should jump over it", rd(RAM(0x106)));
    CHECK(rd(RAM(0x126)) == 0xA003, "the fourth halfword went to %04x, not 0126h", rd(RAM(0x126)));
    expect(0x068, 0x0128, "W_TXBUF_WR_ADDR after the gap");
    wr(PORT(0x070), 0xB000);
    CHECK(rd(RAM(0x128)) == 0xB000, "TX data after the gap: %04x", rd(RAM(0x128)));
    /* W_RXBUF_RD_DATA reads at W_RXBUF_RD_ADDR and steps it, wrapping from END to BEGIN */
    s->ifl[1] = 0;
    wr(PORT(0x012), 1 << 9);
    wr(PORT(0x050), 0x4100); wr(PORT(0x052), 0x4106); wr(PORT(0x062), 0); wr(PORT(0x064), 0);
    wr(PORT(0x058), 0x0102); wr(PORT(0x05C), 2);
    u32 a = rd(PORT(0x060)), b = rd(PORT(0x060)), c = rd(PORT(0x060));
    CHECK(a == 0xA001 && b == 0xA002 && c == 0xA000, "RX data %04x %04x %04x, expected A001 A002 then wrapped to A000", a, b, c);
    expect(0x058, 0x0102, "W_RXBUF_RD_ADDR after three reads (0106h wraps to BEGIN)");
    expect(0x05C, 0, "W_RXBUF_COUNT: runs out and stays at zero");
    expect(0x010, 1 << 9, "W_IF: IRQ9, the count ran out");
    CHECK(s->ifl[1] & (1u << 24), "IRQ9 did not reach the ARM7");
    wr(PORT(0x010), 0xFFFF); wr(PORT(0x012), 0);
}

static u64 us_count(void) {
    return rd(PORT(0x0F8)) | (u64)rd(PORT(0x0FA)) << 16 | (u64)rd(PORT(0x0FC)) << 32 | (u64)rd(PORT(0x0FE)) << 48;
}

static void test_counter(void) {
    s->time = 5000000;
    wr(PORT(0x0E8), 1);
    s->time += RTC_HZ;                  /* one second of bus cycles */
    u64 us = us_count();
    CHECK(us >= 999999 && us <= 1000001, "W_US_COUNT after one second: %llu", (unsigned long long)us);
    wr(PORT(0x0E8), 0);                 /* stopped: it keeps its value */
    u64 stopped = us_count();
    s->time += RTC_HZ;
    CHECK(us_count() == stopped && stopped >= us, "W_US_COUNT moved while disabled: %llu then %llu", (unsigned long long)stopped, (unsigned long long)us_count());
    wr(PORT(0x0F8), 0x1234); wr(PORT(0x0FA), 0); wr(PORT(0x0FC), 0); wr(PORT(0x0FE), 0);
    CHECK(us_count() == 0x1234, "W_US_COUNT after writing it: %llx", (unsigned long long)us_count());
}

static void test_firmware_values(void) {
    /* the 16 ports the SDK initializes from firmware[044h..063h] take every value as it is */
    static const u16 offs[16] = {0x146, 0x148, 0x14A, 0x14C, 0x120, 0x122, 0x154, 0x144, 0x130, 0x132, 0x140, 0x142, 0x038, 0x124, 0x128, 0x150};
    const u8 *f = s->fw.data;
    for (int i = 0; i < 16; i++) {
        u32 v = f[0x44 + 2 * i] | f[0x45 + 2 * i] << 8;
        wr(PORT(offs[i]), v);
        CHECK(rd(PORT(offs[i])) == v, "port %03x holds %04x after the firmware value %04x", offs[i], rd(PORT(offs[i])), v);
    }
    /* and the MAC address from firmware[036h] goes through the three halfword ports */
    for (int i = 0; i < 3; i++) wr(PORT(0x018 + 2 * i), f[0x36 + 2 * i] | f[0x37 + 2 * i] << 8);
    CHECK(rd(PORT(0x018)) == 0x4402 && rd(PORT(0x01A)) == 0x4E53 && rd(PORT(0x01C)) == 0x5441, "MAC address ports %04x %04x %04x", rd(PORT(0x018)), rd(PORT(0x01A)), rd(PORT(0x01C)));
}

int main(void) {
    static u8 rom[0x1000];
    s = nds_create(rom, sizeof rom);
    test_reset_values();
    test_access();
    test_interrupts();
    test_reset_bits();
    test_rx_latch();
    test_power();
    test_baseband();
    test_rf();
    test_random();
    test_ram();
    test_buffers();
    test_counter();
    test_firmware_values();
    nds_shutdown(s);
    if (failures) { printf("%d checks failed\n", failures); return 1; }
    printf("wifi ok\n");
    return 0;
}
