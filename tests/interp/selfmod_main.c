/* Runs the self-modifying test program (selfmod.s) on a flat 4 MB RAM at 0x02000000 with the
 * recompiled code of its static part, until it reaches `done`; prints the results it stored,
 * the interpreter's counters and the addresses the runtime logged as missing.
 *
 *   selfmod <image.bin> <done address> <results address> [--no-interp]
 */
#include <stdio.h>
#include <stdlib.h>
#include "dsr.h"

extern const DispatchEntry g9_table[];
extern const u32 g9_table_len;
s32 dsr_run(Cpu *c, s32 cycles);
void dsr_reset(Cpu *c, int arch, u32 entry);

#define BASE 0x02000000u
#define SIZE 0x400000u

static u32 rs(Cpu *c, u32 a, int size) { (void)c; fprintf(stderr, "read outside RAM %08x (%d)\n", a, size); exit(2); }
static void ws(Cpu *c, u32 a, u32 v, int size) { (void)c; (void)v; fprintf(stderr, "write outside RAM %08x (%d)\n", a, size); exit(2); }
static void log_miss(Cpu *c, u32 pc, u32 thumb) { (void)c; printf("missing %08x %s\n", pc, thumb ? "t" : "a"); }

static Cpu c;

int main(int argc, char **argv) {
    if (argc < 4) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    u8 *ram = calloc(1, SIZE);
    size_t n = fread(ram, 1, SIZE, f);
    fclose(f);
    (void)n;
    u32 done = (u32)strtoul(argv[2], 0, 16), res = (u32)strtoul(argv[3], 0, 16);
    dsr_reset(&c, 5, BASE);
    for (u32 p = 0; p < SIZE >> PAGE_SHIFT; p++)
        c.rp[(BASE >> PAGE_SHIFT) + p] = c.wp[(BASE >> PAGE_SHIFT) + p] = c.wp8[(BASE >> PAGE_SHIFT) + p] = ram + (p << PAGE_SHIFT);
    c.read_slow = rs; c.write_slow = ws; c.log_miss = log_miss;
    c.table = g9_table; c.table_len = g9_table_len;
    c.exit_pc = done;
    c.no_interp = argc > 4 && !strcmp(argv[4], "--no-interp");
    for (int i = 0; i < 100 && !c.stop; i++) dsr_run(&c, 100000);
    printf("results");
    for (int i = 0; i < 9; i++) { u32 v; memcpy(&v, ram + (res - BASE) + 4 * i, 4); printf(" %08x", v); }
    printf("\ncounters %llu %llu %llu stop %u pc %08x\n", (unsigned long long)c.interp_calls, (unsigned long long)c.interp_insns,
           (unsigned long long)c.dispatches, c.stop, c.r[15]);
    return 0;
}
