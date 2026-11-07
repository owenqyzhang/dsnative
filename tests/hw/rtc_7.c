/* The RTC test's ARM7 side: bit-bangs the real-time clock through 0x04000138 the way the Nitro
 * SDK does (16-bit writes, everything LSB first, read bits sampled while SCK is low) and
 * answers the ARM9's commands (rtc_9.c). */
#include "regs.h"

#define REG_RTC  R16(0x04000138)
#define REG_RCNT R16(0x04000134)
#define REG_VCOUNT7 R16(0x04000006)
#define IRQ_RTC (1u << 7)

enum { STAT1 = 0x06, DATETIME = 0x26, STAT2 = 0x46, INT1 = 0x16, TIME = 0x66, READ = 0x80 };

static void delay(int n) { for (volatile int i = 0; i < n; i++) ; }

static void begin(void) { REG_RTC = 0x72; delay(2); REG_RTC = 0x76; delay(2); }    /* CS low, SCK high; CS up */
static void end(void) { REG_RTC = (u16)(REG_RTC & ~4); delay(2); }
static void send(u32 b) {
    for (int i = 0; i < 8; i++) {
        u16 v = (u16)(0x74 | (b >> i & 1));
        REG_RTC = v; delay(4);
        REG_RTC = (u16)(v | 2); delay(4);
    }
}
static u32 recv(void) {
    u32 b = 0;
    for (int i = 0; i < 8; i++) {
        REG_RTC = 0x64; delay(4);           /* SIO an input, SCK low: the RTC puts out the bit */
        b |= (u32)(REG_RTC & 1) << i;
        REG_RTC = 0x66; delay(4);
    }
    return b;
}
static void rd(u32 cmd, u8 *out, int n) { begin(); send(cmd | READ); for (int i = 0; i < n; i++) out[i] = (u8)recv(); end(); }
static void wr(u32 cmd, const u8 *in, int n) { begin(); send(cmd); for (int i = 0; i < n; i++) send(in[i]); end(); }

static u8 dt[7], stat[2];

/* lines until the ARM7's RTC interrupt flag comes up (0xFFFF: not within ten frames) */
static u32 lines_to_irq(void) {
    u32 n = 0;
    u16 v = REG_VCOUNT7;
    while (!(REG_IF & IRQ_RTC)) {
        while (REG_VCOUNT7 == v) ;
        v = REG_VCOUNT7;
        if (++n == 2630) return 0xFFFF;
    }
    REG_IF = IRQ_RTC;
    return n;
}

/* the commands */
static u32 read_clock(void) {               /* the status registers and the date and time */
    rd(STAT1, &stat[0], 1);
    rd(STAT2, &stat[1], 1);
    rd(DATETIME, dt, 7);
    return 0;
}
static u32 last_date(void) { return dt[0] | dt[1] << 8 | dt[2] << 16 | (u32)dt[3] << 24; }
static u32 last_time(void) { return dt[4] | dt[5] << 8 | dt[6] << 16; }
static u32 last_status(void) { return stat[0] | stat[1] << 8; }

/* writes a date and time and reads it back; answers the hour in 12-hour mode */
static u32 write_clock(void) {
    static const u8 set[7] = {0x99, 0x12, 0x31, 0x04, 0x23, 0x59, 0x59};
    u8 st, t[3];
    wr(DATETIME, set, 7);
    rd(DATETIME, dt, 7);
    st = 0x00;
    wr(STAT1, &st, 1);
    rd(TIME, t, 3);
    st = 0x02;
    wr(STAT1, &st, 1);
    return t[0];
}

/* /INT on RCNT's SI line: the 16 Hz output falls as it is turned on at the start of a second,
 * then every 1/16 s (983 lines). Answers the lines to each interrupt. */
static u32 interrupt(void) {
    static const u8 ten[3] = {0x10, 0x00, 0x00};
    u8 v = 0x01;
    wr(TIME, ten, 3);                       /* the second starts now */
    REG_IF = IRQ_RTC;
    REG_RCNT = 0x8100;                      /* general purpose, SI interrupt */
    wr(STAT2, &v, 1);                       /* selected frequency */
    v = 0x10;
    wr(INT1, &v, 1);                        /* 16 Hz */
    u32 first = lines_to_irq(), second = lines_to_irq();
    v = 0;
    wr(STAT2, &v, 1);
    REG_RCNT = 0;
    REG_IF = IRQ_RTC;
    return first | second << 16;
}

/* A table of functions rather than a switch: GCC's ARMv4T Thumb switch table (absolute
 * addresses, ldr then mov pc) isn't something code discovery follows yet. */
static u32 (*const commands[])(void) = {read_clock, last_date, last_time, last_status, write_clock, interrupt};

u32 rtc_command7(u32 arg) {
    return arg < sizeof commands / sizeof *commands ? commands[arg]() : 0xBADC0DEu;
}
