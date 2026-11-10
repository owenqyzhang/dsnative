@ ARM9 start-up for the test program, laid out like a Nitro SDK binary: the module
@ parameters (with the "nitrocode" signature) describe an autoload segment for ITCM and
@ the BLZ-compressed end, so the recompiler's Nitro path is exercised.
    .section .crt0, "ax"
    .arm
    .global _start
_start:
    mov     r0, #0x04000000
    str     r0, [r0, #0x208]            @ IME = 0
    @ decompress the static module in place, as the SDK's crt0 does; the field stays set, so a
    @ restart that reloads the compressed body runs this again
    ldr     r1, =_start_ModuleParams
    ldr     r0, [r1, #0x14]
    bl      blz_uncompress
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
@ BLZ (backward LZ) decompression in place. r0 = end of the compressed data (0: nothing to do).
@ The last 8 bytes: the footer's length (top byte) and the compressed part's length (low 24 bits),
@ then how much the data grows. The stream is read backwards from the footer and written
@ backwards from the end of the output; a flag byte covers 8 items, high bit first: 1 = a
@ halfword (length - 3 in the top 4 bits, distance - 3 in the low 12) copying forward from
@ already written output, 0 = one literal byte.
    .arm
blz_uncompress:
    cmp     r0, #0
    bxeq    lr
    ldr     r1, [r0, #-4]               @ growth
    cmp     r1, #0
    bxeq    lr
    ldr     r2, [r0, #-8]
    mov     r3, r2, lsr #24             @ footer length
    bic     r2, r2, #0xFF000000         @ compressed length
    sub     r3, r0, r3                  @ src: end of the stream
    sub     r2, r0, r2                  @ stop: start of the compressed part
    add     r1, r0, r1                  @ dst: end of the output
.Lblz_flags:
    cmp     r3, r2
    bxls    lr
    ldrb    r12, [r3, #-1]!
    mov     r4, #8
.Lblz_item:
    cmp     r1, r2
    bxls    lr
    cmp     r3, r2
    bxls    lr
    tst     r12, #0x80
    beq     .Lblz_literal
    ldrb    r5, [r3, #-1]!              @ two byte loads: the stream is not halfword aligned
    ldrb    r6, [r3, #-1]!
    orr     r6, r6, r5, lsl #8
    mov     r5, r6, lsr #12
    add     r5, r5, #3                  @ length
    bic     r6, r6, #0xF000
    add     r6, r6, #3                  @ distance
.Lblz_copy:
    cmp     r1, r2
    bls     .Lblz_next
    sub     r1, r1, #1
    ldrb    r7, [r1, r6]
    strb    r7, [r1]
    subs    r5, r5, #1
    bne     .Lblz_copy
    b       .Lblz_next
.Lblz_literal:
    ldrb    r7, [r3, #-1]!
    strb    r7, [r1, #-1]!
.Lblz_next:
    mov     r12, r12, lsl #1
    subs    r4, r4, #1
    bne     .Lblz_item
    b       .Lblz_flags
    .ltorg

    .global _start_ModuleParams
_start_ModuleParams:
    .word   __autoload_list, __autoload_list_end, __autoload_start
    .word   __bss_start, __bss_end
    .word   0                           @ compressed static end (set by the ROM builder)
    .word   0x02008000                  @ SDK version
    .word   0xDEC00621, 0x2106C0DE
    .global _crt0_end
_crt0_end:
