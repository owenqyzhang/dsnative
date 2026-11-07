/* dsnative runtime: the ARM7's sound channels, capture and mixer (0x04000400-0x0400051F). Not implemented yet. */
#ifndef SOUND_H
#define SOUND_H
#include "dsr.h"

struct Nds;

typedef struct {
    int unused;
} Sound;

void sound_init(struct Nds *s);
/* An I/O access (offset from 0x04000000, size 1, 2 or 4) this subsystem handles: returns 1
 * (with *v for a read), or 0 to leave it to nds.c. */
int sound_io_read(struct Nds *s, int cpu, u32 off, int size, u32 *v);
int sound_io_write(struct Nds *s, int cpu, u32 off, int size, u32 v);
/* A command-line style option ("--name", "value"): returns 1 if it is this subsystem's. */
int sound_option(struct Nds *s, const char *name, const char *value);
void sound_shutdown(struct Nds *s);
void sound_line(struct Nds *s, u32 line);
/* nds_audio_take (nds.h) is defined in sound.c */
#endif
