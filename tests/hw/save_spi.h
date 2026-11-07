/* The cartridge's save chip on the AUXSPI bus, for save_9.c and save_7.c: whichever CPU
 * EXMEMCNT gives the NDS slot to. */
#ifndef SAVE_SPI_H
#define SAVE_SPI_H
#include "regs.h"

#define REG_AUXSPIDATA R16(0x040001A2)
#define REG_EXMEMCNT   R16(0x04000204)

/* One byte each way. Chip select stays low while hold is set; the last byte of a command is
 * sent with it clear, as the Nitro SDK does. */
static inline u8 spi_byte(u8 v, int hold) {
    REG_AUXSPICNT = 0xA000 | (hold ? 0x40 : 0);     /* slot on, serial mode, 4 MHz */
    REG_AUXSPIDATA = v;
    while (REG_AUXSPICNT & 0x80) ;                  /* busy */
    return (u8)REG_AUXSPIDATA;
}

/* a command with a 24-bit address; more: data bytes follow */
static inline void spi_cmd_addr(u8 cmd, u32 a, int more) {
    spi_byte(cmd, 1);
    spi_byte((u8)(a >> 16), 1);
    spi_byte((u8)(a >> 8), 1);
    spi_byte((u8)a, more);
}

static inline u32 fnv_byte(u32 h, u8 b) { return (h ^ b) * 0x01000193u; }

/* FNV-1a of n bytes read with READ (03h) or FAST READ (0Bh, after a dummy byte) */
static inline u32 spi_read_hash(u8 cmd, u32 a, int n) {
    spi_cmd_addr(cmd, a, 1);
    if (cmd == 0x0B) spi_byte(0, 1);
    u32 h = 0x811C9DC5u;
    for (int i = 0; i < n; i++) h = fnv_byte(h, spi_byte(0, i < n - 1));
    return h;
}
#endif
