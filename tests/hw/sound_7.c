/* Sound test, ARM7 side (sound_command7). Command 0 runs it: a one-shot PCM16 sound timed
 * from its start to the end of its busy bit, then the mixer read back through the capture
 * units: a PSG square wave panned right (capture 1, right mixer) and a PCM16 loop through
 * volume, divider and panning on the left (capture 0, left mixer). Commands 1-3 return the
 * results. The samples are in main RAM with the program, the captures land in ARM7 WRAM.
 * Arguments 0x10000 + i, 0x20000 + i and 0x30000 + r0 call the BIOS's GetPitchTable(i),
 * GetVolumeTable(i) and SoundBias(r0), the last returning SOUNDBIAS. */
#include "regs.h"

#define SOUNDCNT    R16(0x04000500)
#define SOUNDBIAS   R16(0x04000504)
#define SCNT(n)     R32(0x04000400 + 16 * (n))
#define SBUSY(n)    R8(0x04000403 + 16 * (n))
#define SSAD(n)     R32(0x04000404 + 16 * (n))
#define STMR(n)     R16(0x04000408 + 16 * (n))
#define SPNT(n)     R16(0x0400040A + 16 * (n))
#define SLEN(n)     R32(0x0400040C + 16 * (n))
#define CAPCNT(x)   R8(0x04000508 + (x))
#define CAPDAD(x)   R32(0x04000510 + 8 * (x))
#define CAPLEN(x)   R16(0x04000514 + 8 * (x))

#define START   (1u << 31)
#define LOOP    (1u << 27)
#define ONESHOT (2u << 27)
#define PCM16   (1u << 29)
#define PSG     (3u << 29)
#define DIV(d)  ((u32)(d) << 8)
#define PAN(p)  ((u32)(p) << 16)
#define DUTY(d) ((u32)(d) << 24)
#define TMR(cycles) ((u16)(0x10000 - (cycles) / 2))     /* the channel timers count at bus / 2 */

static const short tone[32] = {
    0, 0x800, 0x1000, 0x1800, 0x2000, 0x2800, 0x3000, 0x3800, 0x4000, 0x3800, 0x3000, 0x2800, 0x2000, 0x1800, 0x1000, 0x800,
    0, -0x800, -0x1000, -0x1800, -0x2000, -0x2800, -0x3000, -0x3800, -0x4000, -0x3800, -0x3000, -0x2800, -0x2000, -0x1800, -0x1000, -0x800,
};
static const short level[8] = {0x4000, 0x4000, 0x4000, 0x4000, 0x4000, 0x4000, 0x4000, 0x4000};
static u16 cap0[8], cap1[16];
static u32 pcm_cycles, psg_result, cap_result;

/* ARM7 timer 0 at F/1 counts bus cycles */
static u16 now(void) { return REG_TM0CNT_L; }
static void wait(u32 cycles) { u16 t = now(); while ((u16)(now() - t) < cycles) ; }
/* waits for a busy bit, giving up after about 60000 bus cycles */
static int wait_clear(volatile u8 *reg, u8 bit) {
    u16 t = now();
    while (*reg & bit) if ((u16)(now() - t) > 60000) return 0;
    return 1;
}

static void run(void) {
    REG_TM0CNT_H = 0; REG_TM0CNT_L = 0; REG_TM0CNT_H = 0x80;
    SOUNDCNT = 0x8000 | 127;
    SOUNDBIAS = 0x200;

    /* 32 samples, 1024 bus cycles each: busy from the start to the start of the last sample */
    SSAD(0) = (u32)tone; STMR(0) = TMR(1024); SPNT(0) = 0; SLEN(0) = 16;
    u16 t0 = now();
    SCNT(0) = START | ONESHOT | PCM16 | 127 | PAN(64);
    int started = (SBUSY(0) & 0x80) != 0;
    int ended = wait_clear(&SBUSY(0), 0x80);
    pcm_cycles = started && ended ? (u16)(now() - t0) : 0;

    /* duty 1 on channel 8: 2 HIGH of every 8 samples, full right; capture 1 takes each sample */
    STMR(8) = TMR(256);
    SCNT(8) = START | PSG | DUTY(1) | 127 | PAN(127);
    wait(16 * 256);
    STMR(3) = TMR(256);
    CAPDAD(1) = (u32)cap1; CAPLEN(1) = 8;
    CAPCNT(1) = 0x80 | 4;                       /* right mixer, one-shot, PCM16 */
    if (wait_clear(&CAPCNT(1), 0x80)) {
        u32 high = 0, low = 0, repeats = 1;
        for (int i = 0; i < 16; i++) {
            high += cap1[i] == 0x7FFF;
            low += cap1[i] == 0x8001;           /* -7FFFh */
            if (i < 8 && cap1[i] != cap1[i + 8]) repeats = 0;
        }
        psg_result = high | low << 8 | repeats << 16;
    }

    /* 4000h at volume 64 (N/128), divided by 2, full left: 1000h in the left mixer */
    SSAD(2) = (u32)level; STMR(2) = TMR(256); SPNT(2) = 0; SLEN(2) = 4;
    SCNT(2) = START | LOOP | PCM16 | 64 | DIV(1) | PAN(0);
    wait(8 * 256);
    STMR(1) = TMR(256);
    CAPDAD(0) = (u32)cap0; CAPLEN(0) = 4;
    CAPCNT(0) = 0x80 | 4;                       /* left mixer, one-shot, PCM16 */
    if (wait_clear(&CAPCNT(0), 0x80)) {
        cap_result = cap0[0] | (u32)cap0[1] << 16;
        for (int i = 2; i < 8; i++) if (cap0[i] != cap0[0]) cap_result = 0xBAD0000u | i;
    }
    SCNT(2) = 0; SCNT(8) = 0;
}

static u32 swi_table(u32 num, u32 i) {
    register u32 r0 __asm__("r0") = i;
    if (num == 0x1B) __asm__ volatile("swi 0x1B" : "+r"(r0) :: "r1", "r2", "r3", "r12", "lr", "memory");
    else __asm__ volatile("swi 0x1C" : "+r"(r0) :: "r1", "r2", "r3", "r12", "lr", "memory");
    return r0;
}
static u32 swi_bias(u32 level) {
    register u32 r0 __asm__("r0") = level;
    register u32 r1 __asm__("r1") = 1;
    __asm__ volatile("swi 8" : "+r"(r0), "+r"(r1) :: "r2", "r3", "r12", "lr", "memory");
    return SOUNDBIAS;
}

u32 sound_command7(u32 arg) {
    switch (arg >> 16) {
    case 1: return swi_table(0x1B, arg & 0xFFFF);
    case 2: return swi_table(0x1C, arg & 0xFFFF);
    case 3: return swi_bias(arg & 0xFFFF);
    }
    switch (arg) {
    case 0: run(); return 0;
    case 1: return pcm_cycles;
    case 2: return psg_result;
    case 3: return cap_result;
    }
    return 0xBADC0DEu;
}
