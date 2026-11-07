/* dsnative runtime: the cartridge's save memory on the AUXSPI bus (0x040001A0-0x040001A3). Not implemented yet. */
#ifndef SAVE_H
#define SAVE_H
#include "dsr.h"

struct Nds;

typedef struct {
    int unused;
} Save;

void save_init(struct Nds *s);
/* An I/O access (offset from 0x04000000, size 1, 2 or 4) this subsystem handles: returns 1
 * (with *v for a read), or 0 to leave it to nds.c. */
int save_io_read(struct Nds *s, int cpu, u32 off, int size, u32 *v);
int save_io_write(struct Nds *s, int cpu, u32 off, int size, u32 v);
/* A command-line style option ("--name", "value"): returns 1 if it is this subsystem's. */
int save_option(struct Nds *s, const char *name, const char *value);
void save_shutdown(struct Nds *s);
#endif
