/* The CPU benchmark of tools/bench.py --homebrew --workload (kernels in bench_kernels.h). It runs
 * only when START is held, so the test suite never starts it. Each kernel prints its checksum
 * ("bench_<name> <sum>", which the host build in bench_host.c must reproduce) and the VBlank
 * counts at its start and end, from which the harness times it. */
#include "regs.h"
#include "bench_kernels.h"

extern volatile u32 vblanks;

void bench_test9(void) {
    if (R16(0x04000130) & (1 << 3)) return;     /* START not held */
    k_init();
    for (int k = 0; k < K_COUNT; k++) {
        u32 v0 = vblanks;
        u32 sum = k_kernels[k].fn(k_kernels[k].rounds);
        u32 v1 = vblanks;
        dbg("bench_"); dbg(k_kernels[k].name); dbg(" "); dbg_hex(sum); dbg("\n");
        dbg("benchspan "); dbg(k_kernels[k].name); dbg(" "); dbg_hex(v0); dbg(" "); dbg_hex(v1); dbg("\n");
    }
    dbg("benchdone\n");
}
