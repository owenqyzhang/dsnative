/* dsnative runtime: the ARM9's 3D engine: geometry engine and rasterizer (0x04000320-0x040006A3). Not implemented yet. */
#ifndef GPU3D_H
#define GPU3D_H
#include "dsr.h"

struct Nds;

typedef struct {
    int unused;
} Gpu3d;

void gpu3d_init(struct Nds *s);
/* An I/O access (offset from 0x04000000, size 1, 2 or 4) this subsystem handles: returns 1
 * (with *v for a read), or 0 to leave it to nds.c. */
int gpu3d_io_read(struct Nds *s, int cpu, u32 off, int size, u32 *v);
int gpu3d_io_write(struct Nds *s, int cpu, u32 off, int size, u32 v);
/* A command-line style option ("--name", "value"): returns 1 if it is this subsystem's. */
int gpu3d_option(struct Nds *s, const char *name, const char *value);
void gpu3d_shutdown(struct Nds *s);
void gpu3d_line(struct Nds *s, u32 line);
#endif
