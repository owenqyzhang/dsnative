#ifndef REGS_H
#define REGS_H
#include <stdint.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef int32_t s32; typedef uint64_t u64; typedef int64_t s64;
#define R16(a) (*(volatile u16 *)(a))
#define R32(a) (*(volatile u32 *)(a))
#define R8(a) (*(volatile u8 *)(a))

#define REG_DISPCNT     R32(0x04000000)
#define REG_DISPSTAT    R16(0x04000004)
#define REG_VCOUNT      R16(0x04000006)
#define REG_DISPCNT_B   R32(0x04001000)
#define REG_BG0CNT_B    R16(0x04001008)
#define REG_BG0HOFS_B   R16(0x04001010)
#define REG_BG0VOFS_B   R16(0x04001012)
#define REG_DMA3SAD     R32(0x040000D4)
#define REG_DMA3DAD     R32(0x040000D8)
#define REG_DMA3CNT     R32(0x040000DC)
#define REG_DMAFILL3    R32(0x040000EC)
#define REG_TM0CNT_L    R16(0x04000100)
#define REG_TM0CNT_H    R16(0x04000102)
#define REG_KEYINPUT    R16(0x04000130)
#define REG_IPCSYNC     R16(0x04000180)
#define REG_IPCFIFOCNT  R16(0x04000184)
#define REG_IPCFIFOSEND R32(0x04000188)
#define REG_IPCFIFORECV R32(0x04100000)
#define REG_AUXSPICNT   R16(0x040001A0)
#define REG_ROMCTRL     R32(0x040001A4)
#define REG_CARDCMD     ((volatile u8 *)0x040001A8)
#define REG_CARDDATA    R32(0x04100010)
#define REG_IME         R32(0x04000208)
#define REG_IE          R32(0x04000210)
#define REG_IF          R32(0x04000214)
#define REG_VRAMCNT_A   R8(0x04000240)
#define REG_VRAMCNT_C   R8(0x04000242)
#define REG_VRAMCNT_D   R8(0x04000243)
#define REG_DIVCNT      R16(0x04000280)
#define REG_DIV_NUMER   (*(volatile s64 *)0x04000290)
#define REG_DIV_DENOM   (*(volatile s64 *)0x04000298)
#define REG_DIV_RESULT  (*(volatile s64 *)0x040002A0)
#define REG_DIVREM      (*(volatile s64 *)0x040002A8)
#define REG_SQRTCNT     R16(0x040002B0)
#define REG_SQRT_RESULT R32(0x040002B4)
#define REG_SQRT_PARAM  (*(volatile u64 *)0x040002B8)
#define REG_POWCNT1     R16(0x04000304)

#define IRQ_VBLANK (1u << 0)
#define IRQ_TIMER0 (1u << 3)
#define IRQ_FIFO_NOT_EMPTY (1u << 18)

#define FIFO_SEND_EMPTY (1u << 0)
#define FIFO_SEND_CLEAR (1u << 3)
#define FIFO_RECV_EMPTY (1u << 8)
#define FIFO_RECV_IRQ (1u << 10)
#define FIFO_ENABLE (1u << 15)

static inline void dbg(const char *s) { while (*s) R8(0x04FFFA1C) = (u8)*s++; }
static inline void dbg_hex(u32 v) { char b[9]; for (int i = 7; i >= 0; i--) { b[i] = "0123456789abcdef"[v & 15]; v >>= 4; } b[8] = 0; dbg(b); }

static inline void intr_wait(u32 discard, u32 flags) {
    register u32 r0 __asm__("r0") = discard;
    register u32 r1 __asm__("r1") = flags;
    __asm__ volatile("swi 4" : "+r"(r0), "+r"(r1) :: "r2", "r3", "r12", "lr", "memory");
}
static inline void vblank_wait(void) {
    __asm__ volatile("swi 5" ::: "r0", "r1", "r2", "r3", "r12", "lr", "memory");
}
/* subsystem tests: commands to the ARM7 (main7.c) are 0x80000000 | id << 24 | argument */
enum { CMD7_RTC = 1, CMD7_SAVE = 2, CMD7_SOUND = 3, CMD7_GPU3D = 4 };
u32 arm7_command(u32 id, u32 arg);      /* main9.c: sends one and waits for the answer */
#endif
