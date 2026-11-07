/* The ARM7 side of the hardware test: answers each FIFO word v with v * 3 + 1, sleeping in
 * the BIOS IntrWait between messages. A word with the top bit set is a command for a
 * subsystem test (in *_7.c): bits 24-30 pick the subsystem, bits 0-23 are its argument. */
#include "regs.h"

/* defined by the subsystem tests that need the ARM7 */
u32 rtc_command7(u32 arg) __attribute__((weak));
u32 save_command7(u32 arg) __attribute__((weak));
u32 sound_command7(u32 arg) __attribute__((weak));
u32 gpu3d_command7(u32 arg) __attribute__((weak));

static u32 command(u32 v) {
    u32 (*fn)(u32) = 0;
    switch ((v >> 24) & 0x7F) {
    case CMD7_RTC: fn = rtc_command7; break;
    case CMD7_SAVE: fn = save_command7; break;
    case CMD7_SOUND: fn = sound_command7; break;
    case CMD7_GPU3D: fn = gpu3d_command7; break;
    }
    return fn ? fn(v & 0xFFFFFF) : 0xBADC0DEu;
}

__attribute__((target("arm"))) void irq_handler7(void) {
    u32 f = REG_IF & REG_IE;
    REG_IF = f;
    R32(0x0380FFF8) |= f;
}

int main(void) {
    REG_IPCFIFOCNT = FIFO_ENABLE | FIFO_SEND_CLEAR | FIFO_RECV_IRQ;
    REG_IE = IRQ_FIFO_NOT_EMPTY;
    REG_IF = ~0u;
    REG_IME = 1;
    REG_IPCSYNC = 0xA << 8;
    for (;;) {
        while (!(REG_IPCFIFOCNT & FIFO_RECV_EMPTY)) {
            u32 v = REG_IPCFIFORECV;
            REG_IPCFIFOSEND = (v & 0x80000000u) ? command(v) : v * 3 + 1;
        }
        /* the interrupt comes when the FIFO stops being empty, so drain it first and keep
         * flags that arrived meanwhile */
        intr_wait(0, IRQ_FIFO_NOT_EMPTY);
    }
}
