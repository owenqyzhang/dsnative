/* The host reference for the benchmark kernels: prints the checksums bench_9.c must print. */
#include <stdio.h>
#include "bench_kernels.h"

int main(void) {
    k_init();
    for (int k = 0; k < K_COUNT; k++) printf("bench_%s %08x\n", k_kernels[k].name, k_kernels[k].fn(k_kernels[k].rounds));
    return 0;
}
