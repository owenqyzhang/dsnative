/* dsnative runtime: the ARM7's real-time clock (RTC, 0x04000138). Not implemented yet. */
#ifndef RTC_H
#define RTC_H
#include "dsr.h"

struct Nds;

typedef struct {
    int unused;
} Rtc;

void rtc_init(struct Nds *s);
/* An I/O access (offset from 0x04000000, size 1, 2 or 4) this subsystem handles: returns 1
 * (with *v for a read), or 0 to leave it to nds.c. */
int rtc_io_read(struct Nds *s, int cpu, u32 off, int size, u32 *v);
int rtc_io_write(struct Nds *s, int cpu, u32 off, int size, u32 v);
/* A command-line style option ("--name", "value"): returns 1 if it is this subsystem's. */
int rtc_option(struct Nds *s, const char *name, const char *value);
void rtc_shutdown(struct Nds *s);
#endif
