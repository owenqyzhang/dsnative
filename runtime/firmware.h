/* dsnative runtime: the firmware flash on the ARM7's SPI bus (device 1). A 256 KB ST M45PE20
 * holding what a console that has been through the boot menu once holds: the header and the
 * Wi-Fi hardware settings, three access point slots (none configured), the user settings. */
#ifndef FIRMWARE_H
#define FIRMWARE_H
#include "dsr.h"

struct Nds;

#define FW_SIZE 0x40000
#define FW_USER_SETTINGS 0x3FE00        /* two areas of 100h bytes: the newer one is the current settings */
#define FW_AP_SLOTS 0x3FA00             /* three access point slots, then the hidden one at 3FD00h */

typedef struct {
    u8 data[FW_SIZE];
    /* the chip's instruction decoder */
    u8 cmd, wel, asleep;        /* instruction, write enable latch, deep power-down */
    u32 addr;                   /* the address the instruction has reached */
} Firmware;

/* Fills `image` (FW_SIZE bytes) with what a never-configured console holds. */
void firmware_build(u8 *image);
void firmware_init(struct Nds *s);
/* One byte of an SPI transfer to the flash: `pos` counts bytes since chip select fell, `last`
 * is set when the transfer deselects the chip (where erases and the write enable latch end).
 * Returns the byte the chip answers. */
u8 firmware_spi_byte(struct Nds *s, int pos, u8 b, int last);
#endif
