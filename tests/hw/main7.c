/* The ARM7 side of the hardware test: answers each FIFO word v with v * 3 + 1, sleeping in
 * the BIOS IntrWait between messages. */
#include "regs.h"

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
            REG_IPCFIFOSEND = v * 3 + 1;
        }
        /* the interrupt comes when the FIFO stops being empty, so drain it first and keep
         * flags that arrived meanwhile */
        intr_wait(0, IRQ_FIFO_NOT_EMPTY);
    }
}
