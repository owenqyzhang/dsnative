/* dsnative runtime: the ARM9's 3D engine: geometry engine and rasterizer (0x04000320-0x040006A3). Not implemented yet. */
#include "nds.h"

void gpu3d_init(Nds *s) { (void)s; }
int gpu3d_io_read(Nds *s, int cpu, u32 off, int size, u32 *v) { (void)s; (void)cpu; (void)off; (void)size; (void)v; return 0; }
int gpu3d_io_write(Nds *s, int cpu, u32 off, int size, u32 v) { (void)s; (void)cpu; (void)off; (void)size; (void)v; return 0; }
int gpu3d_option(Nds *s, const char *name, const char *value) { (void)s; (void)name; (void)value; return 0; }
void gpu3d_shutdown(Nds *s) { (void)s; }
void gpu3d_line(Nds *s, u32 line) { (void)s; (void)line; }
