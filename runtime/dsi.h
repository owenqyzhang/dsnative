/* dsnative runtime: the DSi-only registers a DS-mode game probes (GBATEK "DSi I/O Map", "DSi
 * Control Registers (SCFG)"): what a DSi answers while running a DS cartridge, which is also what
 * a DS answers. The SCFG and MBK registers (0x04004000-0x04004063) and every other new block are
 * locked off (reads give 0, writes are dropped); SNDEXCNT and the GPIO registers work. */
#ifndef DSI_H
#define DSI_H
#include "dsr.h"

struct Nds;

typedef struct {
    u16 sndexcnt;               /* 0x04004700, ARM7 */
    u8 gpio_out, gpio_dir, gpio_edge, gpio_ien;     /* 0x04004C00-0x04004C03, ARM7 */
    u16 gpio_wifi;              /* 0x04004C04, ARM7 */
} Dsi;

void dsi_init(struct Nds *s);
/* An I/O access (offset from 0x04000000, size 1, 2 or 4) this subsystem handles: returns 1
 * (with *v for a read), or 0 to leave it to nds.c. */
int dsi_io_read(struct Nds *s, int cpu, u32 off, int size, u32 *v);
int dsi_io_write(struct Nds *s, int cpu, u32 off, int size, u32 v);
#endif
