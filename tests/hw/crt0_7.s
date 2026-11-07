@ ARM7 start-up for the test program.
    .section .crt0, "ax"
    .arm
    .global _start
_start:
    mov     r0, #0x04000000
    str     r0, [r0, #0x208]            @ IME = 0
    msr     cpsr_c, #0xD2
    ldr     sp, =0x0380FF80
    msr     cpsr_c, #0xD3
    ldr     sp, =0x0380FFC0
    msr     cpsr_c, #0x1F                @ system mode, interrupts unmasked
    ldr     sp, =0x0380FD80
    ldr     r0, =__bss_start
    ldr     r1, =__bss_end
    mov     r2, #0
1:  cmp     r0, r1
    strlo   r2, [r0], #4
    blo     1b
    ldr     r0, =irq_handler7
    ldr     r1, =0x0380FFFC
    str     r0, [r1]
    ldr     r3, =main
    mov     lr, pc
    bx      r3
    b       .
    .pool
