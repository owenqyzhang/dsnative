/* dsnative runtime: the firmware flash. References: GBATEK "DS Firmware Serial Flash Memory"
 * (the instruction set), "DS Firmware Header", "DS Firmware Wifi Calibration Data", "DS Firmware
 * Wifi Internet Access Points" and "DS Firmware User Settings" (the contents). */
#include <stdlib.h>
#include "nds.h"

/* CRC-16 with the reflected polynomial A001h, as the SDK and the firmware use it: the settings
 * start from FFFFh, the Wi-Fi block and the access point slots from 0. */
static u16 crc16(u16 crc, const u8 *p, u32 n) {
    for (u32 i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
    }
    return crc;
}

static void put16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put24(u8 *p, u32 v) { put16(p, v); p[2] = (u8)(v >> 16); }

/* The Wi-Fi hardware settings at 2Ah (the part after the header). The values are the ones
 * GBATEK gives as the same in every existing DS (the ports' values after initialization from
 * the firmware, the RF chip's example table); what is specific to a console (the MAC address,
 * the BB[1Eh] calibration) is a placeholder: the MAC address is a locally administered one
 * ("DSNAT"). Nothing reads them while the Wi-Fi chip is a stub (wifi.c). */
static void build_wifi_settings(u8 *f) {
    static const u16 ports[16] = {          /* 044h..062h: W_CONFIG_146h, 148h, 14Ah, 14Ch, 120h, 122h, 154h, 144h, */
        0x0002, 0x0017, 0x0026, 0x1818,     /* 130h, 132h, 140h, 142h, W_POWER_TX, W_CONFIG_124h, 128h, 150h */
        0x0048, 0x4840, 0x0058, 0x0042,
        0x0140, 0x8064, 0xE0E0, 0x2443,
        0x0003, 0x0032, 0x01F4, 0x0101,
    };
    static const u32 rf[12] = {             /* RF[0, 4, 5, 6, 7, 8, 9, 0Ah, 0Bh, 1, 2, 3]: index << 18 | 18-bit data */
        0x00C007, 0x129C03, 0x141728, 0x1AE8BA, 0x1D456F, 0x23FFFA, 0x241D30, 0x280001, 0x2C0000,
        0x069C03, 0x080022, 0x0DFF6F,
    };
    memset(f + 0x2A, 0xFF, 0x1D6);          /* 2Ah..1FFh: unused bytes are FFh */
    put16(f + 0x2C, 0x0138);                /* length of the settings: 2Ch..163h */
    f[0x2E] = 0;
    f[0x2F] = 0;                            /* version 0: an original DS (W_ID reads 1440h) */
    memset(f + 0x30, 0, 6);
    static const u8 mac[6] = {0x02, 'D', 'S', 'N', 'A', 'T'};
    memcpy(f + 0x36, mac, 6);
    put16(f + 0x3C, 0x3FFE);                /* channels 1 to 13 */
    put16(f + 0x3E, 0xFFFF);
    f[0x40] = 2;                            /* RF chip type 2 (RF9008) */
    f[0x41] = 0x18;                         /* 24 bits an entry */
    f[0x42] = 12;
    f[0x43] = 1;
    for (int i = 0; i < 16; i++) put16(f + 0x44 + 2 * i, ports[i]);
    memset(f + 0x64, 0, 0x69);              /* BB[0..68h]: the registers' initial values */
    f[0x64 + 0x01] = 0x9E; f[0x64 + 0x13] = 0x00; f[0x64 + 0x1E] = 0xBB; f[0x64 + 0x35] = 0x1F;
    f[0xCD] = 0;
    for (int i = 0; i < 12; i++) put24(f + 0xCE + 3 * i, rf[i]);
    for (int ch = 1; ch <= 14; ch++) {      /* channels 1-14: RF[5] and RF[6], the synthesizer words */
        /* The RF PLL multiplies a 22 MHz reference by N + 17 + a 24-bit fraction (RF[5] bits 17-6 hold
         * N, bits 5-0 and RF[6] the fraction), which is how GBATEK's example entries for channel 1
         * (2412 MHz: N = 92, fraction 7/11 = 10676410/2^24) come out. The other channels follow from
         * their centre frequencies, 5 MHz apart and 2484 MHz for channel 14. */
        u32 mhz = ch == 14 ? 2484 : 2412 + 5 * (u32)(ch - 1);
        u32 n = mhz / 22 - 17, frac = (u32)(((u64)(mhz % 22) << 24) * 2 / 22 + 1) / 2;
        put24(f + 0xF2 + 6 * (ch - 1), 5u << 18 | n << 6 | frac >> 18);
        put24(f + 0xF5 + 6 * (ch - 1), 6u << 18 | (frac & 0x3FFFF));
    }
    memset(f + 0x146, 0xB4, 14);            /* BB[1Eh] per channel: a calibration, B1h to B7h on real consoles */
    memset(f + 0x154, 0x10, 14);            /* RF[9] bits 10-14 per channel */
    f[0x162] = 0x1A;
    put16(f + 0x2A, crc16(0, f + 0x2C, 0x138));
}

/* An access point slot of a console that has never been set up: zeros, with 0xFF as the status
 * (not configured, which the SDK's check accepts) and no connection marked in the bit mask at
 * EFh. The CRC covers 00h..FDh, starting from 0. No network, no Wi-Fi Connection user ID. */
static void build_ap_slot(u8 *slot) {
    memset(slot, 0, 0x100);
    slot[0xE7] = 0xFF;
    put16(slot + 0xFE, crc16(0, slot, 0xFE));
}

static void build_user_settings(u8 *f) {
    for (int copy = 0; copy < 2; copy++) {
        u8 *u = f + FW_USER_SETTINGS + copy * 0x100;
        memset(u, 0, 0x100);
        u[0x00] = 5; u[0x02] = 11; u[0x03] = 1; u[0x04] = 1;
        const char *name = "dsnative";
        for (int i = 0; name[i]; i++) u[0x06 + 2 * i] = (u8)name[i];
        u[0x1A] = 8;
        u16 cal[8] = {0x200, 0x200, 0x20, 0x20, 0xE00, 0x800, 0xE0, 0xA0};   /* touch calibration */
        put16(u + 0x58, cal[0]); put16(u + 0x5A, cal[1]); u[0x5C] = 0x20; u[0x5D] = 0x20;
        put16(u + 0x5E, cal[4]); put16(u + 0x60, cal[5]); u[0x62] = 0xE0; u[0x63] = 0xA0;
        /* English, GBA screen bottom, and the "settings okay" bits (10, 11, 13, 14, 15) that a
         * console which has been through the boot menu has set */
        put16(u + 0x64, 0x01 | 0x08 | 0xEC00);
        u[0x66] = 26;                               /* year 2026 */
        u[0x70] = (u8)copy;                         /* update counter: area 1 is the newer */
        put16(u + 0x72, crc16(0xFFFF, u, 0x70));
    }
}

void firmware_build(u8 *f) {
    memset(f, 0xFF, FW_SIZE);
    memset(f, 0, 0x1E);                             /* the bootcode's addresses and CRCs: no firmware code here */
    put16(f + 0x14, (FW_SIZE / 0x20000) << 12);     /* chip size in 128 KB units */
    f[0x1D] = 0xFF;                                 /* console type: DS */
    put16(f + 0x20, FW_USER_SETTINGS / 8);
    memset(f + 0x22, 0, 6);
    build_wifi_settings(f);
    for (int i = 0; i < 3; i++) build_ap_slot(f + FW_AP_SLOTS + 0x100 * i);
    /* the hidden fourth slot, which the SDK's check reads too: zeros, whose CRC is 0 */
    memset(f + FW_AP_SLOTS + 0x300, 0, 0x100);
    build_user_settings(f);
}

void firmware_init(Nds *s) {
    firmware_build(s->fw.data);
}

/* ---- the chip ------------------------------------------------------------------------------- */
enum { OP_WRDI = 0x04, OP_RDSR = 0x05, OP_WREN = 0x06, OP_READ = 0x03, OP_FAST = 0x0B, OP_PW = 0x0A,
       OP_PP = 0x02, OP_PE = 0xDB, OP_SE = 0xD8, OP_DP = 0xB9, OP_RDP = 0xAB, OP_RDID = 0x9F };

u8 firmware_spi_byte(Nds *s, int pos, u8 b, int last) {
    Firmware *fw = &s->fw;
    u8 out = 0;
    if (pos == 0) { fw->cmd = b; fw->addr = 0; }
    else if (fw->asleep && fw->cmd != OP_RDP) return 0;
    switch (fw->cmd) {
    case OP_RDP: if (pos == 0) fw->asleep = 0; break;
    case OP_DP: if (pos == 0) fw->asleep = 1; break;
    case OP_WREN: if (pos == 0) fw->wel = 1; break;
    case OP_WRDI: if (pos == 0) fw->wel = 0; break;
    case OP_RDID: if (pos) out = (u8[]){0x20, 0x40, 0x12}[(pos - 1) % 3]; break;    /* ST M45PE20 */
    case OP_RDSR: if (pos) out = (u8)(fw->wel << 1); break;     /* writes and erases take no time: WIP stays 0 */
    case OP_READ: case OP_FAST: {
        int skip = fw->cmd == OP_FAST;
        if (pos >= 1 && pos <= 3) fw->addr = (fw->addr << 8) | b;
        else if (pos > 3 + skip) out = fw->data[fw->addr++ & (FW_SIZE - 1)];
        break;
    }
    case OP_PW: case OP_PP: case OP_PE: case OP_SE:
        if (pos >= 1 && pos <= 3) fw->addr = (fw->addr << 8) | b;
        else if (pos > 3 && (fw->cmd == OP_PW || fw->cmd == OP_PP)) {
            /* programming stays inside the page, wrapping at its end; PW erases the page first
             * (the bytes not sent keep their values), PP can only clear bits */
            u32 a = fw->addr & (FW_SIZE - 1), page = a & ~0xFFu;
            if (fw->wel) fw->data[a] = fw->cmd == OP_PW ? b : (u8)(fw->data[a] & b);
            fw->addr = page | ((a + 1) & 0xFF);
        }
        if (last && pos >= 3) {                 /* chip select rises: erases run, the latch clears */
            u32 a = fw->addr & (FW_SIZE - 1);
            if (fw->wel && fw->cmd == OP_PE) memset(&fw->data[a & ~0xFFu], 0xFF, 0x100);
            if (fw->wel && fw->cmd == OP_SE) memset(&fw->data[a & ~0xFFFFu], 0xFF, 0x10000);
            fw->wel = 0;
        }
        break;
    default: break;
    }
    return out;
}
