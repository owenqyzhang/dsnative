/* Differential test driver: runs recompiled snippets, the interpreter and a mix of the two on
 * the same state and prints the final state of each, for tests/difftest.py to compare with
 * Unicorn's ARM946 and with each other.
 *
 * Memory: the code image at 0x02000000 is readable only (a store into it goes to the slow
 * path, which just records it), one 4 KB data page at DATA is read-write, and everything else
 * is the slow path, whose reads return a value made from the address and whose accesses are
 * folded into a hash (so a random instruction's wild accesses are compared too).
 *
 * Input (argv[1]): u32 count, u32 image_len, image bytes (loaded at 0x02000000), then per
 * case: u32 start, u32 thumb, u32 exit, u32 kind, u32 regs[16], u32 cpsr.
 * kind 0 is a snippet from the generators (compared with Unicorn too, and also run with part
 * of its compiled entries dropped); kind 1 is one random instruction (compiled and
 * interpreted only), with the banked registers and the SPSR filled in.
 * argv[2] is the run: A the recompiled code alone (no_interp), B no compiled code at all, C
 * the snippet's own entries with half of them dropped (kind 0 only). B and C stop where A
 * did: argv[3] holds A's final pc per case (u32 each), from a run of A (see below). The
 * runs are separate processes because the dispatcher's lookup cache outlives a table. C
 * prints "S index" for a kind 1 case, which it doesn't run, and B and C for a case that
 * can't be compared ("S index" too).
 * Output: per case a line "tag index regs... flags datahash pc cpsr spsr bankhash slowhash stopped";
 * stopped is 0 for a run that used up its budget (a branch to itself) rather than stopping at
 * an address. */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsr.h"

extern const DispatchEntry TABLE[];
extern const u32 TABLE_LEN;
s32 dsr_run(Cpu *c, s32 cycles);
void dsr_reset(Cpu *c, int arch, u32 entry);

#define BASE 0x02000000u
#define SIZE 0x400000u
#define DATA 0x02200000u
#define DATA_LEN 0x1000u
#define SLOT 0x100u

static u8 *ram;
static u32 slow_hash;

static u32 mix(u32 h, u32 v) { v *= 0x9E3779B1u; v ^= v >> 15; return (h ^ v) * 16777619u; }

static u32 rs(Cpu *c, u32 a, int size) {
    (void)c;
    slow_hash = mix(mix(slow_hash, a), (u32)size);
    u32 v = mix(a, 0x5BD1E995u);
    return size == 1 ? v & 0xFF : size == 2 ? v & 0xFFFF : v;
}
static void ws(Cpu *c, u32 a, u32 v, int size) {
    (void)c;
    slow_hash = mix(mix(mix(slow_hash, a), v), (u32)size);
}

static u32 rdw(FILE *f) { u32 v; if (fread(&v, 4, 1, f) != 1) { fprintf(stderr, "short input\n"); exit(2); } return v; }

static Cpu c;

typedef struct { u32 start, thumb, exit, kind, regs[16], cpsr; } Case;

static void setup(Case *k, u32 idx, const u8 *data0, const DispatchEntry *tab, u32 len, u32 exit_pc, int no_interp) {
    /* the page tables stay: only the state around them is cleared */
    memset(&c, 0, offsetof(Cpu, rp));
    memset((u8 *)&c + offsetof(Cpu, sys), 0, sizeof c - offsetof(Cpu, sys));
    dsr_reset(&c, ARCH, k->start | k->thumb);
    c.read_slow = rs; c.write_slow = ws;
    c.table = tab; c.table_len = len;
    c.no_interp = (u32)no_interp;
    memcpy(ram + (DATA - BASE), data0, DATA_LEN);
    slow_hash = 0;
    if (k->kind) {      /* banked registers a mode switch will swap in, and a saved status */
        u32 h = idx * 7919u + 1;
        for (int i = 0; i < 8; i++) { c.bank_r13[i] = h = mix(h, 1); c.bank_r14[i] = h = mix(h, 2); c.bank_spsr[i] = h = mix(h, 3); }
        for (int i = 0; i < 5; i++) { c.usr_r8_12[i] = h = mix(h, 4); c.fiq_r8_12[i] = h = mix(h, 5); }
        c.spsr = mix(h, 6);
    }
    dsr_set_cpsr(&c, k->cpsr, 0xFFFFFFFFu);
    memcpy(c.r, k->regs, 60);
    c.r[15] = k->start;
    c.t = k->thumb;
    c.exit_pc = exit_pc;
}

static void print_state(char tag, u32 idx) {
    u32 hsh = 2166136261u, bank = 2166136261u;
    for (u32 i = 0; i < DATA_LEN; i++) hsh = (hsh ^ ram[(DATA - BASE) + i]) * 16777619u;
    const u32 *banks[5] = {c.bank_r13, c.bank_r14, c.bank_spsr, c.usr_r8_12, c.fiq_r8_12};
    const int n[5] = {8, 8, 8, 5, 5};
    for (int b = 0; b < 5; b++) for (int i = 0; i < n[b]; i++) bank = (bank ^ banks[b][i]) * 16777619u;
    printf("%c %u ", tag, idx);
    for (int i = 0; i < 15; i++) printf("%08x ", c.r[i]);
    printf("%08x %08x %08x %08x %08x %08x %08x %x\n", dsr_cpsr(&c) & 0xF8000000u, hsh, c.r[15], dsr_cpsr(&c), c.spsr, bank,
           slow_hash, c.stop);
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    char run = argv[2][0];
    FILE *ends = argc > 3 ? fopen(argv[3], "rb") : 0;
    if (run != 'A' && !ends) return 2;
    u32 count = rdw(f), len = rdw(f);
    ram = calloc(1, SIZE);
    if (fread(ram, 1, len, f) != len) return 2;
    u8 *data0 = malloc(DATA_LEN);
    memcpy(data0, ram + (DATA - BASE), DATA_LEN);
    for (u32 p = 0; p < (DATA + DATA_LEN - BASE) >> PAGE_SHIFT; p++) c.rp[(BASE >> PAGE_SHIFT) + p] = ram + (p << PAGE_SHIFT);
    c.wp[DATA >> PAGE_SHIFT] = c.wp8[DATA >> PAGE_SHIFT] = ram + (DATA - BASE);
    static DispatchEntry mixed[SLOT + 1];
    for (u32 i = 0; i < count; i++) {
        Case k;
        k.start = rdw(f); k.thumb = rdw(f); k.exit = rdw(f); k.kind = rdw(f);
        for (int j = 0; j < 16; j++) k.regs[j] = rdw(f);
        k.cpsr = rdw(f);
        u32 end = run == 'A' ? 0 : rdw(ends);
        if (run != 'A' && end == 0xFFFFFFFEu) { printf("S %u\n", i); continue; }      /* see difftest.py */
        /* the others stop where A did, unless A ran code there: a loop or its own start (the file
         * says so: 0xFFFFFFFF) or the BIOS (a wait for an interrupt) */
        if (ARCH == 5 ? end >= 0xFFFF0000u : end < 0x4000u) end = 0xFFFFFFFFu;
        if (run == 'A') {
            setup(&k, i, data0, TABLE, TABLE_LEN, k.exit, 1);
        } else if (run == 'B') {
            setup(&k, i, data0, TABLE, 0, end, 0);
        } else {
            if (k.kind) { printf("S %u\n", i); continue; }
            u32 lo = 0, hi = TABLE_LEN, nm = 0;
            while (lo < hi) { u32 mid = (lo + hi) / 2; if (TABLE[mid].key < k.start) lo = mid + 1; else hi = mid; }
            for (; lo < TABLE_LEN && TABLE[lo].key < k.start + SLOT; lo++)
                if (mix(i, TABLE[lo].key) & 0x100) mixed[nm++] = TABLE[lo];
            setup(&k, i, data0, mixed, nm, end, 0);
        }
        dsr_run(&c, 100000);
        print_state(run, i);
    }
    return 0;
}
