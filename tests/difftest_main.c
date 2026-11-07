/* Differential test driver: runs recompiled snippets on a flat 4 MB RAM at 0x02000000 and
 * prints the final state of each, for tests/difftest.py to compare with Unicorn's ARM946.
 *
 * Input (argv[1]): u32 count, u32 image_len, image bytes (loaded at 0x02000000), then per
 * case: u32 start, u32 thumb, u32 exit, u32 regs[16], u32 cpsr.
 * Output: per case a line "regs... cpsr datahash". */
#include <stdio.h>
#include <stdlib.h>
#include "dsr.h"

extern const DispatchEntry g9_table[];
extern const u32 g9_table_len;
s32 dsr_run(Cpu *c, s32 cycles);
void dsr_reset(Cpu *c, int arch, u32 entry);

#define BASE 0x02000000u
#define SIZE 0x400000u
#define DATA 0x02200000u
#define DATA_LEN 0x1000u

static u8 *ram;

static u32 rs(Cpu *c, u32 a, int size) { (void)c; fprintf(stderr, "read outside RAM %08x (%d)\n", a, size); exit(2); }
static void ws(Cpu *c, u32 a, u32 v, int size) { (void)c; (void)v; fprintf(stderr, "write outside RAM %08x (%d)\n", a, size); exit(2); }

static u32 rdw(FILE *f) { u32 v; if (fread(&v, 4, 1, f) != 1) { fprintf(stderr, "short input\n"); exit(2); } return v; }

int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    u32 count = rdw(f), len = rdw(f);
    ram = calloc(1, SIZE);
    if (fread(ram, 1, len, f) != len) return 2;
    u8 *data0 = malloc(DATA_LEN);
    memcpy(data0, ram + (DATA - BASE), DATA_LEN);
    static Cpu c;
    for (u32 i = 0; i < count; i++) {
        u32 start = rdw(f), thumb = rdw(f), ex = rdw(f);
        u32 regs[16];
        for (int k = 0; k < 16; k++) regs[k] = rdw(f);
        u32 cpsr = rdw(f);
        memset(&c, 0, sizeof c);
        dsr_reset(&c, 5, start | thumb);
        for (u32 p = 0; p < SIZE >> PAGE_SHIFT; p++) {
            c.rp[(BASE >> PAGE_SHIFT) + p] = c.wp[(BASE >> PAGE_SHIFT) + p] = c.wp8[(BASE >> PAGE_SHIFT) + p] = ram + (p << PAGE_SHIFT);
        }
        c.read_slow = rs; c.write_slow = ws;
        c.table = g9_table; c.table_len = g9_table_len;
        memcpy(ram + (DATA - BASE), data0, DATA_LEN);
        dsr_set_cpsr(&c, cpsr, 0xFFFFFFFFu);
        memcpy(c.r, regs, 60);
        c.r[15] = start;
        c.t = thumb;
        c.exit_pc = ex;
        dsr_run(&c, 100000);
        u32 hsh = 2166136261u;
        for (u32 k = 0; k < DATA_LEN; k++) hsh = (hsh ^ ram[(DATA - BASE) + k]) * 16777619u;
        for (int k = 0; k < 15; k++) printf("%08x ", c.r[k]);
        printf("%08x %08x\n", dsr_cpsr(&c) & 0xF8000000u, hsh);
    }
    return 0;
}
