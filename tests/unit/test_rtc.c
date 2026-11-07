/* The real-time clock, driven through 0x04000138 the way the Nitro SDK's ARM7 code does it:
 * 16-bit accesses, bits sent LSB first while SCK is low and taken by the RTC as it rises, read
 * bits sampled while SCK is still low. Time advances by moving s->time, as the frame loop does. */
#define _POSIX_C_SOURCE 200809L     /* localtime_r */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "nds.h"

static Nds *s;
static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

enum { STAT1 = 0x06, INT1 = 0x16, DATETIME = 0x26, ADJUST = 0x36, STAT2 = 0x46, INT2 = 0x56, TIME = 0x66, FREE = 0x76,
       READ = 0x80 };

static void put(u16 v) { wr16(&s->arm7, 0x04000138, v); }
static u16 get(void) { return (u16)rd16(&s->arm7, 0x04000138); }

/* CS low with SCK high, then CS high: a transfer starts (directions: CS, SCK and SIO out) */
static void begin(void) { put(0x72); put(0x76); }
static void end(void) { put((u16)(get() & ~4)); }
static void send(u8 b) {
    for (int i = 0; i < 8; i++) { u16 v = (u16)(0x74 | (b >> i & 1)); put(v); put(v | 2); }
}
static u8 recv(void) {
    u8 b = 0;
    for (int i = 0; i < 8; i++) { put(0x64); b |= (u8)((get() & 1) << i); put(0x66); }
    return b;
}
/* GBATEK's flowchart instead (libnds does the same): byte accesses, read bits taken once SCK is
 * high again */
static void rd_gbatek(u8 cmd, u8 *out, int n) {
    wr8(&s->arm7, 0x04000138, 0x72); wr8(&s->arm7, 0x04000138, 0x76);
    for (int i = 0; i < 8; i++) {
        u8 v = (u8)(0x74 | (cmd >> i & 1));
        wr8(&s->arm7, 0x04000138, v); wr8(&s->arm7, 0x04000138, v | 2);
    }
    for (int j = 0; j < n; j++) {
        out[j] = 0;
        for (int i = 0; i < 8; i++) {
            wr8(&s->arm7, 0x04000138, 0x64); wr8(&s->arm7, 0x04000138, 0x66);
            out[j] |= (u8)((rd8(&s->arm7, 0x04000138) & 1) << i);
        }
    }
    wr8(&s->arm7, 0x04000138, 0x62);
}
static void rd(u8 cmd, u8 *out, int n) { begin(); send(cmd | READ); for (int i = 0; i < n; i++) out[i] = recv(); end(); }
static void wr(u8 cmd, const u8 *in, int n) { begin(); send(cmd); for (int i = 0; i < n; i++) send(in[i]); end(); }
static u8 rd1(u8 cmd) { u8 v; rd(cmd, &v, 1); return v; }
static void wr1(u8 cmd, u8 v) { wr(cmd, &v, 1); }

static void advance(double seconds) { s->time += (u64)(seconds * RTC_HZ + 0.5); }

/* reads the date and time and compares them with "YY MM DD W HH MM SS" (hex bytes as read) */
static void expect_datetime(int line, const char *want) {
    u8 b[7];
    char got[32];
    rd(DATETIME, b, 7);
    snprintf(got, sizeof got, "%02x %02x %02x %x %02x %02x %02x", b[0], b[1], b[2], b[3], b[4], b[5], b[6]);
    if (strcmp(got, want)) { failures++; printf("line %d: date and time %s, expected %s\n", line, got, want); }
}
#define EXPECT_DATETIME(want) expect_datetime(__LINE__, want)

static void set_datetime(u8 y, u8 mo, u8 d, u8 w, u8 h, u8 mi, u8 sec) {
    u8 b[7] = {y, mo, d, w, h, mi, sec};
    wr(DATETIME, b, 7);
}

/* runs scanlines (with their rtc_line) for the given time and counts ARM7 RTC interrupts */
static int irqs_over(double seconds) {
    int n = 0;
    u64 lines = (u64)(seconds * RTC_HZ / 2130);
    for (u64 i = 0; i < lines; i++) {
        s->time += 2130;
        rtc_line(s, (u32)(i % 263));
        if (s->ifl[1] & (1u << 7)) { n++; s->ifl[1] &= ~(1u << 7); }
    }
    return n;
}

static void test_reads(void) {
    u8 b[7];
    CHECK(rd1(STAT1) == 0x02, "status 1 %02x: 24-hour mode, no flags", rd1(STAT1));
    CHECK(rd1(STAT2) == 0x00, "status 2 %02x", rd1(STAT2));
    /* 2026-11-07 is a Saturday: 6 counting from Sunday; 12 o'clock reads with the PM bit */
    EXPECT_DATETIME("26 11 07 6 52 34 56");
    u8 t[3];
    rd(TIME, t, 3);
    CHECK(t[0] == 0x52 && t[1] == 0x34 && t[2] == 0x56, "time %02x %02x %02x", t[0], t[1], t[2]);
    /* emulated time moves it, a second per RTC_HZ bus cycles */
    advance(0.999);
    EXPECT_DATETIME("26 11 07 6 52 34 56");
    advance(0.002);
    EXPECT_DATETIME("26 11 07 6 52 34 57");
    advance(63);
    EXPECT_DATETIME("26 11 07 6 52 36 00");
    advance(3600 * 11.5);
    EXPECT_DATETIME("26 11 08 0 00 06 00");
    /* read the other way, with byte accesses */
    rd_gbatek(DATETIME | READ, b, 7);
    CHECK(b[0] == 0x26 && b[1] == 0x11 && b[2] == 0x08 && b[3] == 0 && b[4] == 0x00 && b[5] == 0x06 && b[6] == 0x00,
          "date and time read GBATEK's way %02x %02x %02x %x %02x %02x %02x", b[0], b[1], b[2], b[3], b[4], b[5], b[6]);
    /* the SIO line idles high (open drain) outside a read, and reads the CPU's own bit as an output */
    put(0x64);
    CHECK((get() & 1) == 1, "idle SIO %x", get() & 1);
    put(0x70);
    CHECK((get() & 0x7F) == 0x70, "register %04x", get());
    /* bits 16-31 read as zero; a 32-bit write reaches the register */
    wr32(&s->arm7, 0x04000138, 0xFFFF0070);
    CHECK(rd32(&s->arm7, 0x04000138) == 0x70, "32-bit read %08x", rd32(&s->arm7, 0x04000138));
}

static void test_rollover(void) {
    set_datetime(0x26, 0x12, 0x31, 4, 0x23, 0x59, 0x58);
    EXPECT_DATETIME("26 12 31 4 63 59 58");      /* a write takes effect at once */
    advance(2.5);
    EXPECT_DATETIME("27 01 01 5 00 00 00");      /* midnight, month and year */
    set_datetime(0x28, 0x02, 0x28, 1, 0x23, 0x59, 0x59);
    advance(1);
    EXPECT_DATETIME("28 02 29 2 00 00 00");      /* 2028 is a leap year */
    set_datetime(0x27, 0x02, 0x28, 0, 0x23, 0x59, 0x59);
    advance(1);
    EXPECT_DATETIME("27 03 01 1 00 00 00");
    set_datetime(0x26, 0x04, 0x30, 4, 0x23, 0x59, 0x59);
    advance(1);
    EXPECT_DATETIME("26 05 01 5 00 00 00");
    set_datetime(0x99, 0x12, 0x31, 6, 0x23, 0x59, 0x59);
    advance(1);
    EXPECT_DATETIME("00 01 01 0 00 00 00");      /* 2099 wraps to 2000 */
    set_datetime(0x00, 0x02, 0x28, 1, 0x23, 0x59, 0x59);
    advance(1);
    EXPECT_DATETIME("00 02 29 2 00 00 00");      /* so is 2000 */
    /* the day of week is a counter of its own: whatever was written, it counts on from there */
    set_datetime(0x26, 0x06, 0x01, 3, 0x23, 0x59, 0x59);
    advance(1);
    EXPECT_DATETIME("26 06 02 4 00 00 00");
    /* time only */
    u8 t[3] = {0x08, 0x15, 0x30};
    wr(TIME, t, 3);
    EXPECT_DATETIME("26 06 02 4 08 15 30");
    /* a read latches the clock at its command, so a carry mid-transfer can't tear it */
    set_datetime(0x26, 0x12, 0x31, 4, 0x23, 0x59, 0x59);
    advance(0.9);
    u8 b[7];
    begin(); send(DATETIME | READ);
    for (int i = 0; i < 7; i++) { b[i] = recv(); if (i == 2) advance(0.2); }
    end();
    CHECK(b[0] == 0x26 && b[2] == 0x31 && b[6] == 0x59, "torn read %02x %02x %02x", b[0], b[2], b[6]);
    EXPECT_DATETIME("27 01 01 5 00 00 00");
}

static void test_12h(void) {
    set_datetime(0x26, 0x07, 0x04, 6, 0x23, 0x59, 0x00);
    wr1(STAT1, 0x00);                               /* 12-hour mode */
    CHECK(rd1(STAT1) == 0x00, "status 1 %02x", rd1(STAT1));
    EXPECT_DATETIME("26 07 04 6 51 59 00");         /* 11 PM */
    advance(60);
    EXPECT_DATETIME("26 07 05 0 00 00 00");         /* 12 AM is 0 */
    advance(12 * 3600.0);
    EXPECT_DATETIME("26 07 05 0 40 00 00");         /* 12 PM is 0 with the PM bit */
    u8 t[3] = {0x47, 0x30, 0x00};                   /* 7:30 PM */
    wr(TIME, t, 3);
    EXPECT_DATETIME("26 07 05 0 47 30 00");
    wr1(STAT1, 0x02);                               /* back to 24-hour: the same time reads 19 */
    EXPECT_DATETIME("26 07 05 0 59 30 00");
    t[0] = 0x48;                                    /* 24-hour mode ignores a written PM bit */
    wr(TIME, t, 3);
    EXPECT_DATETIME("26 07 05 0 08 30 00");
    wr1(STAT1, 0x0E);                               /* the general purpose bits keep what's written */
    CHECK(rd1(STAT1) == 0x0E, "status 1 %02x", rd1(STAT1));
    wr1(STAT1, 0x02);
}

static void test_invalid(void) {
    /* out-of-range fields become their lowest value; a day past the month's end, the next 1st */
    set_datetime(0x9A, 0x13, 0x00, 7, 0x24, 0x60, 0x30);
    EXPECT_DATETIME("00 01 01 0 00 00 30");
    set_datetime(0x26, 0x04, 0x31, 2, 0x10, 0x00, 0x00);
    EXPECT_DATETIME("26 05 01 2 10 00 00");
    set_datetime(0x27, 0x02, 0x29, 2, 0x10, 0x00, 0x00);
    EXPECT_DATETIME("27 03 01 2 10 00 00");
    /* an invalid second carries into the minute a second later */
    set_datetime(0x26, 0x01, 0x01, 4, 0x10, 0x20, 0x75);
    advance(1);
    EXPECT_DATETIME("26 01 01 4 10 21 00");
    /* a write cut short changes nothing */
    set_datetime(0x26, 0x01, 0x01, 4, 0x10, 0x20, 0x00);
    begin(); send(DATETIME); send(0x30); send(0x02); end();
    EXPECT_DATETIME("26 01 01 4 10 20 00");
    /* a byte that isn't a command is skipped, and the next one taken as the command */
    begin(); send(0xA7); send(FREE); send(0x5A); end();
    CHECK(rd1(FREE) == 0x5A, "free register %02x after a bad command byte", rd1(FREE));
    /* so is the DSi's extended command code */
    begin(); send(0x0E | READ); send(FREE | READ); u8 v = recv(); end();
    CHECK(v == 0x5A, "free register %02x after an extended command", v);
}

static void test_registers(void) {
    wr1(FREE, 0xA5);
    CHECK(rd1(FREE) == 0xA5, "free register %02x", rd1(FREE));
    wr1(ADJUST, 0x3C);
    CHECK(rd1(ADJUST) == 0x3C, "clock adjustment %02x", rd1(ADJUST));
    /* INT1 is the frequency register (one byte) unless status 2 selects alarm 1 (three bytes);
     * the frequency register and alarm 1's minute are the same byte */
    wr1(STAT2, 0x01);
    wr1(INT1, 0x9F);
    CHECK(rd1(INT1) == 0x9F, "frequency register %02x", rd1(INT1));
    wr1(STAT2, 0x04);
    u8 a[3] = {0xFF, 0xD3, 0xC5}, b[3];
    rd(INT1, b, 3);
    CHECK(b[2] == 0x9F, "alarm 1 minute %02x is the frequency register", b[2]);
    wr(INT1, a, 3);
    rd(INT1, b, 3);
    CHECK(b[0] == 0x87 && b[1] == 0xD3 && b[2] == 0xC5, "alarm 1 %02x %02x %02x", b[0], b[1], b[2]);
    wr(INT2, a, 3);
    rd(INT2, b, 3);
    CHECK(b[0] == 0x87 && b[1] == 0xD3 && b[2] == 0xC5, "alarm 2 %02x %02x %02x", b[0], b[1], b[2]);
    wr1(STAT2, 0x00);
    CHECK(rd1(STAT2) == 0x00, "status 2 %02x", rd1(STAT2));
    /* reset: the initial date, registers cleared, the 12/24-hour bit taken from the same write */
    wr1(STAT1, 0x03);
    CHECK(rd1(STAT1) == 0x02, "status 1 after reset %02x", rd1(STAT1));
    CHECK(rd1(FREE) == 0 && rd1(ADJUST) == 0, "free %02x, adjustment %02x after reset", rd1(FREE), rd1(ADJUST));
    rd(INT2, b, 3);
    CHECK(!b[0] && !b[1] && !b[2], "alarm 2 after reset %02x %02x %02x", b[0], b[1], b[2]);
    EXPECT_DATETIME("00 01 01 0 00 00 00");
    advance(1.5);
    EXPECT_DATETIME("00 01 01 0 00 00 01");
    /* a write with SIO left as an input sends ones: nobody pulls the line low */
    begin(); send(FREE);
    for (int i = 0; i < 8; i++) { put(0x64); put(0x66); }
    end();
    CHECK(rd1(FREE) == 0xFF, "free register %02x written with SIO as an input", rd1(FREE));
    /* the ARM9 has no RTC: its accesses there don't reach the chip */
    u16 reg = s->rtc.reg;
    wr16(&s->arm9, 0x04000138, 0x72); wr16(&s->arm9, 0x04000138, 0x76);
    CHECK(s->rtc.reg == reg && s->rtc.state == 0, "an ARM9 write reached the RTC (%04x)", s->rtc.reg);
}

static void test_interrupts(void) {
    wr16(&s->arm7, 0x04000134, 0x8100);             /* RCNT: general purpose, SI interrupt */
    set_datetime(0x26, 0x03, 0x10, 2, 0x10, 0x00, 0x00);
    /* selected frequency: the output starts low (it falls as it is turned on, at the start of a
     * second here), then 1 Hz falls as each second begins and 16 Hz sixteen times a second. The
     * windows end away from an edge. */
    wr1(STAT2, 0x01);
    wr1(INT1, 0x01);
    int n = irqs_over(3.6);
    CHECK(n == 4, "1 Hz: %d interrupts in 3.6 s", n);
    wr1(INT1, 0x10);                                /* 0.6 s into a second: high, no edge */
    n = irqs_over(1.0);
    CHECK(n == 16, "16 Hz: %d interrupts in a second", n);
    /* 2 Hz and 8 Hz together (the datasheet's example): high only while both are, four falls a
     * second (at 6/16, 8/16, 14/16 and 16/16) */
    wr1(INT1, 0x0A);
    irqs_over(0.1);                                 /* the switch itself pulls it low */
    n = irqs_over(1.0);
    CHECK(n == 4, "2 Hz and 8 Hz: %d interrupts in a second", n);
    wr1(INT1, 0x00);
    CHECK(irqs_over(1.0) == 0, "no frequency selected");
    /* RCNT not in general-purpose mode, or the SI interrupt off: no interrupt */
    wr1(INT1, 0x10);
    wr16(&s->arm7, 0x04000134, 0x8000);
    CHECK(irqs_over(0.5) == 0, "SI interrupt disabled");
    wr16(&s->arm7, 0x04000134, 0x0100);
    CHECK(irqs_over(0.5) == 0, "RCNT in serial mode");
    wr16(&s->arm7, 0x04000134, 0x8100);
    wr1(INT1, 0x00);
    irqs_over(0.1);

    /* per-minute edge: one fall at the first minute carry, held until the mode is turned off */
    set_datetime(0x26, 0x03, 0x10, 2, 0x10, 0x00, 0x50);
    wr1(STAT2, 0x02);
    n = irqs_over(9.5);
    CHECK(n == 0, "per-minute edge before the minute: %d", n);
    n = irqs_over(61);
    CHECK(n == 1, "per-minute edge: %d interrupts over a minute carry and the next (held low)", n);
    wr1(STAT2, 0x00);
    irqs_over(0.1);
    wr1(STAT2, 0x02);
    n = irqs_over(60);
    CHECK(n == 1, "per-minute edge again: %d", n);
    wr1(STAT2, 0x00);

    /* per-minute steady 1: low for the first 30 seconds of each minute, from the first carry */
    set_datetime(0x26, 0x03, 0x10, 2, 0x10, 0x00, 0x50);
    wr1(STAT2, 0x03);
    n = irqs_over(10.5 + 180);
    CHECK(n == 4, "per-minute steady 1: %d interrupts over four minute carries", n);
    /* per-minute steady 2: a 7.8 ms pulse at each carry */
    wr1(STAT2, 0x07);
    n = irqs_over(120);
    CHECK(n == 2, "per-minute steady 2: %d interrupts over two minute carries", n);
    wr1(STAT2, 0x00);

    /* alarm 1 at 10:05 (24-hour, so with the PM bit clear): the INT1 flag, read once */
    set_datetime(0x26, 0x03, 0x10, 2, 0x10, 0x04, 0x30);
    wr1(STAT2, 0x04);
    u8 a[3] = {0x00, 0x80 | 0x10, 0x80 | 0x05};
    wr(INT1, a, 3);
    n = irqs_over(29.5);
    CHECK(n == 0 && !(rd1(STAT1) & 0x30), "alarm 1 early: %d", n);
    n = irqs_over(1);
    u8 st = rd1(STAT1);
    CHECK(n == 1 && (st & 0x30) == 0x10, "alarm 1: %d interrupts, status 1 %02x", n, st);
    CHECK(!(rd1(STAT1) & 0x30), "the alarm flag clears when read");
    wr1(STAT2, 0x00);
    /* alarm 2 every hour at :30 (only the minute compared), independent of INT1's mode */
    a[0] = 0x00; a[1] = 0x00; a[2] = 0x80 | 0x30;
    wr1(STAT2, 0x40);
    wr(INT2, a, 3);
    n = irqs_over(26 * 60.0);
    st = rd1(STAT1);
    CHECK(n == 1 && (st & 0x30) == 0x20, "alarm 2: %d interrupts, status 1 %02x", n, st);
    wr1(STAT2, 0x00);
    /* 32 kHz output: edges all the time */
    wr1(STAT2, 0x08);
    n = irqs_over(0.01);
    CHECK(n > 100, "32 kHz: %d interrupts in 10 ms", n);
    wr1(STAT2, 0x00);
    wr16(&s->arm7, 0x04000134, 0);
}

int main(void) {
    static u8 rom[0x1000];
    s = nds_create(rom, sizeof rom);
    CHECK(!nds_option(s, "--rtc", "2026-02-30T00:00:00"), "--rtc took February 30");
    CHECK(!nds_option(s, "--rtc", "2026-11-07 12:34:56"), "--rtc took a malformed time");
    CHECK(nds_option(s, "--rtc", "2026-11-07T12:34:56"), "--rtc refused");
    test_reads();
    test_rollover();
    test_12h();
    test_invalid();
    test_registers();
    test_interrupts();
    /* the default start is the host's local time (either side of the read, should the date
     * change in between) */
    time_t t0 = time(0);
    Nds *keep = s;
    s = nds_create(rom, sizeof rom);
    u8 b[7];
    rd(DATETIME, b, 7);
    time_t t1 = time(0);
    nds_shutdown(s);
    s = keep;
    int ok = 0;
    for (int i = 0; i < 2; i++) {
        struct tm tm;
        localtime_r(i ? &t1 : &t0, &tm);
        char want[16], got[16];
        snprintf(want, sizeof want, "%02d %02d %02d %d", tm.tm_year % 100, tm.tm_mon + 1, tm.tm_mday, tm.tm_wday);
        snprintf(got, sizeof got, "%02x %02x %02x %x", b[0], b[1], b[2], b[3]);
        ok |= !strcmp(want, got);
        if (i && !ok) { failures++; printf("host start %s, host date %s\n", got, want); }
    }
    nds_shutdown(s);
    if (failures) printf("%d failures\n", failures);
    return failures != 0;
}
