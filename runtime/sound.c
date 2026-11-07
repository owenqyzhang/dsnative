/* dsnative runtime: the ARM7's sound channels, capture and mixer (0x04000400-0x0400051F). Not implemented yet. */
#include "nds.h"

void sound_init(Nds *s) { (void)s; }
int sound_io_read(Nds *s, int cpu, u32 off, int size, u32 *v) { (void)s; (void)cpu; (void)off; (void)size; (void)v; return 0; }
int sound_io_write(Nds *s, int cpu, u32 off, int size, u32 v) { (void)s; (void)cpu; (void)off; (void)size; (void)v; return 0; }
int sound_option(Nds *s, const char *name, const char *value) { (void)s; (void)name; (void)value; return 0; }
void sound_shutdown(Nds *s) { (void)s; }
void sound_line(Nds *s, u32 line) { (void)s; (void)line; }
u32 nds_audio_take(Nds *s, s16 *out, u32 max_frames) { (void)s; (void)out; (void)max_frames; return 0; }
