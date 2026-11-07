/* dsnative runtime: the ARM7's real-time clock (RTC, 0x04000138), a Seiko S-35180 on a 3-wire
 * serial bus the ARM7 bit-bangs. */
#ifndef RTC_H
#define RTC_H
#include "dsr.h"

struct Nds;

#define RTC_HZ 33513982u        /* bus cycles per second: the clock counts emulated time */

typedef struct {
    /* the clock, in binary: year 0-99 (2000-2099), month 1-12, day 1-31, wday 0-6 (Sunday 0),
     * hour 0-23 whatever the 12/24-hour mode */
    u8 year, month, day, wday, hour, min, sec;
    u64 tick;                   /* s->time of the clock's last second, or of its setting */
    u64 carry;                  /* s->time of the last minute carry */
    /* registers, in GBATEK's (LSB-first) bit order */
    u8 stat1, stat2;
    u8 alarm[2][3];             /* INT1, INT2: day of week, hour, minute; [0][2] doubles as INT1's frequency register */
    u8 adjust, free;            /* clock adjustment (kept, no effect) and the free register */
    /* the serial interface */
    u16 reg;                    /* 0x04000138 as last written */
    u8 state;                   /* idle, command, read, write (rtc.c) */
    u8 cmd;                     /* command, as GBATEK's LSB-first ("Fwd") number */
    u8 nbits, shift;            /* bits of the byte being shifted, and the byte */
    u8 pos, len;                /* parameter byte and count */
    u8 sio;                     /* what the RTC drives on SIO: 1 when released (open drain) */
    u8 buf[7];
    /* the /INT line to the ARM7 (RCNT's SI input) */
    u8 latch;                   /* bit 0: INT1, bit 1: INT2 held low until their mode is turned off */
    u8 armed;                   /* minute-periodical modes: a minute carry since the mode was set */
    u8 low;                     /* the line at the last rtc_line */
} Rtc;

void rtc_init(struct Nds *s);
/* An I/O access (offset from 0x04000000, size 1, 2 or 4) this subsystem handles: returns 1
 * (with *v for a read), or 0 to leave it to nds.c. */
int rtc_io_read(struct Nds *s, int cpu, u32 off, int size, u32 *v);
int rtc_io_write(struct Nds *s, int cpu, u32 off, int size, u32 v);
/* A command-line style option ("--name", "value"): returns 1 if it is this subsystem's.
 * "--rtc YYYY-MM-DDTHH:MM:SS" starts the clock there instead of at the host's local time. */
int rtc_option(struct Nds *s, const char *name, const char *value);
void rtc_shutdown(struct Nds *s);
/* End of a scanline: the /INT line, which interrupts the ARM7 through RCNT. */
void rtc_line(struct Nds *s, u32 line);
#endif
