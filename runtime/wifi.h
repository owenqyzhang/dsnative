/* dsnative runtime: the ARM7's Wi-Fi hardware (0x04800000-0x0480FFFF) as a stub that brings the
 * SDK's initialization through and reports no network: the ports with their reset values and
 * writable bits, the baseband and RF chips behind their serial interfaces (always ready), the
 * power states, the 8 KB Wi-Fi RAM and its buffer ports, the interrupt flags. Nothing is ever
 * received; transmit requests are accepted and not carried out. */
#ifndef WIFI_H
#define WIFI_H
#include "dsr.h"

struct Nds;

typedef struct {
    u16 reg[0x800];             /* the ports at 0x04808000 + 2 * i */
    u8 ram[0x2000];             /* 0x04804000-0x04805FFF */
    u8 bb[0x69];                /* baseband chip registers 00h-68h */
    u32 rf[0x20];               /* RF chip registers, 18 bits each */
    u8 power_off;               /* W_POWERSTATE bit 9 */
    u16 rand_latch;             /* the value the next read of W_RANDOM returns */
    u16 irq_active;             /* W_IF & W_IE at the last check */
    u64 us_time, us_count;      /* the microsecond counter: s->time and its value when last set */
} Wifi;

void wifi_init(struct Nds *s);
/* An I/O access (offset from 0x04000000, size 1, 2 or 4) this subsystem handles: returns 1
 * (with *v for a read), or 0 to leave it to nds.c. */
int wifi_io_read(struct Nds *s, int cpu, u32 off, int size, u32 *v);
int wifi_io_write(struct Nds *s, int cpu, u32 off, int size, u32 v);
#endif
