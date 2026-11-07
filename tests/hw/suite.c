/* A portable workload for checking the recompiler: built for the DS (ARM and Thumb, with
 * libgcc's division and soft-float routines) and for the host, it prints the same lines on
 * both when the recompiled code is right. */
#include <stdint.h>
#include "suite.h"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;
typedef int64_t s64;

#if defined(__arm__)
#define ARM_CODE __attribute__((target("arm"), noinline))
#define THUMB_CODE __attribute__((target("thumb"), noinline))
#else
#define ARM_CODE __attribute__((noinline))
#define THUMB_CODE __attribute__((noinline))
#endif

static void out_hex(u32 v) {
    char b[9];
    for (int i = 7; i >= 0; i--) { b[i] = "0123456789abcdef"[v & 15]; v >>= 4; }
    b[8] = 0;
    out_str(b);
}

static void line(const char *name, u32 v) { out_str(name); out_str(" "); out_hex(v); out_str("\n"); }

static u32 rng = 0x12345678;
static u32 xorshift(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static u8 buf[4096];
static s32 arr[512];

ARM_CODE static u32 crc32(const u8 *p, int n) {
    u32 c = 0xFFFFFFFFu;
    for (int i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

THUMB_CODE static void quicksort(s32 *a, int lo, int hi) {
    while (lo < hi) {
        s32 p = a[(lo + hi) / 2];
        int i = lo, j = hi;
        while (i <= j) {
            while (a[i] < p) i++;
            while (a[j] > p) j--;
            if (i <= j) { s32 t = a[i]; a[i] = a[j]; a[j] = t; i++; j--; }
        }
        if (j - lo < hi - i) { quicksort(a, lo, j); lo = i; }
        else { quicksort(a, i, hi); hi = j; }
    }
}

THUMB_CODE static int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

static int op_add(int a, int b) { return a + b; }
static int op_sub(int a, int b) { return a - b; }
ARM_CODE static int op_mul(int a, int b) { return a * b; }
THUMB_CODE static int op_xor(int a, int b) { return a ^ b; }
static int (*const ops[4])(int, int) = {op_add, op_sub, op_mul, op_xor};

ARM_CODE static u32 switchy(u32 x) {
    switch (x % 11) {
    case 0: return x * 3;
    case 1: return x + 17;
    case 2: return x ^ 0x5555;
    case 3: return x >> 2;
    case 4: return ~x;
    case 5: return x * x;
    case 6: return x - 1000;
    case 7: return x << 3;
    case 8: return x | 0x80000000u;
    case 9: return x & 0xFFFF;
    default: return 42;
    }
}

struct big { u32 v[13]; };
THUMB_CODE static struct big mix(struct big a, struct big b) {
    struct big r;
    for (int i = 0; i < 13; i++) r.v[i] = a.v[i] * 31 + b.v[12 - i];
    return r;
}

ARM_CODE static u32 mandel(void) {
    u32 sum = 0;
    for (int y = 0; y < 24; y++)
        for (int x = 0; x < 32; x++) {
            s32 cr = (x - 22) * 0x2000 / 8, ci = (y - 12) * 0x2000 / 8;
            s32 zr = 0, zi = 0;
            int i = 0;
            while (i < 32) {
                s64 zr2 = (s64)zr * zr >> 13, zi2 = (s64)zi * zi >> 13;
                if (zr2 + zi2 > 4 * 0x2000) break;
                s32 nz = (s32)(zr2 - zi2) + cr;
                zi = (s32)(((s64)2 * zr * zi) >> 13) + ci;
                zr = nz;
                i++;
            }
            sum = sum * 33 + (u32)i;
        }
    return sum;
}

THUMB_CODE static double newton_sqrt(double v) {
    double x = v > 1 ? v / 2 : 1;
    for (int i = 0; i < 30; i++) x = 0.5 * (x + v / x);
    return x;
}

static u32 dbits_lo(double d) { union { double d; u64 u; } x; x.d = d; return (u32)x.u; }
static u32 dbits_hi(double d) { union { double d; u64 u; } x; x.d = d; return (u32)(x.u >> 32); }
static u32 fbits(float f) { union { float f; u32 u; } x; x.f = f; return x.u; }

void run_suite(void) {
    for (int i = 0; i < 4096; i++) buf[i] = (u8)xorshift();
    line("crc32", crc32(buf, 4096));

    for (int i = 0; i < 512; i++) arr[i] = (s32)xorshift() >> 3;
    quicksort(arr, 0, 511);
    u32 h = 0; int sorted = 1;
    for (int i = 0; i < 512; i++) { h = h * 131 + (u32)arr[i]; if (i && arr[i - 1] > arr[i]) sorted = 0; }
    line("sort", h ^ (u32)sorted);

    line("fib", (u32)fib(20));

    u32 acc = 0;
    for (int i = 0; i < 200; i++) {
        int x = (int)xorshift(), y = (int)(xorshift() & 0xFFFF);   /* sequenced: argument order is unspecified */
        acc += (u32)ops[i & 3](x, y);
    }
    line("fnptr", acc);

    acc = 0;
    for (u32 i = 0; i < 500; i++) acc = acc * 7 + switchy(xorshift());
    line("switch", acc);

    u32 q = 0;
    for (int i = 0; i < 300; i++) {
        s32 a = (s32)xorshift();
        u32 t1 = xorshift(), t2 = xorshift() & 31;
        s32 b = (s32)(t1 >> t2) | 1;
        u32 ua = xorshift();
        u32 t3 = xorshift(), t4 = xorshift() & 31;
        u32 ub = (t3 >> t4) | 1;
        q = q * 3 + (u32)(a / b) + (u32)(a % b) + ua / ub + ua % ub;
    }
    line("div32", q);

    u64 q64 = 0;
    for (int i = 0; i < 200; i++) {
        u64 hi = xorshift(), lo = xorshift(), a = (hi << 32) | lo;
        u64 hi2 = xorshift(), lo2 = xorshift(), sh = xorshift() & 63;
        u64 b = ((hi2 << 32) | lo2) >> sh;
        s64 sa = (s64)a, sb = (s64)(b | 1);
        q64 = q64 * 5 + a / (b | 1) + a % (b | 1) + (u64)(sa / sb) + (u64)(sa % sb) + (a >> (i & 63)) + (u64)(sa >> (i & 63)) + a * b;
    }
    line("div64_lo", (u32)q64);
    line("div64_hi", (u32)(q64 >> 32));

    struct big a, b;
    for (int i = 0; i < 13; i++) { a.v[i] = xorshift(); b.v[i] = xorshift(); }
    for (int k = 0; k < 10; k++) a = mix(a, b);
    h = 0; for (int i = 0; i < 13; i++) h ^= a.v[i] + (u32)i;
    line("struct", h);

    line("mandel", mandel());

    double d = 0;
    for (int i = 1; i < 200; i++) d += 1.0 / ((double)i * i) + newton_sqrt((double)i) * 1e-3;
    line("double_lo", dbits_lo(d));
    line("double_hi", dbits_hi(d));
    float f = 1.0f;
    for (int i = 0; i < 100; i++) f = f * 1.0137f + (float)(int)(xorshift() & 255) / 7.0f;
    line("float", fbits(f));
    u32 r3 = xorshift();
    line("f2i", (u32)(s32)(f / 3.0f) + (u32)(s32)(-d * 1000.0) + (u32)(r3 / 3.5));

    u32 c = 0;
    for (int i = 0; i < 64; i++) { u32 v = xorshift() >> (i & 31); c += (u32)(v ? __builtin_clz(v) : 32) + (u32)__builtin_popcount(v); }
    line("bits", c);
}
