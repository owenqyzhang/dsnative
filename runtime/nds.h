/* dsnative runtime: the DS machine (memory map, I/O, interrupts, timers, DMA, IPC, card). */
#ifndef NDS_H
#define NDS_H
#include "dsr.h"
#include "rtc.h"
#include "save.h"
#include "sound.h"
#include "gpu3d.h"

#define SCREEN_W 256
#define SCREEN_H 192

enum {
    IRQ_VBLANK = 1 << 0, IRQ_HBLANK = 1 << 1, IRQ_VCOUNT = 1 << 2, IRQ_TIMER0 = 1 << 3,
    IRQ_DMA0 = 1 << 8, IRQ_KEYPAD = 1 << 12, IRQ_IPCSYNC = 1 << 16, IRQ_FIFO_EMPTY = 1 << 17,
    IRQ_FIFO_NOT_EMPTY = 1 << 18, IRQ_CARD_DONE = 1 << 19, IRQ_GXFIFO = 1 << 21, IRQ_SPI = 1 << 23,
};

/* buttons, as KEYINPUT bits (0-9) plus X and Y (10, 11) */
enum { KEY_A = 1 << 0, KEY_B = 1 << 1, KEY_SELECT = 1 << 2, KEY_START = 1 << 3, KEY_RIGHT = 1 << 4,
       KEY_LEFT = 1 << 5, KEY_UP = 1 << 6, KEY_DOWN = 1 << 7, KEY_R = 1 << 8, KEY_L = 1 << 9,
       KEY_X = 1 << 10, KEY_Y = 1 << 11 };

typedef struct {
    u32 src, dst, cnt;          /* as written */
    u32 isrc, idst, icount;     /* internal, latched at start */
} Dma;

typedef struct {
    u16 reload, ctrl, counter;
    u32 frac;                   /* bus cycles towards the next tick */
} Timer;

typedef struct {
    u32 data[16];
    int head, count;
} Fifo;

typedef struct Nds {
    Cpu arm9, arm7;
    u8 *ram;                    /* 4 MB main RAM */
    u8 itcm[0x8000], dtcm[0x4000], swram[0x8000], wram7[0x10000];
    u8 *vram;                   /* banks A-I, 656 KB */
    u8 pal[0x800], oam[0x800];
    u8 io[2][0x2000];           /* raw I/O storage per CPU (0x04000000-0x04001FFF) */
    u8 vramcnt[9], wramcnt;
    u32 vcount, frame;
    u16 dispstat[2];
    u32 ime[2], ie[2], ifl[2];
    Dma dma[2][4];
    u32 dmafill[4];
    Timer tim[2][4];
    Fifo fifo[2];               /* fifo[0]: ARM9 to ARM7, fifo[1]: ARM7 to ARM9 */
    u16 ipcsync[2], fifocnt[2];
    u32 fifo_last[2];
    u16 keys;                   /* pressed buttons (KEY_*) */
    int touch_down; int touch_x, touch_y;
    u16 powcnt1, exmemcnt;
    /* divider and square root */
    u16 divcnt, sqrtcnt;
    s64 div_numer, div_denom, div_result, div_rem;
    u64 sqrt_param; u32 sqrt_result;
    /* card */
    const u8 *rom; u32 rom_size;
    u8 card_cmd[8]; u32 romctrl, card_addr, card_left; u16 auxspicnt;
    u32 chip_id;
    /* SPI (ARM7): power manager, firmware, touch screen */
    u16 spicnt; u8 spi_data; int spi_pos; u8 spi_cmd; u32 spi_addr;
    u8 pm_regs[8]; u8 pm_index;
    u8 *firmware; u16 touch_adc;
    /* screens */
    u32 fb[2][SCREEN_W * SCREEN_H];     /* [0] top, [1] bottom, 0xAARRGGBB */
    /* debug output (no$gba port) */
    char dbg[1024]; int dbg_len;
    void (*debug_line)(const char *s);
    u64 time;                   /* bus (ARM7) cycles since start */
    /* subsystems in files of their own (rtc.c, save.c, sound.c, gpu3d.c) */
    Rtc rtc;
    Save save;
    Sound sound;
    Gpu3d g3d;
} Nds;

Nds *nds_create(const u8 *rom, u32 rom_size);
void nds_direct_boot(Nds *s, const u8 *arm9, u32 arm9_len);
void nds_run_frame(Nds *s);
void nds_remap(Nds *s);
void nds_request_irq(Nds *s, int cpu, u32 bits);
/* A subsystem option ("--save", "game.sav"): returns 0 when no subsystem knows the name. */
int nds_option(Nds *s, const char *name, const char *value);
/* Lets the subsystems write what they keep (the save file, the sound recording). */
void nds_shutdown(Nds *s);
/* Takes up to max_frames stereo frames (left, right) of mixed sound produced since the last
 * call, at 32768 Hz; returns how many it wrote. */
u32 nds_audio_take(Nds *s, s16 *out, u32 max_frames);

/* gpu2d.c */
void gpu2d_render_line(Nds *s, int engine, int line, u32 *out);
u8 *vram_engine_ptr(Nds *s, int engine, int obj, u32 offset);    /* engine-relative VRAM */
#endif
