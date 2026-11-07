/* The RTC test's ARM9 side: has the ARM7 (rtc_7.c) read the clock, which headless --rtc starts
 * at a fixed time (tests/hw/expect_rtc.txt), write it, and take its interrupt; prints what came
 * back. */
#include "regs.h"

static void put_digit(u32 v) { char b[2] = {"0123456789abcdef"[v & 15], 0}; dbg(b); }
static void put_bcd(u32 v) { put_digit(v >> 4); put_digit(v); }

/* "YYYY-MM-DD W HH:MM:SS" from the ARM7's last read (BCD, as read) */
static void put_datetime(void) {
    u32 d = arm7_command(CMD7_RTC, 1), t = arm7_command(CMD7_RTC, 2);
    dbg("20"); put_bcd(d); dbg("-"); put_bcd(d >> 8); dbg("-"); put_bcd(d >> 16); dbg(" "); put_digit(d >> 24);
    dbg(" "); put_bcd(t & 0x3F); dbg(":"); put_bcd(t >> 8); dbg(":"); put_bcd(t >> 16);    /* hour bit 6: PM */
}

void rtc_test9(void) {
    arm7_command(CMD7_RTC, 0);
    dbg("rtc "); put_datetime(); dbg("\n");
    u32 st = arm7_command(CMD7_RTC, 3);
    dbg("rtcstat "); put_bcd(st); dbg(" "); put_bcd(st >> 8); dbg("\n");
    /* a write read back at once, then the hour register in 12-hour mode */
    u32 h12 = arm7_command(CMD7_RTC, 4);
    dbg("rtcwrite "); put_datetime(); dbg(" "); put_bcd(h12); dbg("\n");
    /* the interrupt: at once as the output is turned on, then a 16 Hz period (983 lines) later */
    u32 irq = arm7_command(CMD7_RTC, 5), first = irq & 0xFFFF, second = irq >> 16;
    if (first <= 1 && second >= 975 && second <= 990) dbg("rtcirq ok\n");
    else { dbg("rtcirq FAIL "); dbg_hex(irq); dbg("\n"); }
}
