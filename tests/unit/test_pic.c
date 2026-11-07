/* Code without an address (a stub run from the stack): the dispatcher runs the stub whose
 * bytes are in memory at an address nothing was compiled for. */
#include <stdio.h>
#include <string.h>
#include "dsr.h"

s32 dsr_run(Cpu *c, s32 cycles);
void dsr_reset(Cpu *c, int arch, u32 entry);

static u8 ram[0x4000];
static Cpu cpu;
static const u32 BASE = 0x02000000u;
static int failures;

static u32 slow_read(Cpu *c, u32 a, int size) { (void)c; (void)a; (void)size; return 0; }
static void slow_write(Cpu *c, u32 a, u32 v, int size) { (void)c; (void)a; (void)v; (void)size; }

static int ran_short, ran_long;
static u32 stub_short(Cpu *c, u32 pc) { ran_short++; c->r[1] = pc; return BASE + 0x3000; }
static u32 stub_long(Cpu *c, u32 pc) { ran_long++; c->r[1] = pc; return BASE + 0x3000; }

static const u8 b_short[] = {0xB0, 0x20, 0xC1, 0xE1, 0x10, 0xFF, 0x2F, 0xE1};            /* strh r2, [r1]; bx r0 */
static const u8 b_long[] = {0xB0, 0x20, 0xC1, 0xE1, 0x10, 0xFF, 0x2F, 0xE1, 0x1E, 0xFF, 0x2F, 0xE1};  /* ... bx lr */
/* the long one first: it is the longer, and the short one is its prefix */
static const PicEntry pics[] = {{sizeof b_long, b_long, stub_long}, {sizeof b_short, b_short, stub_short}};

/* Run from `pc` until the code at BASE + 0x3000 (where the stubs return to). */
static void run_at(u32 pc) {
    cpu.r[15] = pc;
    cpu.t = 0;
    cpu.stop = 0;
    cpu.budget = 0;
    dsr_run(&cpu, 100);
}

static void check(int cond, const char *what) {
    if (!cond) { printf("%s\n", what); failures++; }
}

int main(void) {
    for (u32 p = 0; p < sizeof ram; p += 0x1000) {
        cpu.rp[(BASE + p) >> PAGE_SHIFT] = ram + p;
        cpu.wp[(BASE + p) >> PAGE_SHIFT] = ram + p;
    }
    cpu.read_slow = slow_read;
    cpu.write_slow = slow_write;
    dsr_reset(&cpu, 4, BASE);
    cpu.exit_pc = BASE + 0x3000;
    cpu.pic = pics;
    cpu.pic_len = 2;

    memcpy(ram + 0x100, b_short, sizeof b_short);
    run_at(BASE + 0x100);
    check(ran_short == 1 && ran_long == 0 && cpu.r[1] == BASE + 0x100, "the stub at 0x100 did not run");
    check(!cpu.misses, "a match counted as a miss");

    /* the same bytes at another address (the stack is somewhere else each time) */
    memcpy(ram + 0x2FF8, b_short, sizeof b_short);
    run_at(BASE + 0x2FF8);
    check(ran_short == 2 && cpu.r[1] == BASE + 0x2FF8, "the stub at 0x2ff8 did not run");

    /* a stub that is the prefix of a longer one: the longer one wins where all of it matches */
    memcpy(ram + 0x200, b_long, sizeof b_long);
    run_at(BASE + 0x200);
    check(ran_long == 1 && ran_short == 2, "the longer stub did not win");

    /* one byte different: no stub, and the miss is reported */
    memcpy(ram + 0x300, b_short, sizeof b_short);
    ram[0x305] ^= 1;
    run_at(BASE + 0x300);
    check(ran_short == 2 && cpu.misses == 1 && cpu.last_miss == BASE + 0x300, "a stub with a changed byte ran");

    /* Thumb state, and an address no page covers: no stub, no read of the machine */
    memcpy(ram + 0x400, b_short, sizeof b_short);
    cpu.r[15] = BASE + 0x400; cpu.t = 1; cpu.stop = 0; cpu.budget = 0;
    dsr_run(&cpu, 100);
    check(ran_short == 2 && cpu.misses == 2, "a stub ran in Thumb state");
    run_at(0x08000000u);
    check(ran_short == 2 && cpu.misses == 3, "a stub ran at an address without memory");

    if (!failures) printf("position-independent stubs ok\n");
    return failures != 0;
}
