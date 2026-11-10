/* CPU-bound kernels for tools/bench.py --homebrew --workload: built for the DS (bench_9.c, ARM and
 * Thumb, as game code is) and for the host (bench_host.c), where they must give the same checksums.
 * Each stands for something game code does: bit twiddling in loops (crc32), loads, stores and
 * branches on data (quicksort), calls and returns (fib), multiply-accumulate (matmul), byte and
 * word copies (copy), indirect jumps through a table (vm) and hardware registers, which take the
 * runtime's slow path (mmio). */
#ifndef BENCH_KERNELS_H
#define BENCH_KERNELS_H
#include <stdint.h>

#if defined(__arm__)
#define K_ARM __attribute__((target("arm"), noinline))
#define K_THUMB __attribute__((target("thumb"), noinline))
#else
#define K_ARM __attribute__((noinline))
#define K_THUMB __attribute__((noinline))
#endif

static uint32_t k_rng = 0x2545F491u;
static uint32_t k_next(void) { k_rng ^= k_rng << 13; k_rng ^= k_rng >> 17; k_rng ^= k_rng << 5; return k_rng; }

static uint8_t k_buf[8192];
static int32_t k_arr[2048];

K_ARM static uint32_t k_crc32(int rounds) {
    uint32_t sum = 0;
    for (int r = 0; r < rounds; r++) {
        uint32_t c = 0xFFFFFFFFu;
        for (int i = 0; i < 2048; i++) {
            c ^= k_buf[i] + (uint32_t)r;
            for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
        }
        sum += ~c;
    }
    return sum;
}

K_THUMB static void k_qsort(int32_t *a, int lo, int hi) {
    while (lo < hi) {
        int32_t p = a[(lo + hi) / 2];
        int i = lo, j = hi;
        while (i <= j) {
            while (a[i] < p) i++;
            while (a[j] > p) j--;
            if (i <= j) { int32_t t = a[i]; a[i] = a[j]; a[j] = t; i++; j--; }
        }
        if (j - lo < hi - i) { k_qsort(a, lo, j); lo = i; }
        else { k_qsort(a, i, hi); hi = j; }
    }
}

K_THUMB static uint32_t k_quicksort(int rounds) {
    uint32_t sum = 0;
    for (int r = 0; r < rounds; r++) {
        for (int i = 0; i < 2048; i++) k_arr[i] = (int32_t)k_next() >> 8;
        k_qsort(k_arr, 0, 2047);
        for (int i = 0; i < 2048; i += 64) sum = sum * 31 + (uint32_t)k_arr[i];
    }
    return sum;
}

K_THUMB static int k_fibn(int n) { return n < 2 ? n : k_fibn(n - 1) + k_fibn(n - 2); }
K_THUMB static uint32_t k_fib(int rounds) {
    uint32_t sum = 0;
    for (int r = 0; r < rounds; r++) sum += (uint32_t)k_fibn(22 + (r & 3));
    return sum;
}

K_ARM static uint32_t k_matmul(int rounds) {
    static int32_t a[24][24], b[24][24], c[24][24];
    uint32_t sum = 0;
    for (int r = 0; r < rounds; r++) {
        for (int i = 0; i < 24; i++) for (int j = 0; j < 24; j++) { a[i][j] = (int32_t)(k_next() & 0xFF) - 128; b[i][j] = (int32_t)(k_next() & 0xFF) - 128; }
        for (int i = 0; i < 24; i++)
            for (int j = 0; j < 24; j++) {
                int32_t s = 0;
                for (int k = 0; k < 24; k++) s += a[i][k] * b[k][j];
                c[i][j] = s;
            }
        for (int i = 0; i < 24; i++) sum = sum * 33 + (uint32_t)c[i][i];
    }
    return sum;
}

/* byte loops, word loops and a half-word loop over a few KB, as memcpy, memset and blits compile */
K_THUMB static uint32_t k_copy(int rounds) {
    static uint8_t dst[8192];
    static uint16_t half[2048];
    uint32_t sum = 0;
    for (int r = 0; r < rounds; r++) {
        for (int i = 0; i < 8192; i++) dst[i] = (uint8_t)(k_buf[i] + r);
        const uint32_t *s = (const uint32_t *)dst;
        uint32_t *d = (uint32_t *)k_buf;
        for (int i = 0; i < 2048; i++) d[i] = s[i] ^ (uint32_t)r;
        for (int i = 0; i < 2048; i++) half[i] = (uint16_t)(half[i] + (uint16_t)(d[i] >> 7));
        for (int i = 0; i < 2048; i += 16) sum = sum * 3 + half[i] + dst[i];
    }
    return sum;
}

/* a bytecode loop: a dense switch is a jump table, an indirect jump on every instruction */
K_THUMB static uint32_t k_vm(int rounds) {
    static uint8_t code[256];
    uint32_t sum = 0;
    for (int i = 0; i < 256; i++) code[i] = (uint8_t)(k_next() >> 11);
    for (int r = 0; r < rounds; r++) {
        uint32_t acc = (uint32_t)r, x = 1;
        for (int pc = 0, n = 0; n < 4096; n++) {
            switch (code[pc & 255] & 7) {
            case 0: acc += x; break;
            case 1: acc ^= x << 3; break;
            case 2: x = x * 5 + 1; break;
            case 3: acc = (acc << 1) | (acc >> 31); break;
            case 4: x ^= acc >> 5; break;
            case 5: acc -= x; pc += 3; break;
            case 6: x += (uint32_t)pc; break;
            default: acc = ~acc; break;
            }
            pc++;
        }
        sum += acc ^ x;
    }
    return sum;
}

/* registers of the machine: DIV_NUMER, DIV_DENOM and the result (the host build computes the same) */
#if defined(__arm__)
#define K_W32(a, v) (*(volatile uint32_t *)(a) = (v))
#define K_R32(a) (*(volatile uint32_t *)(a))
#else
static uint32_t k_mm_n, k_mm_d, k_mm_r;
#define K_W32(a, v) do { if ((a) == 0x04000290) k_mm_n = (v); else if ((a) == 0x04000298) { k_mm_d = (v); k_mm_r = k_mm_d ? (uint32_t)((int32_t)k_mm_n / (int32_t)k_mm_d) : 0; } } while (0)
#define K_R32(a) ((a) == 0x040002A0 ? k_mm_r : 0)
#endif
K_THUMB static uint32_t k_mmio(int rounds) {
    uint32_t sum = 0;
    for (int r = 0; r < rounds; r++) {
        K_W32(0x04000280, 0);
        for (int i = 1; i <= 256; i++) {
            K_W32(0x04000290, (uint32_t)(r * 7919 + i * 104729));
            K_W32(0x04000298, (uint32_t)(i * 3 + 1));
            sum += K_R32(0x040002A0);
        }
    }
    return sum;
}

typedef struct { const char *name; uint32_t (*fn)(int); int rounds; } KernelInfo;
static const KernelInfo k_kernels[] = {
    {"crc32", k_crc32, 500}, {"quicksort", k_quicksort, 260}, {"fib", k_fib, 60}, {"matmul", k_matmul, 400},
    {"copy", k_copy, 1300}, {"vm", k_vm, 1100}, {"mmio", k_mmio, 30000},
};
#define K_COUNT ((int)(sizeof k_kernels / sizeof k_kernels[0]))

static void k_init(void) { for (int i = 0; i < 8192; i++) k_buf[i] = (uint8_t)(k_next() >> 9); }
#endif
