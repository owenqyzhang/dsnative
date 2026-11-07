/* dsnative runtime: the ARM7's real-time clock (RTC, 0x04000138). Not implemented yet. */
#include "nds.h"

void rtc_init(Nds *s) { (void)s; }
int rtc_io_read(Nds *s, int cpu, u32 off, int size, u32 *v) { (void)s; (void)cpu; (void)off; (void)size; (void)v; return 0; }
int rtc_io_write(Nds *s, int cpu, u32 off, int size, u32 v) { (void)s; (void)cpu; (void)off; (void)size; (void)v; return 0; }
int rtc_option(Nds *s, const char *name, const char *value) { (void)s; (void)name; (void)value; return 0; }
void rtc_shutdown(Nds *s) { (void)s; }
