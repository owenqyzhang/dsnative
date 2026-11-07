/* The ARM7 side of the save memory test: with the NDS slot lent to it (EXMEMCNT bit 11), reads
 * a page of the FLASH at the address it is sent and answers its hash. */
#include "save_spi.h"

u32 save_command7(u32 addr) {
    return spi_read_hash(0x03, addr, 256);
}
