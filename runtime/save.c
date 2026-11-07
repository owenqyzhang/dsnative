/* dsnative runtime: the cartridge's save memory on the AUXSPI bus (0x040001A0-0x040001A3). Not implemented yet. */
#include "nds.h"

void save_init(Nds *s) { (void)s; }
int save_io_read(Nds *s, int cpu, u32 off, int size, u32 *v) { (void)s; (void)cpu; (void)off; (void)size; (void)v; return 0; }
int save_io_write(Nds *s, int cpu, u32 off, int size, u32 v) { (void)s; (void)cpu; (void)off; (void)size; (void)v; return 0; }
int save_option(Nds *s, const char *name, const char *value) { (void)s; (void)name; (void)value; return 0; }
void save_shutdown(Nds *s) { (void)s; }
