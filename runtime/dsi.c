/* dsnative runtime: the DSi registers in DS mode. References: GBATEK "DSi I/O Map", "DSi Control
 * Registers (SCFG)" (the values SCFG_EXT holds for a DS cartridge: bit 31 clear locks the SCFG and
 * MBK range, and the bits for the new DMA, camera, DSP, AES, SD/MMC, SDIO Wi-Fi, microphone and
 * I2C blocks are clear), "DSi GPIO Registers", "DSi Microphone and SoundExt" (SNDEXCNT). */
#include "nds.h"

void dsi_init(Nds *s) {
    Dsi *d = &s->dsi;
    memset(d, 0, sizeof *d);
    d->gpio_wifi = 0x0100;      /* the old NDS Wi-Fi mode (bit 8): the hardware at 0x04800000 is the one in use */
}

/* GPIO data in: the pins not driven by the ARM7 read their level, the driven ones the output
 * ANDed with it. Bits 0-2 and 4 are pulled high, 5 (headphones) is unplugged, 6 (power button)
 * idle and 7 (sound enable) high. */
static u8 gpio_in(const Dsi *d) {
    const u8 pins = 0xD7;
    return (u8)(pins & (d->gpio_out | ~d->gpio_dir));
}

/* the 16-bit registers by byte, so 8, 16 and 32-bit accesses all go the same way */
static u8 reg_read8(Nds *s, u32 a) {
    Dsi *d = &s->dsi;
    switch (a) {
    case 0x4700: return (u8)d->sndexcnt;
    case 0x4701: return (u8)(d->sndexcnt >> 8);
    case 0x4C00: return gpio_in(d);
    case 0x4C01: return d->gpio_dir;
    case 0x4C02: return d->gpio_edge;
    case 0x4C03: return d->gpio_ien;
    case 0x4C04: return (u8)d->gpio_wifi;
    case 0x4C05: return (u8)(d->gpio_wifi >> 8);
    }
    return 0;
}

static void reg_write8(Nds *s, u32 a, u8 v) {
    Dsi *d = &s->dsi;
    switch (a) {
    case 0x4700: d->sndexcnt = (u16)((d->sndexcnt & 0xFF00) | (v & 0x0F)); break;     /* NITRO/DSP ratio */
    case 0x4701: {
        /* mute (bit 14) and enable (bit 15) are plain; the I2S frequency (bit 13) takes a write only
         * while the enable bit was 0 before it */
        u16 keep13 = (d->sndexcnt & 0x8000) ? (d->sndexcnt & 0x2000) : (u16)((v & 0x20) << 8);
        d->sndexcnt = (u16)((d->sndexcnt & 0x00FF) | keep13 | ((v & 0xC0) << 8));
        break;
    }
    case 0x4C00: d->gpio_out = v & 0xF7; break;
    case 0x4C01: d->gpio_dir = v & 0xF7; break;
    case 0x4C02: d->gpio_edge = v & 0xF7; break;
    case 0x4C03: d->gpio_ien = v & 0xF7; break;
    case 0x4C04: d->gpio_wifi = (u16)((d->gpio_wifi & 0xFF00) | (v & 0x01)); break;
    case 0x4C05: d->gpio_wifi = (u16)((d->gpio_wifi & 0x00FF) | ((v & 0x01) << 8)); break;
    }
}

/* the block at 0x04004000 on both CPUs, IE2/IF2 (0x04000218/21C) and the second cartridge
 * slot's ports (0x040021A0-0x040021BF) */
static int claimed(int cpu, u32 off) {
    if (off >= 0x4000 && off < 0x5000) return 1;
    if (off >= 0x21A0 && off < 0x21C0) return 1;
    (void)cpu;
    return off >= 0x218 && off < 0x220;
}

int dsi_io_read(Nds *s, int cpu, u32 off, int size, u32 *v) {
    if (!claimed(cpu, off)) return 0;
    u32 r = 0;
    if (cpu == 1 && off >= 0x4000)
        for (int i = 0; i < size; i++) r |= (u32)reg_read8(s, off + (u32)i) << (8 * i);
    *v = r;
    return 1;
}

int dsi_io_write(Nds *s, int cpu, u32 off, int size, u32 v) {
    if (!claimed(cpu, off)) return 0;
    if (cpu == 1 && off >= 0x4000)
        for (int i = 0; i < size; i++) reg_write8(s, off + (u32)i, (u8)(v >> (8 * i)));
    return 1;
}
