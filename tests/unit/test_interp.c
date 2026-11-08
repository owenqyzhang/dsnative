/* The interpreter's mechanics (tests/difftest.py checks what each instruction does, and
 * tests/test_interp.py runs a self-modifying program): the budget it spends, taking an
 * interrupt in the middle of interpreted code (the handler, the BIOS entry and return, all
 * interpreted or in C), leaving for a BIOS wait and for a halt, handing over to compiled code
 * at an address that has some, the check of an overlay's bytes, code at an address that can't
 * be fetched from, --no-interp, and the log of missing addresses (once each). */
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "dsr.h"

s32 dsr_run(Cpu *c, s32 cycles);
void dsr_reset(Cpu *c, int arch, u32 entry);

#define BASE 0x02000000u
static u8 ram[0x10000];
static Cpu cpu;
static int failures;

static void check(int cond, const char *what) {
    if (!cond) { printf("%s\n", what); failures++; }
}

/* where the interpreted instruction count stood when a guest store to MARK_ADDR ran */
#define MARK_ADDR 0x04000300u
static u64 marked_at;
static u32 slow_read(Cpu *c, u32 a, int size) { (void)c; printf("read outside RAM %08x (%d)\n", a, size); failures++; return 0; }
static void slow_write(Cpu *c, u32 a, u32 v, int size) {
    (void)v;
    if (a == 0x04000208) return;        /* IME: what the BIOS sets while it waits */
    if (a == MARK_ADDR) { marked_at = c->interp_insns; return; }
    printf("write outside RAM %08x (%d)\n", a, size); failures++;
}

static u32 w32(u32 off) { u32 v; memcpy(&v, ram + off, 4); return v; }
static void put(u32 off, const u32 *w, int n) { memcpy(ram + off, w, (size_t)n * 4); }

#define MAX_LOG 64
static u32 logged[MAX_LOG], nlogged;
static void on_miss(Cpu *c, u32 pc, u32 thumb) { (void)c; if (nlogged < MAX_LOG) logged[nlogged++] = pc | thumb; }
static int times_logged(u32 key) { int n = 0; for (u32 i = 0; i < nlogged; i++) n += logged[i] == key; return n; }
/* every logged address is `a` or `b` (an address already logged by an earlier case isn't logged again: the set is per process) */
static int only_logged(u32 a, u32 b) { for (u32 i = 0; i < nlogged; i++) if (logged[i] != a && logged[i] != b) return 0; return 1; }

static int irq_flag_at(Cpu *c) { (void)c; return w32(0xF10) != 0; }
static int woken(Cpu *c) { (void)c; return w32(0xF00) != 0; }
/* a request that arrives while interpreted code runs, after its 30th instruction: it is due at that count, whenever
 * the CPU first asks (a poll at the stop address would find it too, 370 instructions later) */
#define LATE_IRQ_DUE 30
static int late_irq(Cpu *c) {
    if (c->interp_insns >= LATE_IRQ_DUE && !w32(0xF18)) { put(0xF10, (const u32 []){1}, 1); put(0xF18, (const u32 []){1}, 1); }
    return w32(0xF10) != 0;
}

/* A fresh CPU in system mode with interrupts on, running at BASE + pc. */
static void start(int arch, u32 pc) {
    memset(ram, 0, sizeof ram);
    memset(&cpu, 0, offsetof(Cpu, rp));
    memset((u8 *)&cpu + offsetof(Cpu, sys), 0, sizeof cpu - offsetof(Cpu, sys));
    for (u32 p = 0; p < sizeof ram; p += 0x1000) {
        cpu.rp[(BASE + p) >> PAGE_SHIFT] = cpu.wp[(BASE + p) >> PAGE_SHIFT] = cpu.wp8[(BASE + p) >> PAGE_SHIFT] = ram + p;
    }
    cpu.read_slow = slow_read; cpu.write_slow = slow_write; cpu.log_miss = on_miss;
    dsr_reset(&cpu, arch, BASE + pc);
    dsr_set_cpsr(&cpu, 0x1F, 0xFF);
    cpu.irq_check_addr = BASE + 0xF00;
    cpu.exit_pc = 0xFFFFFFFFu;
    nlogged = 0;
}

/* the guest: a counting loop and a marker, at BASE */
static const u32 count_loop[] = {
    0xE3A000C8,     /* 00: mov r0, #200 */
    0xE2500001,     /* 04: subs r0, r0, #1 */
    0x1AFFFFFD,     /* 08: bne 04 */
    0xE3A04001,     /* 0c: mov r4, #1 */
    0xEAFFFFFE,     /* 10: b . */
};

/* a compiled stand-in for a function at BASE + 0x100, with the bytes it was compiled from */
static int compiled_ran;
static u32 compiled_fn(Cpu *c, u32 pc) { (void)pc; compiled_ran++; c->budget -= 2; c->r[0] = 111; return c->r[14]; }
static const u8 compiled_bytes[8] = {0x0A, 0x00, 0xA0, 0xE3, 0x1E, 0xFF, 0x2F, 0xE1};     /* mov r0, #10; bx lr */
static const DispatchEntry table[] = {
    {BASE + 0x100, 8, compiled_fn, compiled_bytes},
    {BASE + 0x200, 0, compiled_fn, 0},
};

int main(void) {
    /* the budget: one per instruction, so a slice of 100 runs 100 and the next picks up there */
    start(5, 0);
    put(0, count_loop, 5);
    cpu.exit_pc = BASE + 0x10;
    s32 ran = dsr_run(&cpu, 100);
    check(ran == 100 && cpu.interp_insns == 100, "a slice of 100 did not run 100 instructions");
    check(cpu.r[0] == 200 - 1 - 49 && cpu.r[15] == BASE + 0x08 + 0, "the loop is not where 100 instructions leave it");
    check(!cpu.stop, "the run stopped early");
    dsr_run(&cpu, 100000);
    check(cpu.stop && cpu.r[15] == BASE + 0x10 && cpu.r[0] == 0 && cpu.r[4] == 1, "the loop did not finish at the stop address");
    check(cpu.interp_insns == 1 + 200 * 2 + 1, "wrong total of interpreted instructions");
    check(times_logged(BASE) == 1 && nlogged == 1 && cpu.interp_calls == 1,
          "the first address was not logged once, or the resume inside the loop counted as an entry");

    /* an interrupt in the middle of interpreted code: the BIOS entry saves r0-r3, r12 and lr, the
     * handler (ARM code in RAM, interpreted too) clears the request and counts, and the return
     * resumes the loop with those registers back */
    static const u32 handler[] = {
        0xE3A00402,     /* mov r0, #0x02000000 */
        0xE2800C0F,     /* add r0, r0, #0xf00 */
        0xE3A01000,     /* mov r1, #0 */
        0xE5801010,     /* str r1, [r0, #0x10] */
        0xE5902014,     /* ldr r2, [r0, #0x14] */
        0xE2822001,     /* add r2, r2, #1 */
        0xE5802014,     /* str r2, [r0, #0x14] */
        0xE12FFF1E,     /* bx lr */
    };
    start(5, 0);
    put(0, count_loop, 5);
    put(0x400, handler, 8);
    u32 hp = BASE + 0x400;
    memcpy(ram + 0xF04, &hp, 4);
    cpu.irq_line = irq_flag_at;
    cpu.bank_r13[2] = BASE + 0xE00;         /* the IRQ stack */
    cpu.exit_pc = BASE + 0x10;
    cpu.r[1] = 0x11111111; cpu.r[2] = 0x22222222; cpu.r[3] = 0x33333333; cpu.r[12] = 0x1212;
    dsr_run(&cpu, 20);
    check(w32(0xF14) == 0 && cpu.r[0] == 200 - 1 - 9, "the loop did not run before the interrupt");
    u32 flag = 1;
    memcpy(ram + 0xF10, &flag, 4);
    dsr_run(&cpu, 100000);
    check(w32(0xF14) == 1, "the handler did not run exactly once");
    check(w32(0xF10) == 0, "the handler did not clear the request");
    check(cpu.stop && cpu.r[15] == BASE + 0x10 && cpu.r[0] == 0 && cpu.r[4] == 1, "the loop did not finish after the interrupt");
    check(cpu.r[1] == 0x11111111 && cpu.r[2] == 0x22222222 && cpu.r[3] == 0x33333333 && cpu.r[12] == 0x1212,
          "the interrupt return did not restore the registers");
    check(cpu.mode == MODE_SYS && !cpu.irq_off, "the interrupt return did not restore the mode");
    check(cpu.interp_calls == 2 && times_logged(BASE + 0x400) == 1 && only_logged(BASE, BASE + 0x400),
          "the loop resumed after its slice and after the interrupt counted or was logged as a new entry, or the handler was not one");

    /* the same, with the request arriving in the middle of a slice: the interpreter itself leaves for the
     * interrupt at once (the dispatcher would only see it when the loop is over, at the stop address), and
     * the loop's resume is not a new entry either. The handler's first instruction marks the count it runs at. */
    static const u32 marking_handler[] = {
        0xE5855000,     /* str r5, [r5]: r5 is MARK_ADDR */
        0xE3A00402,     /* mov r0, #0x02000000 */
        0xE2800C0F,     /* add r0, r0, #0xf00 */
        0xE3A01000,     /* mov r1, #0 */
        0xE5801010,     /* str r1, [r0, #0x10] */
        0xE5902014,     /* ldr r2, [r0, #0x14] */
        0xE2822001,     /* add r2, r2, #1 */
        0xE5802014,     /* str r2, [r0, #0x14] */
        0xE12FFF1E,     /* bx lr */
    };
    start(5, 0);
    put(0, count_loop, 5);
    put(0x400, marking_handler, 9);
    memcpy(ram + 0xF04, &hp, 4);
    cpu.irq_line = late_irq;
    cpu.bank_r13[2] = BASE + 0xE00;
    cpu.exit_pc = BASE + 0x10;
    cpu.r[5] = MARK_ADDR;
    marked_at = 0;
    dsr_run(&cpu, 100000);
    check(w32(0xF14) == 1 && cpu.stop && cpu.r[0] == 0 && cpu.r[4] == 1, "an interrupt during a slice was not taken once, or the loop did not finish");
    check(marked_at > LATE_IRQ_DUE && marked_at <= LATE_IRQ_DUE + 2,
          "the interpreter did not leave for an interrupt that arrived mid-slice: the handler ran well after the request was due");
    check(cpu.interp_calls == 2 && only_logged(BASE, BASE + 0x400),
          "the loop resumed after an interrupt in mid-slice counted or was logged as a new entry");

    /* a BIOS call that waits (SWI 4: the number is in bits 16-23) leaves the interpreter at once: IntrWait runs as BIOS code */
    start(5, 0);
    static const u32 wait[] = {0xE3A00001, 0xE3A01001, 0xEF040000, 0xE3A04001, 0xEAFFFFFE};
    put(0, wait, 5);
    cpu.irq_line = irq_flag_at;
    cpu.irq_off = 1;
    dsr_run(&cpu, 1000);
    check(cpu.halted && cpu.r[15] == cpu.bios_base + 0x300 && cpu.r[4] == 0, "IntrWait did not leave for the BIOS");
    check(cpu.interp_insns == 3, "IntrWait: wrong instruction count");
    /* the wait ends: the flow goes on after the SWI, and is not a new entry */
    cpu.irq_wake = woken;
    put(0xF00, (const u32 []){1}, 1);
    dsr_run(&cpu, 1000);
    check(cpu.r[4] == 1 && cpu.interp_calls == 1 && only_logged(BASE, BASE), "the flow after IntrWait counted or was logged as a new entry");

    /* Halt (SWI 6) and wait for interrupt (CP15) halt: the next instruction waits */
    start(5, 0);
    static const u32 halt[] = {0xEF060000, 0xE3A04001, 0xEAFFFFFE};
    put(0, halt, 3);
    dsr_run(&cpu, 1000);
    check(cpu.halted && cpu.r[15] == BASE + 4 && cpu.interp_insns == 1, "SWI Halt did not stop the interpreter");
    start(5, 0);
    static const u32 wfi[] = {0xEE070F90, 0xE3A04001, 0xEAFFFFFE};      /* mcr p15, 0, r0, c7, c0, 4 */
    put(0, wfi, 3);
    dsr_run(&cpu, 1000);
    check(cpu.halted && cpu.r[15] == BASE + 4 && cpu.interp_insns == 1, "wait for interrupt did not stop the interpreter");

    /* an undefined instruction stops, whatever its condition */
    start(5, 0);
    static const u32 undef[] = {0xE3A04001, 0x0600001F, 0xEAFFFFFE};      /* beq-never: a media instruction */
    put(0, undef, 3);
    dsr_run(&cpu, 1000);
    check(cpu.stop && cpu.r[15] == BASE + 4 && cpu.r[4] == 1, "an undefined instruction did not stop");

    /* code at an address with nothing to fetch from stops, as a missing address always did */
    start(5, 0);
    static const u32 away[] = {0xE3A00302, 0xE12FFF10};     /* mov r0, #0x08000000; bx r0 */
    put(0, away, 2);
    dsr_run(&cpu, 1000);
    check(cpu.stop && cpu.r[15] == 0x08000000u, "a jump out of memory did not stop at the target");

    /* compiled code where there is some: an overlay entry runs while its bytes are in memory, and
     * the interpreter takes the address when they change and hands it back when they are restored */
    start(5, 0);
    cpu.table = table; cpu.table_len = 2;
    /* mov r5, #0x02000000; add r5, r5, #0x100 (r5 = function); blx r5; b . */
    static const u32 call_fn[] = {0xE3A05402, 0xE2855C01, 0xE12FFF35, 0xEAFFFFFE};
    put(0, call_fn, 4);
    memcpy(ram + 0x100, compiled_bytes, 8);
    cpu.exit_pc = BASE + 0x0C;
    compiled_ran = 0;
    dsr_run(&cpu, 1000);
    check(compiled_ran == 1 && cpu.r[0] == 111 && cpu.interp_insns == 3, "the matching overlay entry did not run compiled");
    /* patched: mov r0, #0xaa; bx lr */
    static const u32 patch[] = {0xE3A000AA, 0xE12FFF1E};
    put(0x100, patch, 2);
    cpu.r[15] = BASE; cpu.stop = 0; cpu.r[0] = 0;
    u64 before = cpu.interp_insns;
    dsr_run(&cpu, 1000);
    check(compiled_ran == 1 && cpu.r[0] == 0xAA && cpu.stop, "the patched function was not interpreted");
    check(cpu.interp_insns - before == 5, "patched call: wrong number of interpreted instructions");
    check(times_logged(BASE + 0x100) == 1, "the patched function was not logged");
    /* restored */
    memcpy(ram + 0x100, compiled_bytes, 8);
    cpu.r[15] = BASE; cpu.stop = 0; cpu.r[0] = 0;
    dsr_run(&cpu, 1000);
    check(compiled_ran == 2 && cpu.r[0] == 111, "the restored function did not run compiled again");
    /* patched a second time: interpreted again, and logged no more than once */
    put(0x100, patch, 2);
    cpu.r[15] = BASE; cpu.stop = 0; cpu.r[0] = 0;
    dsr_run(&cpu, 1000);
    check(compiled_ran == 2 && cpu.r[0] == 0xAA && times_logged(BASE + 0x100) == 1, "a second patch was not interpreted, or was logged twice");

    /* falling into compiled code: the interpreter hands over at the entry without running the word there */
    start(5, 0);
    cpu.table = table; cpu.table_len = 2;
    /* mov r1, #5; nop; the next word is the entry at 0x200 (compiled_fn, nothing to check) */
    static const u32 pre[] = {0xE3A01005, 0xE1A00000};
    put(0x1F8, pre, 2);
    cpu.r[14] = BASE + 0x0C;
    put(0x0C, (const u32 []){0xEAFFFFFE}, 1);
    cpu.exit_pc = BASE + 0x0C;
    cpu.r[15] = BASE + 0x1F8;
    compiled_ran = 0;
    dsr_run(&cpu, 1000);
    check(compiled_ran == 1 && cpu.r[1] == 5 && cpu.r[0] == 111 && cpu.interp_insns == 2 && cpu.stop,
          "interpreted code did not fall into the compiled entry");

    /* --no-interp: the dispatcher stops at a missing address as it always did */
    start(5, 0);
    put(0, count_loop, 5);
    cpu.no_interp = 1;
    dsr_run(&cpu, 1000);
    check(cpu.stop && cpu.misses == 1 && cpu.last_miss == BASE && cpu.interp_insns == 0 && cpu.r[0] == 0,
          "--no-interp did not stop at the first missing address");
    check(times_logged(BASE) == 1, "the missing address was not logged");

    if (!failures) printf("interpreter mechanics ok\n");
    return failures ? 1 : 0;
}
