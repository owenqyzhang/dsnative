@ ARM9 start-up for the test program, laid out like a Nitro SDK binary: the module
@ parameters (with the "nitrocode" signature) describe an autoload segment for ITCM and
@ the BLZ-compressed end, so the recompiler's Nitro path is exercised.
    .section .crt0, "ax"
    .arm
    .global _start
_start:
    mov     r0, #0x04000000
    str     r0, [r0, #0x208]            @ IME = 0
    @ decompression would run here; the loader hands over the decompressed image
    ldr     r1, =_start_ModuleParams
    ldr     r0, [r1, #0x14]
    cmp     r0, #0
    bne     .                           @ still compressed: not expected
    @ CP15: DTCM at 0x027C0000 (16 KB), ITCM over 32 MB, both on
    ldr     r0, =0x027C000A
    mcr     p15, 0, r0, c9, c1, 0
    mov     r0, #0x20
    mcr     p15, 0, r0, c9, c1, 1
    mrc     p15, 0, r0, c1, c0, 0
    orr     r0, r0, #0x50000
    mcr     p15, 0, r0, c1, c0, 0
    @ stacks in DTCM
    msr     cpsr_c, #0xD2
    ldr     sp, =0x027C3F00
    msr     cpsr_c, #0xD3
    ldr     sp, =0x027C3FC0
    msr     cpsr_c, #0x1F                @ system mode, interrupts unmasked
    ldr     sp, =0x027C3E00
    @ autoload: copy each segment from the end of the image to its address, clear its bss
    ldr     r1, =_start_ModuleParams
    ldr     r4, [r1, #0]                @ list
    ldr     r5, [r1, #4]                @ list end
    ldr     r6, [r1, #8]                @ data
1:  cmp     r4, r5
    bhs     4f
    ldmia   r4!, {r7, r8, r9}           @ destination, size, bss size
    add     r8, r7, r8
2:  cmp     r7, r8
    ldrlo   r10, [r6], #4
    strlo   r10, [r7], #4
    blo     2b
    add     r9, r7, r9
    mov     r10, #0
3:  cmp     r7, r9
    strlo   r10, [r7], #4
    blo     3b
    b       1b
4:  @ static bss
    ldr     r0, [r1, #0x0C]
    ldr     r2, [r1, #0x10]
    mov     r3, #0
5:  cmp     r0, r2
    strlo   r3, [r0], #4
    blo     5b
    @ interrupt handler for the BIOS
    ldr     r0, =irq_handler
    ldr     r1, =0x027C3FFC
    str     r0, [r1]
    ldr     r3, =main
    blx     r3
    b       .
    .pool

    .balign 4
    .global _start_ModuleParams
_start_ModuleParams:
    .word   __autoload_list, __autoload_list_end, __autoload_start
    .word   __bss_start, __bss_end
    .word   0                           @ compressed static end (set by the ROM builder)
    .word   0x02008000                  @ SDK version
    .word   0xDEC00621, 0x2106C0DE
    .global _crt0_end
_crt0_end:
