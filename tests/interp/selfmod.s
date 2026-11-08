@ Self-modifying and never-compiled code, for tests/test_interp.py. Results go to RES.
@ Everything is ARMv5TE: the ARM9 runs it.
        .syntax unified
        .cpu arm946e-s
        .equ RES, 0x02100000
        .equ BUF_A, 0x02003000          @ where the ARM function is copied
        .equ BUF_T, 0x02003100          @ the Thumb function
        .equ BUF_F, 0x02003200          @ f of the mutual recursion
        .equ BUF_W, 0x02003300          @ code written and called again and again

        .section .text, "ax"
        .arm
        .global _start
_start:
        ldr     sp, =0x02300000
        ldr     r9, =RES

        @ 1. a compiled function (target, in the overlay segment) patched at run time and called
        @    from compiled code: as compiled, with its first word replaced (bx lr straight
        @    away), as compiled again once the bytes are restored, and with only its second
        @    halfword replaced, so the interpreter runs the patched head and falls into the
        @    compiled loop
        bl      call_target
        str     r0, [r9, #0]            @ 56
        ldr     r4, =target
        bic     r4, r4, #1
        ldr     r5, [r4]                @ the original first word: push {r4, lr}; movs r0, #1
        ldr     r6, =0x477020aa         @ movs r0, #0xaa; bx lr
        str     r6, [r4]
        bl      call_target
        str     r0, [r9, #4]            @ 0xaa
        str     r5, [r4]
        bl      call_target
        str     r0, [r9, #8]            @ 56 again, compiled
        ldr     r6, =0x2003b510         @ push {r4, lr}; movs r0, #3
        str     r6, [r4]
        bl      call_target
        str     r0, [r9, #12]           @ 58
        str     r5, [r4]

        @ 2. an ARM function copied to RAM: never compiled; calls compiled code
        ldr     r0, =BUF_A
        ldr     r1, =ramfunc_a
        ldr     r2, =ramfunc_a_end
        bl      copy
        mov     r0, #10
        ldr     r1, =helper_sum
        ldr     r3, =BUF_A
        blx     r3
        str     r0, [r9, #16]           @ 2 * (1 + ... + 10) + 1 = 111

        @ 3. the same in Thumb, entered and left through interworking
        ldr     r0, =BUF_T
        ldr     r1, =ramfunc_t
        ldr     r2, =ramfunc_t_end
        bl      copy
        mov     r0, #5
        ldr     r1, =helper_sum
        ldr     r3, =BUF_T + 1
        blx     r3
        str     r0, [r9, #20]           @ 4 * 5 * 2 + 1 + 7 = 48

        @ 4. compiled and interpreted code calling each other, four deep
        ldr     r0, =BUF_F
        ldr     r1, =ramfunc_f
        ldr     r2, =ramfunc_f_end
        bl      copy
        mov     r0, #4
        ldr     r3, =BUF_F
        blx     r3
        str     r0, [r9, #24]           @ 106

        @ 5. write then execute, twice with different code at the same address: movs r0, #n; bx lr
        ldr     r4, =BUF_W
        ldr     r6, =0x477020aa
        str     r6, [r4]
        ldr     r3, =BUF_W + 1
        blx     r3
        str     r0, [r9, #28]           @ 0xaa
        ldr     r6, =0x477020bb
        str     r6, [r4]
        ldr     r3, =BUF_W + 1
        blx     r3
        str     r0, [r9, #32]           @ 0xbb

        .global done
done:   b       done
        .ltorg

@ copy r1 .. r2 to r0
copy:   ldr     r3, [r1], #4
        str     r3, [r0], #4
        cmp     r1, r2
        blo     copy
        bx      lr

call_target:
        push    {lr}
        ldr     r3, =target
        blx     r3
        pop     {pc}
        .ltorg

@ r0 = 2 * r0 + 1
helper_sum:
        lsl     r0, r0, #1
        add     r0, r0, #1
        bx      lr

@ g(n) = n == 0 ? 200 : f(n - 1) + 2, f in RAM
helper_rec:
        push    {r4, lr}
        cmp     r0, #0
        moveq   r0, #200
        beq     1f
        sub     r0, r0, #1
        ldr     r4, =BUF_F
        blx     r4
        add     r0, r0, #2
1:      pop     {r4, pc}
        .ltorg

        .section .ovl, "ax"
        .thumb
        .thumb_func
        .global target
target:
        push    {r4, lr}
        movs    r0, #1
        movs    r4, #10
loop:   adds    r0, r0, r4
        subs    r4, r4, #1
        bne     loop
        pop     {r4, pc}

@ the code that is copied to RAM; nothing in it depends on where it runs
        .section .ramcode_a, "ax"
        .arm
ramfunc_a:                              @ r0 = n, r1 = helper: helper(1 + ... + n)
        push    {r4, r5, lr}
        mov     r4, r0
        mov     r5, #0
1:      add     r5, r5, r4
        subs    r4, r4, #1
        bne     1b
        mov     r2, r1
        mov     r0, r5
        blx     r2
        pop     {r4, r5, pc}
ramfunc_a_end:

        .section .ramcode_t, "ax"
        .thumb
ramfunc_t:                              @ r0 = n, r1 = helper (ARM): helper(4 * n) + 7
        push    {r4, lr}
        lsls    r0, r0, #2
        blx     r1
        adds    r0, #7
        pop     {r4, pc}
ramfunc_t_end:

        .section .ramcode_f, "ax"
        .arm
ramfunc_f:                              @ f(n) = n == 0 ? 100 : g(n - 1) + 1, g compiled
        push    {r4, lr}
        cmp     r0, #0
        moveq   r0, #100
        beq     1f
        sub     r0, r0, #1
        ldr     r4, =helper_rec
        blx     r4
        add     r0, r0, #1
1:      pop     {r4, pc}
        .ltorg
ramfunc_f_end:
