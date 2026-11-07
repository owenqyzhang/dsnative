/* dsnative runtime: the ARM7's real-time clock (RTC, 0x04000138). References: GBATEK "DS
 * Real-Time Clock (RTC)", and the S-35190A datasheet (Seiko/ABLIC, the same chip family) for
 * what GBATEK leaves open: which clock edge moves data, invalid dates, the state after a reset
 * and the /INT output modes.
 *
 * The ARM7 drives CS, SCK and SIO through the register. CS rising starts a transfer. The RTC
 * samples SIO on each rising SCK edge: a command byte (fixed code 0110, a 3-bit command, then
 * read/write), then the parameter bytes, all LSB first. When reading, the RTC drives SIO from
 * each falling edge: the Nitro SDK samples it while SCK is still low, GBATEK's flowchart once
 * SCK is high again, and both see the same bit. The clock runs on emulated time (s->time), so
 * a run is reproducible given its start (--rtc). */
#define _POSIX_C_SOURCE 200809L     /* localtime_r */
#include <stdio.h>
#include <time.h>
#include "nds.h"

enum { ST_IDLE, ST_CMD, ST_READ, ST_WRITE };
/* commands, as GBATEK's LSB-first ("Fwd") numbers */
enum { C_STAT1 = 0, C_INT1 = 1, C_DATETIME = 2, C_ADJUST = 3, C_STAT2 = 4, C_INT2 = 5, C_TIME = 6, C_FREE = 7 };
#define IRQ_RTC (1u << 7)           /* ARM7 IE/IF bit 7: SIO/RCNT, where /INT arrives */

static u8 bcd(u32 v) { return (u8)((v / 10) << 4 | v % 10); }
/* a BCD byte's value, or -1 when a digit is not decimal */
static int unbcd(u32 v) { return (v & 0xF) > 9 || (v >> 4) > 9 ? -1 : (int)((v >> 4) * 10 + (v & 0xF)); }

static int days_in(int year, int month) {       /* year 0-99 (2000-2099): every fourth is leap */
    static const u8 n[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && !(year & 3) ? 29 : n[month - 1];
}

static int weekday(int y, int m, int d) {       /* Sakamoto's method; 0 = Sunday, as the SDK counts */
    static const u8 t[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y--;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

/* the hour register as it reads: 12-hour mode counts 0-11 (12 o'clock is 0); bit 6 is PM in both */
static u8 hour_reg(const Rtc *r) {
    return (u8)(bcd((r->stat1 & 2) ? r->hour : r->hour % 12u) | (r->hour >= 12 ? 0x40 : 0));
}

/* An alarm compares the registers as they read, so in 24-hour mode its PM bit must be right too
 * (GBATEK; the datasheet's 24-hour example sets it). Bit 7 of each byte enables its compare. */
static int alarm_match(const Rtc *r, int i) {
    const u8 *a = r->alarm[i];
    return (!(a[0] & 0x80) || (a[0] & 7) == r->wday) &&
           (!(a[1] & 0x80) || (a[1] & 0x7F) == hour_reg(r)) &&
           (!(a[2] & 0x80) || (a[2] & 0x7F) == bcd(r->min));
}

/* INT1's output mode, status 2 bits 0-3 (the datasheet's INT1FE, INT1ME, INT1AE, 32kE) */
static u8 int1_mode(const Rtc *r) { return r->stat2 & 0xF; }
static int mode_freq(u8 m) { return (m & 0xB) == 1; }          /* selected frequencies */
static int mode_edge(u8 m) { return (m & 0xB) == 2; }          /* per-minute edge */
static int mode_periodic(u8 m) { return m == 3 || m == 7; }    /* per-minute steady 1 and 2 */
static int mode_alarm(u8 m) { return m == 4; }
static int mode_32k(u8 m) { return m >> 3; }

static void minute_carry(Rtc *r) {
    r->carry = r->tick;
    if (++r->min == 60) {
        r->min = 0;
        if (++r->hour == 24) {
            r->hour = 0;
            r->wday = (u8)((r->wday + 1) % 7);
            if (++r->day > days_in(r->year, r->month)) {
                r->day = 1;
                if (++r->month == 13) { r->month = 1; r->year = (u8)((r->year + 1) % 100); }
            }
        }
    }
    /* the alarms and the per-minute modes act on minute carries only (datasheet), so they hold
     * /INT low until the program turns their mode off */
    u8 m = int1_mode(r);
    if (mode_edge(m)) r->latch |= 1;
    if (mode_periodic(m)) r->armed = 1;
    if (mode_alarm(m) && alarm_match(r, 0)) { r->latch |= 1; r->stat1 |= 0x10; }
    if ((r->stat2 & 0x40) && alarm_match(r, 1)) { r->latch |= 2; r->stat1 |= 0x20; }
}

/* Brings the clock up to s->time. */
static void sync(Nds *s) {
    Rtc *r = &s->rtc;
    if (s->time < r->tick) return;
    u64 n = (s->time - r->tick) / RTC_HZ;
    while (n) {
        u32 step = 60u - r->sec;            /* seconds to the next minute carry */
        if (n < step) { r->sec = (u8)(r->sec + n); r->tick += n * RTC_HZ; return; }
        n -= step;
        r->tick += (u64)step * RTC_HZ;
        r->sec = 0;
        minute_carry(r);
    }
}

static void set_clock(Nds *s, int year, int month, int day, int wday, int hour, int min, int sec) {
    Rtc *r = &s->rtc;
    r->year = (u8)year; r->month = (u8)month; r->day = (u8)day; r->wday = (u8)wday;
    r->hour = (u8)hour; r->min = (u8)min; r->sec = (u8)sec;
    r->tick = s->time;
}

static void clock_from_host(Nds *s) {
    time_t now = time(0);
    struct tm tm;
    localtime_r(&now, &tm);
    int y = tm.tm_year - 100;               /* the RTC counts 2000-2099 */
    set_clock(s, y < 0 ? 0 : y % 100, tm.tm_mon + 1, tm.tm_mday, tm.tm_wday, tm.tm_hour, tm.tm_min,
              tm.tm_sec > 59 ? 59 : tm.tm_sec);
}

/* A reset (status 1 bit 0): the datasheet's state after initialization, which keeps the
 * 12/24-hour and general purpose bits written along with the reset. */
static void reset(Nds *s, u8 v) {
    Rtc *r = &s->rtc;
    set_clock(s, 0, 1, 1, 0, 0, 0, 0);
    r->stat1 = v & 0x0E; r->stat2 = 0;
    memset(r->alarm, 0, sizeof r->alarm);
    r->adjust = r->free = 0;
    r->latch = r->armed = 0;
}

/* Real-time data is checked as the datasheet says: a field out of range becomes its lowest
 * value, and a day past the end of its month the first of the next month. */
static void set_date(Rtc *r, const u8 *b) {
    int y = unbcd(b[0]), m = unbcd(b[1] & 0x1F), d = unbcd(b[2] & 0x3F), w = b[3] & 7;
    if (y < 0) y = 0;
    if (m < 1 || m > 12) m = 1;
    if (d < 1 || d > 31) d = 1;
    else if (d > days_in(y, m)) { d = 1; if (++m == 13) { m = 1; y = (y + 1) % 100; } }
    r->year = (u8)y; r->month = (u8)m; r->day = (u8)d; r->wday = (u8)(w == 7 ? 0 : w);
}

static void set_time(Nds *s, const u8 *b) {
    Rtc *r = &s->rtc;
    int h = unbcd(b[0] & 0x3F), m = unbcd(b[1] & 0x7F), sec = unbcd(b[2] & 0x7F);
    if (r->stat1 & 2) { if (h < 0 || h > 23) h = 0; }       /* 24-hour: the PM bit is ignored */
    else { if (h < 0 || h > 11) h = 0; if (b[0] & 0x40) h += 12; }
    if (m < 0 || m > 59) m = 0;
    if (sec < 0 || sec > 59) sec = 59;      /* an invalid second carries a second later */
    r->hour = (u8)h; r->min = (u8)m; r->sec = (u8)sec;
    r->tick = s->time;                      /* the second starts now */
}

static u8 param_len(const Rtc *r, u8 cmd) {
    switch (cmd) {
    case C_DATETIME: return 7;
    case C_TIME: case C_INT2: return 3;
    case C_INT1: return (r->stat2 & 4) ? 3 : 1;     /* alarm 1, or the frequency register */
    default: return 1;
    }
}

/* A read latches its parameters when the command arrives, so a carry during the transfer
 * can't tear the date (datasheet). */
static void start_read(Nds *s) {
    Rtc *r = &s->rtc;
    u8 *b = r->buf;
    sync(s);
    switch (r->cmd) {
    case C_STAT1: b[0] = r->stat1; r->stat1 &= 0x0F; break;    /* the flags clear on read */
    case C_STAT2: b[0] = r->stat2; break;
    case C_DATETIME:
        b[0] = bcd(r->year); b[1] = bcd(r->month); b[2] = bcd(r->day); b[3] = r->wday;
        b[4] = hour_reg(r); b[5] = bcd(r->min); b[6] = bcd(r->sec);
        break;
    case C_TIME: b[0] = hour_reg(r); b[1] = bcd(r->min); b[2] = bcd(r->sec); break;
    case C_INT1: if (r->len == 3) memcpy(b, r->alarm[0], 3); else b[0] = r->alarm[0][2]; break;
    case C_INT2: memcpy(b, r->alarm[1], 3); break;
    case C_ADJUST: b[0] = r->adjust; break;
    case C_FREE: b[0] = r->free; break;
    }
}

/* A write takes effect with its last byte; a transfer cut short changes nothing. */
static void finish_write(Nds *s) {
    Rtc *r = &s->rtc;
    const u8 *b = r->buf;
    sync(s);
    switch (r->cmd) {
    case C_STAT1:
        if (b[0] & 1) reset(s, b[0]);
        else r->stat1 = (u8)((r->stat1 & 0xF0) | (b[0] & 0x0E));
        break;
    case C_STAT2:
        /* a held INT1 output ends when its mode changes, INT2's when its enable is cleared */
        if ((r->stat2 ^ b[0]) & 0xF) { r->latch &= ~1; r->armed = 0; }
        if (!(b[0] & 0x40)) r->latch &= ~2;
        r->stat2 = b[0];
        break;
    case C_DATETIME: set_date(r, b); set_time(s, b + 4); break;
    case C_TIME: set_time(s, b); break;
    case C_INT1:
        if (r->len == 1) { r->alarm[0][2] = b[0]; break; }
        /* fall through: alarm 1 */
    case C_INT2: {
        u8 *a = r->alarm[r->cmd == C_INT2];
        a[0] = b[0] & 0x87; a[1] = b[1]; a[2] = b[2];
        break;
    }
    case C_ADJUST: r->adjust = b[0]; break;
    case C_FREE: r->free = b[0]; break;
    }
}

/* SIO is open drain on the RTC's side: low if either end pulls it low, high (pulled up) when
 * the CPU has it as an input and the RTC lets go */
static u32 sio_line(const Rtc *r) { return ((r->reg & 0x10) ? r->reg & 1u : 1u) & r->sio; }

static void reg_write(Nds *s, u16 v) {
    Rtc *r = &s->rtc;
    u16 old = r->reg;
    r->reg = v;
    if (!(old & 4)) {
        if (v & 4) { r->state = ST_CMD; r->nbits = 0; r->shift = 0; r->sio = 1; }  /* CS up: a transfer */
        return;
    }
    if (!(v & 4)) { r->state = ST_IDLE; r->sio = 1; return; }
    if (!(old & 2) && (v & 2)) {            /* SCK rising: the RTC samples SIO */
        if (r->state != ST_CMD && r->state != ST_WRITE) return;
        r->shift |= (u8)(sio_line(r) << r->nbits);
        if (++r->nbits < 8) return;
        u8 b = r->shift;
        r->nbits = 0; r->shift = 0;
        if (r->state == ST_WRITE) {
            r->buf[r->pos++] = b;
            if (r->pos == r->len) { finish_write(s); r->state = ST_IDLE; }
            return;
        }
        /* Not a command (this includes the DSi's extended ones, fixed code 0111): the chip goes
         * on looking for one in the next byte (datasheet). */
        if ((b & 0xF) != 6) return;
        r->cmd = b >> 4 & 7; r->len = param_len(r, r->cmd); r->pos = 0;
        if (b & 0x80) { r->state = ST_READ; start_read(s); }
        else r->state = ST_WRITE;
    } else if ((old & 2) && !(v & 2) && r->state == ST_READ) {     /* SCK falling: the next bit out */
        r->sio = r->pos < r->len ? (r->buf[r->pos] >> r->nbits) & 1 : 1;
        if (++r->nbits == 8) { r->nbits = 0; r->pos++; }
    }
}

/* SIO set as an input reads the line; as an output, what the CPU wrote */
static u16 reg_read(const Rtc *r) { return (r->reg & 0x10) ? r->reg : (u16)((r->reg & ~1u) | sio_line(r)); }

int rtc_io_read(Nds *s, int cpu, u32 off, int size, u32 *v) {
    if (cpu != 1 || off < 0x138 || off > 0x13B) return 0;
    u32 x = (u32)reg_read(&s->rtc) >> ((off - 0x138) * 8);     /* bits 16-31 are unused */
    *v = size == 4 ? x : size == 2 ? x & 0xFFFF : x & 0xFF;
    return 1;
}

int rtc_io_write(Nds *s, int cpu, u32 off, int size, u32 v) {
    if (cpu != 1 || off < 0x138 || off > 0x13B) return 0;
    u32 sh = (off - 0x138) * 8;
    u32 mask = (size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu : 0xFFu) << sh;
    reg_write(s, (u16)((s->rtc.reg & ~mask) | ((v << sh) & mask)));
    return 1;
}

/* the frequency output: 2^k Hz square waves in step with the seconds, each low for the first
 * half of its period (the datasheet's 1 Hz falls as the seconds count), ANDed: any low wave
 * pulls /INT low */
static int freq_low(const Nds *s) {
    const Rtc *r = &s->rtc;
    u64 frac = s->time - r->tick;           /* into the current second (just synced) */
    for (int k = 0; k < 5; k++)
        if ((r->alarm[0][2] >> k & 1) && !(((frac << (k + 1)) / RTC_HZ) & 1)) return 1;
    return 0;
}

void rtc_line(Nds *s, u32 line) {
    Rtc *r = &s->rtc;
    (void)line;
    /* with no /INT source on, the clock needn't keep up: it syncs when read or written */
    if (!(r->stat2 & 0x4F) && !r->latch) { r->low = 0; return; }
    sync(s);
    u8 m = int1_mode(r);
    int low = r->latch != 0;                /* open drain: any source holding it low wins */
    if (!low && mode_freq(m)) low = freq_low(s);
    if (!low && m == 3) low = r->armed && r->sec < 30;                         /* 30 s of each minute */
    if (!low && m == 7) low = r->armed && s->time - r->carry < RTC_HZ / 128;   /* 7.8 ms */
    /* 32 kHz toggles many times a line: one interrupt a line stands for those edges */
    int edge = (low && !r->low) || (!low && mode_32k(m));
    r->low = (u8)low;
    /* /INT is RCNT's SI input: it interrupts on a falling edge in general-purpose mode with the
     * SI interrupt enabled (GBATEK: RCNT = 8100h or 8144h) */
    u16 rcnt = (u16)(s->io[1][0x134] | s->io[1][0x135] << 8);
    if (edge && (rcnt & 0xC100) == 0x8100) nds_request_irq(s, 1, IRQ_RTC);
}

void rtc_init(Nds *s) {
    Rtc *r = &s->rtc;
    memset(r, 0, sizeof *r);
    clock_from_host(s);
    r->stat1 = 2;                           /* 24-hour mode, the SDK's; no power-loss flags: a set clock */
    r->sio = 1;
}

int rtc_option(Nds *s, const char *name, const char *value) {
    if (strcmp(name, "--rtc")) return 0;
    int y, mo, d, h, mi, sec;
    char end;
    if (sscanf(value, "%4d-%2d-%2dT%2d:%2d:%2d%c", &y, &mo, &d, &h, &mi, &sec, &end) != 6 ||
        y < 2000 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > days_in(y - 2000, mo) ||
        h < 0 || h > 23 || mi < 0 || mi > 59 || sec < 0 || sec > 59) {
        fprintf(stderr, "--rtc %s: expected a time YYYY-MM-DDTHH:MM:SS in 2000-2099\n", value);
        return 0;
    }
    set_clock(s, y - 2000, mo, d, weekday(y, mo, d), h, mi, sec);
    return 1;
}

void rtc_shutdown(Nds *s) { (void)s; }
