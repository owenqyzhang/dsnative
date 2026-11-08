/* dsnative runtime: the Wi-Fi hardware as a stub. References: GBATEK "DS Wireless Communications"
 * (the I/O map, "DS Wifi Control", "Interrupts", "Power-Down Registers", "Receive/Transmit
 * Buffers", "Baseband Chip", "RF Chip", "Initialization"). */
#include "nds.h"

enum { K_NONE, K_RW, K_RO, K_WO };      /* a port is unused, read/write (masked), read-only or write-only */
typedef struct { u16 off, mask, init; u8 kind; } Port;
#define RW(o, m, i) {o, m, i, K_RW}
#define RO(o, i) {o, 0, i, K_RO}
#define WO(o) {o, 0, 0, K_WO}

/* The I/O map: offset from 0x04808000, writable bits, value after reset. The ports marked "?"
 * or "unknown" in GBATEK read as their reset value here. */
static const Port ports[] = {
    RO(0x000, 0x1440), RW(0x004, 0x9FFF, 0), RW(0x006, 0x007F, 0), RW(0x008, 0xFFFF, 0), RW(0x00A, 0xFFFF, 0),
    RW(0x010, 0xFFFF, 0), RW(0x012, 0xFFFF, 0),
    RW(0x018, 0xFFFF, 0), RW(0x01A, 0xFFFF, 0), RW(0x01C, 0xFFFF, 0),
    RW(0x020, 0xFFFF, 0), RW(0x022, 0xFFFF, 0), RW(0x024, 0xFFFF, 0),
    RW(0x028, 0x000F, 0), RW(0x02A, 0x07FF, 0), RW(0x02C, 0xFFFF, 0x0707), RW(0x02E, 0x0001, 0),
    RW(0x030, 0xFF0E, 0), RW(0x032, 0xFFFF, 0), RO(0x034, 0),
    RW(0x036, 0x0003, 0x0001), RW(0x038, 0x0007, 0x0003), RW(0x03C, 0x0003, 0), RW(0x040, 0x8001, 0),
    RO(0x044, 0), RW(0x048, 0x0003, 0),
    RW(0x050, 0xFFFF, 0x4000), RW(0x052, 0xFFFF, 0x4800), RO(0x054, 0), RW(0x056, 0x0FFF, 0),
    RW(0x058, 0x1FFE, 0), RW(0x05A, 0x0FFF, 0), RW(0x05C, 0x0FFF, 0), RO(0x060, 0),
    RW(0x062, 0x1FFE, 0), RW(0x064, 0x0FFF, 0),
    RW(0x068, 0x1FFE, 0), RW(0x06C, 0x0FFF, 0), WO(0x070), RW(0x074, 0x1FFE, 0), RW(0x076, 0x0FFF, 0),
    RW(0x080, 0xFFFF, 0), RW(0x084, 0x00FF, 0), RW(0x088, 0x00FF, 0), RW(0x08C, 0x03FF, 0x0064),
    RW(0x08E, 0x00FF, 0), RW(0x090, 0xFFFF, 0), RW(0x094, 0xFFFF, 0), RO(0x098, 0), RW(0x09C, 0xFFFF, 0x0050),
    RW(0x0A0, 0xFFFF, 0), RW(0x0A4, 0xFFFF, 0), RW(0x0A8, 0xFFFF, 0), WO(0x0AC), WO(0x0AE),
    RO(0x0B0, 0x0010), WO(0x0B4), RO(0x0B6, 0), RO(0x0B8, 0),
    RW(0x0BC, 0x0003, 0x0001), RW(0x0C0, 0xFFFF, 0), RW(0x0C4, 0xFFFF, 0),
    RW(0x0D0, 0x1FFF, 0x0401), RW(0x0D4, 0x0003, 0x0001), RW(0x0D8, 0x0FFF, 0x0004), RW(0x0DA, 0xFFFF, 0x0602),
    RW(0x0E0, 0x000F, 0x0008),
    RW(0x0E8, 0x0001, 0), RW(0x0EA, 0x0001, 0), RW(0x0EC, 0x3F1F, 0x3F03), RW(0x0EE, 0x0001, 0x0001),
    RW(0x0F0, 0xFC00, 0xFC00), RW(0x0F2, 0xFFFF, 0xFFFF), RW(0x0F4, 0xFFFF, 0xFFFF), RW(0x0F6, 0xFFFF, 0xFFFF),
    RW(0x0F8, 0xFFFF, 0), RW(0x0FA, 0xFFFF, 0), RW(0x0FC, 0xFFFF, 0), RW(0x0FE, 0xFFFF, 0),
    RW(0x10C, 0xFFFF, 0), RW(0x110, 0xFFFF, 0), RW(0x118, 0xFFFF, 0), RW(0x11C, 0xFFFF, 0),
    RW(0x120, 0x81FF, 0x0048), RW(0x122, 0xFFFF, 0x4840), RW(0x124, 0xFFFF, 0), RW(0x128, 0xFFFF, 0),
    RW(0x130, 0x0FFF, 0x0142), RW(0x132, 0x8FFF, 0x8064), RW(0x134, 0xFFFF, 0xFFFF),
    RW(0x140, 0xFFFF, 0), RW(0x142, 0xFFFF, 0x2443), RW(0x144, 0x00FF, 0x0042), RW(0x146, 0x00FF, 0x0016),
    RW(0x148, 0x00FF, 0x0016), RW(0x14A, 0x00FF, 0x0016), RW(0x14C, 0xFFFF, 0x162C), RW(0x150, 0xFF3F, 0x0204),
    RW(0x154, 0x7A7F, 0x0058),
    RW(0x158, 0xF0FF, 0x00B5), RW(0x15A, 0x00FF, 0), RO(0x15C, 0x00B5), RO(0x15E, 0),
    RW(0x160, 0x4100, 0x0100), RW(0x168, 0x800F, 0x800D),
    RW(0x17C, 0xFFFF, 0x0800), RW(0x17E, 0xFFFF, 0xC008), RO(0x180, 0), RW(0x184, 0x413F, 0x0018),
    RW(0x190, 0xFFFF, 0), RW(0x194, 0x0007, 0), RW(0x198, 0x000F, 0), RO(0x19C, 0x0004),
    RW(0x1A0, 0x0933, 0), RW(0x1A2, 0x0003, 0x0001), RW(0x1A4, 0xFFFF, 0),
    RO(0x1A8, 0), RW(0x1AA, 0xFFFF, 0), RO(0x1AC, 0), RW(0x1AE, 0xFFFF, 0),
    RW(0x1B0, 0xFFFF, 0), RW(0x1B2, 0xFFFF, 0), RW(0x1B4, 0xFFFF, 0), RW(0x1B6, 0xFFFF, 0),
    RW(0x1B8, 0xFFFF, 0), RW(0x1BA, 0xFFFF, 0), RW(0x1BC, 0xFFFF, 0), RW(0x1BE, 0xFFFF, 0),
    RW(0x1C0, 0x00FF, 0), RO(0x1C4, 0),
    RW(0x1D0, 0xFFFF, 0), RW(0x1D2, 0xFFFF, 0), RW(0x1D4, 0xFFFF, 0), RW(0x1D6, 0xFFFF, 0),
    RW(0x1D8, 0xFFFF, 0), RW(0x1DA, 0xFFFF, 0), RW(0x1DC, 0xFFFF, 0), RW(0x1DE, 0xFFFF, 0),
    RW(0x1F0, 0x0003, 0), RO(0x204, 0), RO(0x208, 0), WO(0x20C), RO(0x210, 0), RO(0x214, 0x0009),
    WO(0x21C), RW(0x220, 0xFFFF, 0), RW(0x224, 0x0003, 0x0003), WO(0x228), RW(0x230, 0x00FF, 0x0047),
    RW(0x234, 0x0EFF, 0x0EFF), RW(0x238, 0xFFFF, 0), RW(0x244, 0xFFFF, 0), RW(0x248, 0xFFFF, 0),
    RO(0x24C, 0), RO(0x24E, 0), RO(0x250, 0), RO(0x254, 0xFFFF), RO(0x258, 0), RO(0x25C, 0), RO(0x260, 0x0FEF),
    RO(0x264, 0), RO(0x268, 0x0005), RO(0x270, 0), RO(0x274, 0x0001), RW(0x278, 0xFFFF, 0x000F), RO(0x27C, 0x000A),
    RO(0x290, 0xFFFF), WO(0x298), RW(0x2A0, 0xFFFF, 0), RO(0x2A2, 0x7FFF), RO(0x2A4, 0), WO(0x2A8),
    RO(0x2AC, 0x0038), WO(0x2B0), RW(0x2B4, 0x1003, 0), RO(0x2B8, 0), RW(0x2C0, 0x0001, 0), RO(0x2C4, 0x000A),
    RO(0x2C8, 0), RO(0x2CC, 0), RW(0x2F0, 0xFFFF, 0), RW(0x2F2, 0xFFFF, 0), RW(0x2F4, 0xFFFF, 0), RW(0x2F6, 0xFFFF, 0),
};

enum {
    W_MODE_RST = 0x004, W_IF = 0x010, W_IE = 0x012, W_RXCNT = 0x030, W_POWER_US = 0x036, W_POWERSTATE = 0x03C,
    W_POWERFORCE = 0x040, W_RANDOM = 0x044, W_RXBUF_BEGIN = 0x050, W_RXBUF_END = 0x052, W_RXBUF_WRCSR = 0x054,
    W_RXBUF_WR_ADDR = 0x056, W_RXBUF_RD_ADDR = 0x058, W_RXBUF_COUNT = 0x05C, W_RXBUF_RD_DATA = 0x060,
    W_RXBUF_GAP = 0x062, W_RXBUF_GAPDISP = 0x064, W_TXBUF_WR_ADDR = 0x068, W_TXBUF_COUNT = 0x06C,
    W_TXBUF_WR_DATA = 0x070, W_TXBUF_GAP = 0x074, W_TXBUF_GAPDISP = 0x076, W_TXBUF_MIRROR = 0x078,
    W_TXBUF_CMD = 0x090, W_TXBUF_REPLY1 = 0x094, W_TXBUF_REPLY2 = 0x098, W_TXBUF_LOC1 = 0x0A0, W_TXBUF_LOC2 = 0x0A4,
    W_TXBUF_LOC3 = 0x0A8, W_TXREQ_RESET = 0x0AC, W_TXREQ_SET = 0x0AE, W_TXREQ_READ = 0x0B0, W_TXBUF_RESET = 0x0B4,
    W_US_COUNTCNT = 0x0E8, W_US_COUNT0 = 0x0F8, W_BB_CNT = 0x158, W_BB_WRITE = 0x15A, W_BB_READ = 0x15C,
    W_BB_POWER = 0x168, W_RF_DATA2 = 0x17C, W_RF_DATA1 = 0x17E, W_RF_PINS = 0x19C, W_RF_STATUS = 0x214,
    W_IF_SET = 0x21C,
};

static u16 pmask[0x800], pinit[0x800];
static u8 pkind[0x800];
static u16 rnd_seq[0x5FD];

static u16 *port(Wifi *w, u32 o) { return &w->reg[o >> 1]; }

static void irq_update(Nds *s) {
    Wifi *w = &s->wifi;
    u16 active = *port(w, W_IF) & *port(w, W_IE);
    if (active && !w->irq_active) nds_request_irq(s, 1, 1u << 24);      /* ARM7 IF bit 24: only on a rise */
    w->irq_active = active;
}

static void if_set(Nds *s, u16 bits) {
    *port(&s->wifi, W_IF) |= bits & 0xFBFF;       /* IRQ10 doesn't exist */
    irq_update(s);
}

/* ---- the baseband chip (Mitsumi MM3155): 8-bit registers ----------------------------------- */
static int bb_writable(u32 i) {
    return (i >= 0x01 && i <= 0x0C) || (i >= 0x13 && i <= 0x15) || (i >= 0x1B && i <= 0x26) ||
           (i >= 0x28 && i <= 0x4C) || (i >= 0x4E && i <= 0x5C) || i == 0x62 || i == 0x63 || i == 0x65 ||
           i == 0x67 || i == 0x68;
}

static u8 bb_read(Wifi *w, u32 i) {
    if (i == 0x00) return 0x6D;                 /* chip ID */
    if (i == 0x5D) return 0x01;
    if (i == 0x4D) return 0x00;                 /* 00h or BFh depending on other registers */
    if (i == 0x64) return 0xFF;                 /* FFh or 3Fh */
    return bb_writable(i) ? w->bb[i] : 0;
}

/* ---- the RF chip (RF9008): 18-bit registers -------------------------------------------------- */
static void rf_reset(Wifi *w) {
    static const struct { u8 i; u32 v; } on[] = {
        {0x00, 0x00007}, {0x01, 0x09003}, {0x02, 0x00022}, {0x03, 0x1FF78}, {0x04, 0x09003}, {0x05, 0x01780},
        {0x07, 0x14578}, {0x08, 0x1E742}, {0x09, 0x00120}, {0x1B, 0x0000F}, {0x1F, 0x00001},
    };
    memset(w->rf, 0, sizeof w->rf);
    for (size_t k = 0; k < sizeof on / sizeof on[0]; k++) w->rf[on[k].i] = on[k].v;
}

/* ---- the microsecond counter ------------------------------------------------------------------ */
static u64 us_value(Nds *s) {
    Wifi *w = &s->wifi;
    if (!(*port(w, W_US_COUNTCNT) & 1)) return w->us_count;
    return w->us_count + (s->time - w->us_time) * 1000000ull / RTC_HZ;
}

static void us_set(Nds *s, u64 v) { s->wifi.us_count = v; s->wifi.us_time = s->time; }

/* ---- reads ------------------------------------------------------------------------------------ */
static u16 wrap_buffer(Wifi *w, u16 addr, u16 gap, u16 gapdisp) {
    /* the circular buffer's address step: wrap at its end, then jump the gap (GBATEK "DS Wifi
     * Receive Buffer"); the transmit side has no end to wrap at */
    u16 end = *port(w, W_RXBUF_END) & 0x1FFE, begin = *port(w, W_RXBUF_BEGIN) & 0x1FFE;
    addr = (u16)((addr + 2) & 0x1FFE);
    if (end && addr == end) addr = begin;
    if (gap && addr == gap) {
        addr = (u16)(gap + gapdisp * 2);
        if (end && addr >= end) addr = (u16)(addr + begin - end);
        addr &= 0x1FFE;
    }
    return addr;
}

static u16 port_read(Nds *s, u32 o) {
    Wifi *w = &s->wifi;
    switch (o) {
    case W_RANDOM: {            /* the value latched at the last read, then a new latch */
        u16 old = w->rand_latch;
        w->rand_latch = rnd_seq[s->time % 0x5FD];
        return old;
    }
    case W_POWERSTATE: return (u16)((w->power_off ? 0x200 : 0) | (*port(w, o) & 3));
    case W_RXBUF_RD_DATA: {
        u16 *addr = port(w, W_RXBUF_RD_ADDR), *cnt = port(w, W_RXBUF_COUNT);
        u16 v = (u16)(w->ram[*addr & 0x1FFE] | w->ram[(*addr & 0x1FFE) + 1] << 8);
        *addr = wrap_buffer(w, *addr, *port(w, W_RXBUF_GAP), *port(w, W_RXBUF_GAPDISP));
        if (*cnt && !--*cnt) if_set(s, 1 << 9);     /* IRQ9: the count ran out; it stays at zero */
        return v;
    }
    case W_TXBUF_MIRROR: return *port(w, W_TXBUF_WR_ADDR);
    case W_US_COUNT0: case W_US_COUNT0 + 2: case W_US_COUNT0 + 4: case W_US_COUNT0 + 6:
        return (u16)(us_value(s) >> (16 * ((o - W_US_COUNT0) >> 1)));
    }
    if (pkind[o >> 1] == K_NONE || pkind[o >> 1] == K_WO) return 0;
    return *port(w, o);
}

/* ---- writes ----------------------------------------------------------------------------------- */
static void reset_ports(Wifi *w, const u16 *offs, int n) {
    for (int i = 0; i < n; i++) *port(w, offs[i]) = pinit[offs[i] >> 1];
}

static void mode_rst_write(Nds *s, u16 v) {
    Wifi *w = &s->wifi;
    u16 old = *port(w, W_MODE_RST);
    *port(w, W_MODE_RST) = v & 0x9FFF;
    if (v & 0x2000) {
        static const u16 list[] = {W_RXBUF_WR_ADDR, 0x0C0, 0x0C4, 0x1A4, 0x278};
        reset_ports(w, list, 5);
    }
    if (v & 0x4000) {
        static const u16 list[] = {0x006, 0x008, 0x00A, 0x018, 0x01A, 0x01C, 0x020, 0x022, 0x024, 0x028, 0x02A, 0x02C,
                                   0x02E, W_RXBUF_BEGIN, W_RXBUF_END, 0x084, 0x0BC, 0x0D0, 0x0D4, 0x0E0, 0x0EC,
                                   0x194, 0x198, 0x1A2, 0x224, 0x230};
        reset_ports(w, list, (int)(sizeof list / sizeof list[0]));
    }
    if (!(old & 1) && (v & 1)) {        /* 0 then 1 */
        *port(w, 0x034) = 0x0002; *port(w, W_RF_PINS) = 0x0046; *port(w, W_RF_STATUS) = 0x0009; *port(w, 0x27C) = 0x0005;
    }
    if ((old & 1) && !(v & 1)) *port(w, 0x27C) = 0x000A;      /* 1 then 0 */
}

static void power_write(Nds *s, u32 o, u16 v) {
    Wifi *w = &s->wifi;
    if (o == W_POWERSTATE) {
        *port(w, o) = v & 3;
        /* a queued request to power up works once the chip is out of its power-down bits and
         * the hardware mode is 1: the RF part wakes (IRQ11) */
        if ((v & 2) && !(*port(w, W_POWER_US) & 1) && (*port(w, W_MODE_RST) & 1)) {
            *port(w, o) &= (u16)~2;
            if (w->power_off) { w->power_off = 0; if_set(s, 1 << 11); }
        }
        return;
    }
    *port(w, o) = v & 0x8001;           /* W_POWERFORCE */
    if (v & 0x8000) {
        int off = v & 1;
        if (off && !w->power_off) {     /* power down at once */
            *port(w, 0x034) = 0x0002; *port(w, W_TXREQ_READ) = 0; *port(w, W_RF_PINS) = 0x0046; *port(w, W_RF_STATUS) = 0x0009;
        }
        w->power_off = (u8)off;
    }
}

static void bb_write_port(Wifi *w, u16 v) {
    *port(w, W_BB_CNT) = v & 0xF0FF;
    /* the serial interface works once W_POWER_US bit 0 and W_BB_POWER bit 15 are clear; it is
     * ready again at once */
    if ((*port(w, W_POWER_US) & 1) || (*port(w, W_BB_POWER) & 0x8000)) return;
    u32 idx = v & 0xFF, dir = (v >> 12) & 0xF;
    if (dir == 5) { if (idx < sizeof w->bb && bb_writable(idx)) w->bb[idx] = (u8)*port(w, W_BB_WRITE); }
    else if (dir == 6) *port(w, W_BB_READ) = bb_read(w, idx);
}

static void rf_write_port(Wifi *w, u16 v) {
    /* 24-bit transfers of the type 2 chip: DATA2 holds the index (bits 2-6), the command (bit 7:
     * 1 reads) and the top two data bits, DATA1 the low 16 */
    *port(w, W_RF_DATA2) = v;
    u32 idx = (v >> 2) & 0x1F;
    if (v & 0x80) {
        u32 d = w->rf[idx];
        *port(w, W_RF_DATA1) = (u16)d;
        *port(w, W_RF_DATA2) = (u16)((v & ~3u) | (d >> 16 & 3));
    } else {
        w->rf[idx] = (u32)(v & 3) << 16 | *port(w, W_RF_DATA1);
        if (idx == 0x1F) rf_reset(w);       /* writing the reset register resets the chip */
    }
}

static void port_write(Nds *s, u32 o, u16 v) {
    Wifi *w = &s->wifi;
    u32 kind = pkind[o >> 1];
    switch (o) {
    case W_MODE_RST: mode_rst_write(s, v); return;
    case W_IF: *port(w, W_IF) &= (u16)~v; irq_update(s); return;       /* write 1 to clear */
    case W_IE: *port(w, W_IE) = v; irq_update(s); return;
    case W_IF_SET: if_set(s, v); return;
    case W_RXCNT:
        *port(w, o) = v & 0xFF0E;
        if (v & 1) *port(w, W_RXBUF_WRCSR) = *port(w, W_RXBUF_WR_ADDR) & 0x0FFF;     /* force the buffer empty */
        if (v & 0x80) { *port(w, W_TXBUF_REPLY2) = *port(w, W_TXBUF_REPLY1); *port(w, W_TXBUF_REPLY1) = 0; }
        return;
    case W_POWERSTATE: case W_POWERFORCE: power_write(s, o, v); return;
    case W_TXBUF_WR_DATA: {
        u16 *addr = port(w, W_TXBUF_WR_ADDR), *cnt = port(w, W_TXBUF_COUNT), gap = *port(w, W_TXBUF_GAP);
        w->ram[*addr & 0x1FFE] = (u8)v; w->ram[(*addr & 0x1FFE) + 1] = (u8)(v >> 8);
        *addr = (u16)((*addr + 2) & 0x1FFE);
        if (gap && *addr == gap) *addr = (u16)((*addr + *port(w, W_TXBUF_GAPDISP) * 2) & 0x1FFE);
        if (*cnt && !--*cnt) if_set(s, 1 << 8);     /* IRQ8: the count ran out */
        return;
    }
    case W_TXREQ_SET: *port(w, W_TXREQ_READ) |= v & 0x0F; return;      /* requests are held, never carried out */
    case W_TXREQ_RESET: *port(w, W_TXREQ_READ) &= (u16)~(v & 0x0F); return;
    case W_TXBUF_RESET: {
        static const u16 loc[4] = {W_TXBUF_LOC1, W_TXBUF_CMD, W_TXBUF_LOC2, W_TXBUF_LOC3};
        for (int i = 0; i < 4; i++) if (v & (1 << i)) *port(w, loc[i]) &= 0x7FFF;
        return;
    }
    case W_US_COUNTCNT: us_set(s, us_value(s)); *port(w, o) = v & 1; return;
    case W_US_COUNT0: case W_US_COUNT0 + 2: case W_US_COUNT0 + 4: case W_US_COUNT0 + 6: {
        u32 sh = 16 * ((o - W_US_COUNT0) >> 1);
        us_set(s, (us_value(s) & ~(0xFFFFull << sh)) | (u64)v << sh);
        return;
    }
    case W_BB_CNT: bb_write_port(w, v); return;
    case W_RF_DATA2: rf_write_port(w, v); return;
    }
    if (kind == K_RW) *port(w, o) = (u16)((*port(w, o) & ~pmask[o >> 1]) | (v & pmask[o >> 1]));
}

/* ---- the bus -------------------------------------------------------------------------------- */
void wifi_init(Nds *s) {
    Wifi *w = &s->wifi;
    for (size_t i = 0; i < sizeof ports / sizeof ports[0]; i++) {
        u32 k = ports[i].off >> 1;
        pkind[k] = ports[i].kind; pmask[k] = ports[i].mask; pinit[k] = ports[i].init;
    }
    u16 x = 0x7FF;              /* W_RANDOM: x = (x & 1) ^ rol11(x); 5FDh values before it repeats */
    for (int i = 0; i < 0x5FD; i++) { rnd_seq[i] = x; x = (u16)((x & 1) ^ (((x << 1) | (x >> 10)) & 0x7FF)); }
    memset(w, 0, sizeof *w);
    for (u32 k = 0; k < 0x800; k++) w->reg[k] = pinit[k];
    w->power_off = 1;           /* W_POWERSTATE reads 0200h after reset */
    rf_reset(w);
}

/* The ARM7 sees the ports at 0x04808000 and the RAM at 0x04804000, both repeated through 32 KB
 * regions at 0x04800000 and 0x04808000. */
static int decode(int cpu, u32 off, u32 *rel) {
    if (cpu != 1 || off < 0x800000 || off >= 0x810000) return 0;
    *rel = (off - 0x800000) & 0x7FFF;
    return 1;
}

int wifi_io_read(Nds *s, int cpu, u32 off, int size, u32 *v) {
    u32 rel;
    if (!decode(cpu, off, &rel)) return 0;
    Wifi *w = &s->wifi;
    if (rel >= 0x4000 && rel < 0x6000) {
        u32 a = rel - 0x4000, r = 0;
        memcpy(&r, &w->ram[a & ~(size == 4 ? 3u : size == 2 ? 1u : 0u)], (size_t)size);
        *v = r;
        return 1;
    }
    if (rel >= 0x1000) { *v = 0; return 1; }
    u32 r = port_read(s, rel & ~1u);
    if (size == 4) r |= (u32)port_read(s, (rel & ~3u) + 2) << 16;
    else if (size == 1) r = (r >> ((rel & 1) * 8)) & 0xFF;
    *v = r;
    return 1;
}

int wifi_io_write(Nds *s, int cpu, u32 off, int size, u32 v) {
    u32 rel;
    if (!decode(cpu, off, &rel)) return 0;
    if (size == 1) return 1;                    /* STRB to the ports and the RAM is ignored */
    Wifi *w = &s->wifi;
    if (rel >= 0x4000 && rel < 0x6000) {
        u32 a = (rel - 0x4000) & ~(size == 4 ? 3u : 1u);
        memcpy(&w->ram[a], &v, (size_t)size);
        return 1;
    }
    if (rel >= 0x1000) return 1;
    if (size == 4) { port_write(s, rel & ~3u, (u16)v); port_write(s, (rel & ~3u) + 2, (u16)(v >> 16)); }
    else port_write(s, rel & ~1u, (u16)v);
    return 1;
}
