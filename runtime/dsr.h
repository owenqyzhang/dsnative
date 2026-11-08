/* dsnative runtime: the interface between recompiled code and the runtime.
 *
 * Every recompiled function has the signature  u32 fn(Cpu *c, u32 pc)  : it is entered at
 * one of its labels (pc selects which), runs guest code until control leaves the function
 * (a call, a return, an indirect jump, a system call that waits, or the end of the time
 * slice) and returns the guest address to continue at. The Thumb bit lives in c->t. The
 * dispatcher (cpu.c) looks the address up and calls the function that holds it. No guest
 * frame lives on the host stack, so guest threads, interrupts and context switches need no
 * special handling: the whole guest state is in Cpu.
 */
#ifndef DSR_H
#define DSR_H
#include <stdint.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

#define PAGE_SHIFT 12
#define PAGE_COUNT (0x10000000u >> PAGE_SHIFT)   /* fast-path pages cover the low 256 MB */

enum { MODE_USR = 0x10, MODE_FIQ = 0x11, MODE_IRQ = 0x12, MODE_SVC = 0x13, MODE_ABT = 0x17, MODE_UND = 0x1B, MODE_SYS = 0x1F };

typedef struct Cpu Cpu;
typedef u32 (*GuestFn)(Cpu *c, u32 pc);

struct Cpu {
    u32 r[16];
    u32 fn, fz, fc, fv, fq;     /* flags, 0 or 1 */
    u32 t;                      /* Thumb state */
    u32 mode, irq_off, fiq_off; /* CPSR control bits */
    u32 spsr;
    u32 bank_r13[8], bank_r14[8], bank_spsr[8];   /* by bank index (see cpu.c) */
    u32 usr_r8_12[5], fiq_r8_12[5];
    s32 budget;                 /* cycles left in this slice */
    u32 halted;                 /* waiting for an interrupt */
    u32 arch;                   /* 5: ARM946E-S (ARMv5TE), 4: ARM7TDMI (ARMv4T) */
    u8 *rp[PAGE_COUNT];         /* read pages: host pointer or NULL for the slow path */
    u8 *wp[PAGE_COUNT];         /* 16/32-bit write pages */
    u8 *wp8[PAGE_COUNT];        /* 8-bit write pages (VRAM ignores 8-bit writes on the ARM9) */
    void *sys;                  /* the machine this CPU belongs to */
    /* slow-path memory access, provided by the machine */
    u32 (*read_slow)(Cpu *c, u32 a, int size);
    void (*write_slow)(Cpu *c, u32 a, u32 v, int size);
    /* the code: a table from guest address to recompiled function (cpu.c) */
    const struct DispatchEntry *table;
    u32 table_len;
    const struct PicEntry *pic; /* code without an address (a stub run from the stack), found by its bytes */
    u32 pic_len;
    /* statistics */
    u64 dispatches, misses;
    u32 last_miss;
    u32 exit_pc;                /* the dispatcher stops when it reaches this address (tests) */
    u32 stop;
    /* BIOS interrupt wait (IntrWait) */
    u32 wait_mask, wait_ret, wait_ret_t;
    /* the machine's interrupt lines */
    int (*irq_line)(Cpu *c);    /* an enabled interrupt is requested (IME, IE & IF) */
    int (*irq_wake)(Cpu *c);    /* IE & IF: wakes a halted CPU */
    void (*remap)(Cpu *c);      /* TCM settings changed */
    void (*log_miss)(Cpu *c, u32 pc, u32 thumb);
    u32 irq_check_addr;         /* where the BIOS IntrWait flags live */
    u32 bios_base;              /* 0xFFFF0000 (ARM9, high vectors) or 0 (ARM7) */
    /* CP15 (ARM9) */
    u32 cp15_ctrl, cp15_dtcm, cp15_itcm;
    u64 cycles;                 /* total cycles run */
    /* the interpreter (interp.c) runs code with no compiled entry, unless no_interp is set */
    u32 no_interp;
    u64 interp_calls, interp_insns;
    u32 interp_res[4], interp_nres;     /* where interpreted flows left the interpreter, to be brought back to (see interp.c) */
};

typedef struct DispatchEntry {
    u32 key;                    /* address | Thumb bit */
    u32 check_len;              /* bytes to compare before running (overlays), 0 for none */
    GuestFn fn;
    const u8 *check;            /* the bytes the code was recompiled from */
} DispatchEntry;

/* Code that runs wherever it is: nothing in it depends on its own address. The dispatcher
 * runs it on an address nothing was compiled for, when memory there holds `bytes`. */
typedef struct PicEntry {
    u32 len;                    /* bytes, a multiple of 4 (ARM code) */
    const u8 *bytes;
    GuestFn fn;
} PicEntry;

/* ---- memory ------------------------------------------------------------- */
u32 dsr_read_slow(Cpu *c, u32 a, int size);
void dsr_write_slow(Cpu *c, u32 a, u32 v, int size);

static inline u32 rd8(Cpu *c, u32 a) {
    if (a < 0x10000000u) { u8 *p = c->rp[a >> PAGE_SHIFT]; if (p) return p[a & 0xFFF]; }
    return c->read_slow(c, a, 1);
}
static inline u32 rd16(Cpu *c, u32 a) {
    a &= ~1u;
    if (a < 0x10000000u) { u8 *p = c->rp[a >> PAGE_SHIFT]; if (p) { u16 v; memcpy(&v, p + (a & 0xFFF), 2); return v; } }
    return c->read_slow(c, a, 2);
}
static inline u32 rd32(Cpu *c, u32 a) {
    a &= ~3u;
    if (a < 0x10000000u) { u8 *p = c->rp[a >> PAGE_SHIFT]; if (p) { u32 v; memcpy(&v, p + (a & 0xFFF), 4); return v; } }
    return c->read_slow(c, a, 4);
}
static inline void wr8(Cpu *c, u32 a, u32 v) {
    if (a < 0x10000000u) { u8 *p = c->wp8[a >> PAGE_SHIFT]; if (p) { p[a & 0xFFF] = (u8)v; return; } }
    c->write_slow(c, a, v & 0xFF, 1);
}
static inline void wr16(Cpu *c, u32 a, u32 v) {
    a &= ~1u;
    if (a < 0x10000000u) { u8 *p = c->wp[a >> PAGE_SHIFT]; if (p) { u16 x = (u16)v; memcpy(p + (a & 0xFFF), &x, 2); return; } }
    c->write_slow(c, a, v & 0xFFFF, 2);
}
static inline void wr32(Cpu *c, u32 a, u32 v) {
    a &= ~3u;
    if (a < 0x10000000u) { u8 *p = c->wp[a >> PAGE_SHIFT]; if (p) { memcpy(p + (a & 0xFFF), &v, 4); return; } }
    c->write_slow(c, a, v, 4);
}

static inline u32 ror32(u32 v, u32 s) { s &= 31; return s ? (v >> s) | (v << (32 - s)) : v; }

/* LDR: an unaligned address returns the aligned word rotated (ARMv4 and ARMv5). */
static inline u32 ldr32(Cpu *c, u32 a) { return ror32(rd32(c, a), (a & 3) << 3); }
/* LDRH / LDRSH: the ARM9 ignores bit 0; the ARM7 rotates (LDRH) or loads a byte (LDRSH). */
static inline u32 ldrh(Cpu *c, u32 a) {
    u32 v = rd16(c, a);
    return (c->arch == 4 && (a & 1)) ? ror32(v, 8) : v;
}
static inline u32 ldrsh(Cpu *c, u32 a) {
    if (c->arch == 4 && (a & 1)) return (u32)(s32)(s8)rd8(c, a);
    return (u32)(s32)(s16)rd16(c, a);
}

/* ---- flags -------------------------------------------------------------- */
static inline void NZ(Cpu *c, u32 r) { c->fn = r >> 31; c->fz = r == 0; }
static inline u32 ADDS(Cpu *c, u32 a, u32 b) { u32 r = a + b; NZ(c, r); c->fc = r < a; c->fv = ((a ^ r) & (b ^ r)) >> 31; return r; }
static inline u32 ADCS(Cpu *c, u32 a, u32 b) { u64 t = (u64)a + b + c->fc; u32 r = (u32)t; NZ(c, r); c->fc = (u32)(t >> 32); c->fv = ((a ^ r) & (b ^ r)) >> 31; return r; }
static inline u32 SUBS(Cpu *c, u32 a, u32 b) { u32 r = a - b; NZ(c, r); c->fc = a >= b; c->fv = ((a ^ b) & (a ^ r)) >> 31; return r; }
static inline u32 SBCS(Cpu *c, u32 a, u32 b) { u32 bw = !c->fc; u32 r = a - b - bw; NZ(c, r); c->fc = (u64)a >= (u64)b + bw; c->fv = ((a ^ b) & (a ^ r)) >> 31; return r; }

/* register-specified shifts: the amount is the bottom byte of Rs */
static inline u32 LSL_R(u32 v, u32 s, u32 cin, u32 *co) {
    s &= 0xFF;
    if (s == 0) { *co = cin; return v; }
    if (s < 32) { *co = (v >> (32 - s)) & 1; return v << s; }
    *co = s == 32 ? (v & 1) : 0; return 0;
}
static inline u32 LSR_R(u32 v, u32 s, u32 cin, u32 *co) {
    s &= 0xFF;
    if (s == 0) { *co = cin; return v; }
    if (s < 32) { *co = (v >> (s - 1)) & 1; return v >> s; }
    *co = s == 32 ? (v >> 31) : 0; return 0;
}
static inline u32 ASR_R(u32 v, u32 s, u32 cin, u32 *co) {
    s &= 0xFF;
    if (s == 0) { *co = cin; return v; }
    if (s < 32) { *co = ((s32)v >> (s - 1)) & 1; return (u32)((s32)v >> s); }
    *co = v >> 31; return (u32)((s32)v >> 31);
}
static inline u32 ROR_R(u32 v, u32 s, u32 cin, u32 *co) {
    s &= 0xFF;
    if (s == 0) { *co = cin; return v; }
    s &= 31;
    if (s == 0) { *co = v >> 31; return v; }
    *co = (v >> (s - 1)) & 1; return ror32(v, s);
}

/* saturating arithmetic (QADD family) */
static inline u32 QSAT(Cpu *c, s64 v) {
    if (v > 0x7FFFFFFFLL) { c->fq = 1; return 0x7FFFFFFFu; }
    if (v < -0x80000000LL) { c->fq = 1; return 0x80000000u; }
    return (u32)v;
}

/* ---- CPU state (cpu.c) -------------------------------------------------- */
u32 dsr_cpsr(Cpu *c);
void dsr_set_cpsr(Cpu *c, u32 v, u32 mask);
u32 dsr_get_spsr(Cpu *c);
void dsr_set_spsr(Cpu *c, u32 v, u32 mask);
void dsr_msr(Cpu *c, int spsr, u32 field_mask, u32 v);
u32 dsr_exception_return(Cpu *c, u32 target);   /* CPSR = SPSR, then jump (T from the new CPSR) */
u32 dsr_get_user_reg(Cpu *c, int n);
void dsr_set_user_reg(Cpu *c, int n, u32 v);
u32 dsr_undefined(Cpu *c, u32 pc, u32 word);
u32 dsr_bad_entry(Cpu *c, u32 pc, u32 fn);
u32 dsr_swi(Cpu *c, u32 num, u32 next);          /* BIOS call; nonzero: leave the function for that address */
int dsr_cp15_write(Cpu *c, u32 crn, u32 crm, u32 op2, u32 v);   /* nonzero: leave (halt) */
u32 dsr_cp15_read(Cpu *c, u32 crn, u32 crm, u32 op2);

/* The interpreter (interp.c): runs the code at pc, which nothing was compiled for (or whose
 * bytes changed), until control reaches an address the dispatcher can run; returns that address. */
u32 dsr_interp(Cpu *c, u32 pc);

/* BX-style jump: bit 0 selects the state */
static inline u32 dsr_bx(Cpu *c, u32 v) { c->t = v & 1; return c->t ? (v & ~1u) : (v & ~3u); }
/* a load to the PC: ARMv5 interworks, ARMv4 stays in the current state */
static inline u32 dsr_ldpc(Cpu *c, u32 v) {
    if (c->arch >= 5) return dsr_bx(c, v);
    return c->t ? (v & ~1u) : (v & ~3u);
}

#define R (c->r)

#endif
