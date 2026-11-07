/* dsnative runtime: the ARM7's sound (GBATEK "DS Sound"): 16 channels (PCM8, PCM16,
 * IMA-ADPCM; PSG square waves on 8-13, noise on 14-15), the two capture units and the mixer,
 * at 0x04000400-0x0400051F.
 *
 * The sound runs lazily: to the machine's time at the end of each scanline and before each
 * access to its registers, so the busy bits the SDK polls are current to the CPU's time
 * slice. A channel's output only changes when its timer overflows, so between the instants
 * something looks at the outputs (a mixed frame, a capture) each channel is stepped alone.
 *
 * Frames are mixed at SOUND_RATE (32768 Hz) of emulated time by sampling the mixer, as
 * nds_audio_take promises; the hardware's PWM output runs at bus / 1024 (32728.5 Hz). */
#include "nds.h"

#define BUS_HZ 33513982u
#define BUSY 0x80000000u
#define HOLD 0x8000u
#define MASTER_ON 0x8000u

enum { PCM8, PCM16, ADPCM, PSG };

/* bus cycles per sample: the timers count at the bus clock / 2 from TMR to overflow */
static u32 period(u16 tmr) { return 2u * (0x10000u - tmr); }
static u64 frame_time(u64 n) { return n * BUS_HZ / SOUND_RATE; }

/* The channels and capture units go through the ARM7's memory map, but not into I/O: a
 * read there would come back into the sound. */
static u32 mem8(Nds *s, u32 a) { return (a >> 24) == 4 ? 0 : rd8(&s->arm7, a); }
static u32 mem16(Nds *s, u32 a) { return (a >> 24) == 4 ? 0 : rd16(&s->arm7, a); }
static void mem_write32(Nds *s, u32 a, u32 v) { if ((a >> 24) != 4) wr32(&s->arm7, a, v); }

/* ---- channels ------------------------------------------------------------------------- */
/* 89 steps; the hardware's table reads 0 past them (an out-of-range header index) */
static const u16 adpcm_step[128] = {
    0x0007, 0x0008, 0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x000E, 0x0010, 0x0011, 0x0013, 0x0015,
    0x0017, 0x0019, 0x001C, 0x001F, 0x0022, 0x0025, 0x0029, 0x002D, 0x0032, 0x0037, 0x003C, 0x0042,
    0x0049, 0x0050, 0x0058, 0x0061, 0x006B, 0x0076, 0x0082, 0x008F, 0x009D, 0x00AD, 0x00BE, 0x00D1,
    0x00E6, 0x00FD, 0x0117, 0x0133, 0x0151, 0x0173, 0x0198, 0x01C1, 0x01EE, 0x0220, 0x0256, 0x0292,
    0x02D4, 0x031C, 0x036C, 0x03C3, 0x0424, 0x048E, 0x0502, 0x0583, 0x0610, 0x06AB, 0x0756, 0x0812,
    0x08E0, 0x09C3, 0x0ABD, 0x0BD0, 0x0CFF, 0x0E4C, 0x0FBA, 0x114C, 0x1307, 0x14EE, 0x1706, 0x1954,
    0x1BDC, 0x1EA5, 0x21B6, 0x2515, 0x28CA, 0x2CDF, 0x315B, 0x364B, 0x3BB9, 0x41B2, 0x4844, 0x4F7E,
    0x5771, 0x602F, 0x69CE, 0x7462, 0x7FFF,
};
static const s8 adpcm_index[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

/* The hardware's arithmetic, with its rounding (each term rounded down on its own) and its
 * clipping (to +-7FFFh, which leaves a -8000h header value alone until it goes down). */
static void adpcm_decode(SoundChannel *c, u32 nib) {
    s32 t = adpcm_step[c->idx], diff = t >> 3;
    if (nib & 1) diff += t >> 2;
    if (nib & 2) diff += t >> 1;
    if (nib & 4) diff += t;
    if (nib & 8) { c->pcm -= diff; if (c->pcm < -0x7FFF) c->pcm = -0x7FFF; }
    else { c->pcm += diff; if (c->pcm > 0x7FFF) c->pcm = 0x7FFF; }
    c->idx += adpcm_index[nib & 7];
    if (c->idx < 0) c->idx = 0;
    if (c->idx > 88) c->idx = 88;
}

static void channel_start(SoundChannel *c) {
    int fmt = (c->cnt >> 29) & 3;
    c->active = 1;
    /* Hold keeps the old level through the first period of the start delay, no longer */
    if (!(c->held && (c->cnt & HOLD))) c->sample = 0;
    c->held = 0;
    c->src = c->sad;
    /* the first sample comes after 1 period (PSG, noise), 3 (PCM) or 3 + 8 for the ADPCM header */
    c->pos = fmt == PSG ? -1 : fmt == ADPCM ? -11 : -3;
    c->left = period(c->tmr);
    c->lfsr = 0x7FFF;
    c->loop_pcm = 0; c->loop_idx = 0;
}

/* A timer overflow: the next sample. */
static void channel_step(Nds *s, SoundChannel *c, int n) {
    int fmt = (c->cnt >> 29) & 3;
    s32 prev = c->pos++;
    if (c->pos < 0) { c->sample = 0; return; }
    if (fmt == PSG) {
        c->pos &= 7;
        if (n >= 14) {
            /* noise: X = X >> 1, and on a carry LOW and X ^= 6000h, else HIGH */
            int carry = c->lfsr & 1;
            c->lfsr >>= 1;
            if (carry) c->lfsr ^= 0x6000;
            c->sample = carry ? -0x7FFF : 0x7FFF;
        } else if (n >= 8) {
            /* eight steps from the start of the LOW part: duty N is HIGH for N + 1, but 7 is silent LOW */
            u32 duty = (c->cnt >> 24) & 7;
            c->sample = duty != 7 && (u32)c->pos >= 7 - duty ? 0x7FFF : -0x7FFF;
        } else {
            c->sample = 0;      /* no PSG on channels 0-7 */
        }
        return;
    }
    u32 words = (u32)c->pnt + c->len;
    if (words < 4) { c->sample = 0; return; }       /* too short: the hardware hangs, busy and silent */
    s32 loop, end;
    if (fmt == PCM8) { loop = 4 * c->pnt; end = (s32)(4 * words); }
    else if (fmt == PCM16) { loop = 2 * c->pnt; end = (s32)(2 * words); }
    else { loop = c->pnt ? 8 * (c->pnt - 1) : 0; end = (s32)(8 * (words - 1)); }   /* the header is a word */
    u32 rep = (c->cnt >> 27) & 3;
    if (c->pos >= end) {
        if (rep & 1) {
            c->pos = loop;
            if (fmt == ADPCM) { c->pcm = c->loop_pcm; c->idx = c->loop_idx; }
        } else if (rep & 2) {
            /* one-shot: the last sample's period is over */
            c->active = 0; c->held = 1; c->cnt &= ~BUSY;
            return;
        }
        /* manual: plays on past the end until the CPU stops it */
    }
    switch (fmt) {
    case PCM8: c->sample = (s32)(s8)mem8(s, c->src + (u32)c->pos) * 256; break;
    case PCM16: c->sample = (s16)mem16(s, c->src + 2 * (u32)c->pos); break;
    case ADPCM: {
        if (prev < 0) {
            u32 h = mem16(s, c->src) | mem16(s, c->src + 2) << 16;
            c->pcm = (s16)h;
            c->idx = (h >> 16) & 0x7F;
        }
        if (c->pos == loop) { c->loop_pcm = c->pcm; c->loop_idx = c->idx; }
        u32 b = mem8(s, c->src + 4 + ((u32)c->pos >> 1));
        adpcm_decode(c, (c->pos & 1) ? b >> 4 : b & 15);
        c->sample = c->pcm;
        break;
    }
    }
    /* a one-shot reads not busy from the start of its last sample's period */
    if (rep == 2 && c->pos == end - 1) c->cnt &= ~BUSY;
}

static s32 channel_output(const SoundChannel *c) {
    return (c->active || (c->held && (c->cnt & HOLD))) ? c->sample : 0;
}

s32 sound_channel_output(const Sound *d, int n) { return channel_output(&d->ch[n & 15]); }

/* After the volume divider and factor: 16.11 fixed point. */
static s64 channel_vol(const SoundChannel *c) {
    static const u8 div_shift[4] = {0, 1, 2, 4};
    u32 mul = c->cnt & 0x7F;
    if (mul == 127) mul = 128;      /* 0-126 and 127 mean N = 0-126 and 128 */
    return (((s64)channel_output(c) * 16) >> div_shift[(c->cnt >> 8) & 3]) * mul;
}

/* ---- the mixer ------------------------------------------------------------------------ */
typedef struct {
    s64 vol[16];                /* each channel after its volume, 16.11 */
    s64 l, r;                   /* the mixers, 20.8 */
    s64 ch1[2], ch3[2];         /* channels 1 and 3 after panning, for SOUNDCNT's output selection */
} Mix;

static void mix(Sound *d, Mix *m) {
    memset(m, 0, sizeof *m);
    for (int n = 0; n < 16; n++) m->vol[n] = channel_vol(&d->ch[n]);
    for (int n = 0; n < 16; n++) {
        s64 v = m->vol[n];
        if (n < 4 && (d->cap[n >> 1].cnt & 0x81) == 0x81) {
            /* addition: channel 1 (3) joins channel 0 (2) before its panning, muted when
             * channel 0 (2) is off or SOUNDCNT keeps channel 1 (3) from the mixer */
            if (n & 1) continue;
            if (d->ch[n].active && !(d->soundcnt & (n ? 0x2000 : 0x1000))) v += m->vol[n + 1];
        }
        u32 p = (d->ch[n].cnt >> 16) & 0x7F;
        if (p == 127) p = 128;
        s64 l = (v * (128 - p)) >> 10, r = (v * p) >> 10;   /* 16.18, rounded down to 16.8 */
        if (n == 1) { m->ch1[0] = l; m->ch1[1] = r; if (d->soundcnt & 0x1000) continue; }
        if (n == 3) { m->ch3[0] = l; m->ch3[1] = r; if (d->soundcnt & 0x2000) continue; }
        m->l += l; m->r += r;
    }
}

/* SOUNDCNT's source for one speaker, master volume (N/128/64), bias and the 10-bit clip;
 * then back to signed 16 bits without the bias, as the speaker's coupling removes it. */
static s16 speaker(const Sound *d, const Mix *m, int right) {
    s64 mixer = right ? m->r : m->l, c1 = m->ch1[right], c3 = m->ch3[right];
    u32 sel = (d->soundcnt >> (right ? 10 : 8)) & 3;
    s64 v = sel == 0 ? mixer : sel == 1 ? c1 : sel == 2 ? c3 : c1 + c3;
    u32 mul = d->soundcnt & 0x7F;
    if (mul == 127) mul = 128;
    s64 o = ((v * mul) >> 21) + d->bias;
    if (o < 0) o = 0;
    if (o > 0x3FF) o = 0x3FF;
    s64 out = (o - d->bias) * 64;
    return (s16)(out > 0x7FFF ? 0x7FFF : out < -0x8000 ? -0x8000 : out);
}

static void put16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8 *p, u32 v) { put16(p, v); put16(p + 2, v >> 16); }

static void emit_frame(Sound *d) {
    s16 l = 0, r = 0;
    if (d->soundcnt & MASTER_ON) {
        Mix m;
        mix(d, &m);
        l = speaker(d, &m, 0); r = speaker(d, &m, 1);
    }
    if (d->ring_count == SOUND_RING) { d->ring_head = (d->ring_head + 1) % SOUND_RING; d->ring_count--; }  /* drop the oldest */
    u32 i = (d->ring_head + d->ring_count) % SOUND_RING;
    d->ring[i][0] = l; d->ring[i][1] = r;
    d->ring_count++;
    if (d->wav) {
        u8 b[4];
        put16(b, (u16)l); put16(b + 2, (u16)r);
        fwrite(b, 1, 4, d->wav);
        d->wav_frames++;
    }
}

/* ---- capture -------------------------------------------------------------------------- */
static void capture_start(Sound *d, int x) {
    SoundCapture *k = &d->cap[x];
    k->active = 1;
    k->pos = 0; k->word = 0; k->nbytes = 0;
    k->left = period(d->ch[2 * x + 1].tmr);
}

static void capture_tick(Nds *s, int x) {
    Sound *d = &s->sound;
    SoundCapture *k = &d->cap[x];
    Mix m;
    mix(d, &m);
    s64 v;      /* 16.8 */
    if (k->cnt & 2) {
        /* channel 0 (2) itself, unclipped, with the hardware's bugs: an added channel 1 (3)
         * overflows into the sign, and without addition two negative channels give -8000h */
        s64 va = m.vol[2 * x] >> 3, vb = m.vol[2 * x + 1] >> 3;
        if (k->cnt & 1) v = (d->ch[2 * x].active ? va : 0) + vb;
        else v = va < 0 && vb < 0 ? -0x800000 : va;
    } else {
        v = x ? m.r : m.l;
        if (v < -0x800000) v = -0x800000;
        if (v > 0x7FFFFF) v = 0x7FFFFF;
    }
    int pcm8 = k->cnt & 8;
    s64 i = v >> (pcm8 ? 16 : 8);
    if (v < 0 && (v & (pcm8 ? 0x8000 : 0x80))) i++;     /* negative halves round toward zero */
    k->word |= (pcm8 ? (u32)i & 0xFF : (u32)i & 0xFFFF) << (8 * k->nbytes);
    k->nbytes += pcm8 ? 1 : 2;
    if (k->nbytes == 4) {
        mem_write32(s, k->dad + k->pos, k->word);
        k->word = 0; k->nbytes = 0;
        k->pos += 4;
        if (k->pos >= 4u * (k->len ? k->len : 1)) {
            k->pos = 0;
            if (k->cnt & 4) { k->active = 0; k->cnt &= 0x7F; }   /* one-shot */
        }
    }
    if (k->active) k->left = period(d->ch[2 * x + 1].tmr);
}

/* ---- running -------------------------------------------------------------------------- */
static void advance(Nds *s, u64 dt) {
    Sound *d = &s->sound;
    if (!(d->soundcnt & MASTER_ON) || !dt) return;      /* the channels wait while the master is off */
    for (int n = 0; n < 16; n++) {
        SoundChannel *c = &d->ch[n];
        u64 t = dt;
        while (c->active && t >= c->left) {
            t -= c->left;
            c->left = period(c->tmr);
            channel_step(s, c, n);
        }
        if (c->active) c->left -= (u32)t;
    }
    for (int x = 0; x < 2; x++)
        if (d->cap[x].active) d->cap[x].left -= (u32)dt;
}

static void run_to(Nds *s, u64 until) {
    Sound *d = &s->sound;
    while (d->time < until) {
        int on = (d->soundcnt & MASTER_ON) != 0;
        u64 frame = frame_time(d->frames + 1), next = frame;
        for (int x = 0; x < 2; x++)
            if (on && d->cap[x].active && d->time + d->cap[x].left < next) next = d->time + d->cap[x].left;
        if (next > until) { advance(s, until - d->time); d->time = until; return; }
        advance(s, next - d->time);
        d->time = next;
        for (int x = 0; x < 2; x++)
            if (on && d->cap[x].active && !d->cap[x].left) capture_tick(s, x);
        if (next == frame) { emit_frame(d); d->frames++; }
    }
}

/* ---- registers ------------------------------------------------------------------------ */
/* as the CPU reads them: SAD, TMR, PNT, LEN and the capture lengths are write-only */
static u32 reg_read(const Sound *d, u32 base) {
    if (base < 0x500) return (base & 0xF) == 0 ? d->ch[(base - 0x400) >> 4].cnt : 0;
    switch (base) {
    case 0x500: return d->soundcnt;
    case 0x504: return d->bias;
    case 0x508: return d->cap[0].cnt | (u32)d->cap[1].cnt << 8;
    case 0x510: return d->cap[0].dad;
    case 0x518: return d->cap[1].dad;
    }
    return 0;
}

/* as written, to merge an 8- or 16-bit write into */
static u32 reg_raw(const Sound *d, u32 base) {
    if (base < 0x500) {
        const SoundChannel *c = &d->ch[(base - 0x400) >> 4];
        switch (base & 0xF) {
        case 0x0: return c->cnt;
        case 0x4: return c->sad;
        case 0x8: return c->tmr | (u32)c->pnt << 16;
        default: return c->len;
        }
    }
    if (base == 0x514) return d->cap[0].len;
    if (base == 0x51C) return d->cap[1].len;
    return reg_read(d, base);
}

static void reg_write(Sound *d, u32 base, u32 v) {
    if (base < 0x500) {
        SoundChannel *c = &d->ch[(base - 0x400) >> 4];
        switch (base & 0xF) {
        case 0x0: {
            u32 old = c->cnt;
            c->cnt = v & 0xFF7F837Fu;
            if (!(old & BUSY) && (c->cnt & BUSY)) channel_start(c);
            else if ((old & BUSY) && !(c->cnt & BUSY)) { c->active = 0; c->held = 0; }
            break;
        }
        case 0x4: c->sad = v & 0x07FFFFFCu; break;
        case 0x8: c->tmr = (u16)v; c->pnt = (u16)(v >> 16); break;
        default: c->len = v & 0x3FFFFF; break;
        }
        return;
    }
    switch (base) {
    case 0x500: d->soundcnt = (u16)(v & 0xBF7F); break;
    case 0x504: d->bias = (u16)(v & 0x3FF); break;
    case 0x508:
        for (int x = 0; x < 2; x++) {
            SoundCapture *k = &d->cap[x];
            u8 old = k->cnt;
            k->cnt = (u8)((v >> (8 * x)) & 0x8F);
            if (!(old & 0x80) && (k->cnt & 0x80)) capture_start(d, x);
            else if (!(k->cnt & 0x80)) k->active = 0;
            /* a new format on a running capture drops the half-collected word: left, nbytes would
             * skip past 4 for good (and overshift) */
            else if ((old ^ k->cnt) & 8) { k->word = 0; k->nbytes = 0; }
        }
        break;
    case 0x510: d->cap[0].dad = v & 0x07FFFFFCu; break;
    case 0x514: d->cap[0].len = (u16)v; break;
    case 0x518: d->cap[1].dad = v & 0x07FFFFFCu; break;
    case 0x51C: d->cap[1].len = (u16)v; break;
    }
}

static int is_sound(int cpu, u32 off) { return cpu == 1 && off >= 0x400 && off < 0x520; }

int sound_io_read(Nds *s, int cpu, u32 off, int size, u32 *v) {
    if (!is_sound(cpu, off)) return 0;
    run_to(s, s->time);
    u32 w = reg_read(&s->sound, off & ~3u) >> ((off & 3) * 8);
    *v = size == 4 ? w : w & ((1u << (8 * size)) - 1);
    return 1;
}

int sound_io_write(Nds *s, int cpu, u32 off, int size, u32 v) {
    if (!is_sound(cpu, off)) return 0;
    run_to(s, s->time);
    u32 base = off & ~3u, sh = (off & 3) * 8;
    u32 mask = size == 4 ? 0xFFFFFFFFu : ((1u << (8 * size)) - 1) << sh;
    reg_write(&s->sound, base, (reg_raw(&s->sound, base) & ~mask) | ((v << sh) & mask));
    return 1;
}

/* ---- the machine's hooks -------------------------------------------------------------- */
void sound_init(Nds *s) {
    s->sound.bias = 0x200;      /* what the firmware leaves */
}

void sound_line(Nds *s, u32 line) { (void)line; run_to(s, s->time); }

static void wav_header(FILE *f, u32 frames) {
    u8 h[44];
    memcpy(h, "RIFF", 4); put32(h + 4, 36 + 4 * frames);
    memcpy(h + 8, "WAVEfmt ", 8); put32(h + 16, 16);
    put16(h + 20, 1); put16(h + 22, 2);                         /* PCM, stereo */
    put32(h + 24, SOUND_RATE); put32(h + 28, SOUND_RATE * 4);
    put16(h + 32, 4); put16(h + 34, 16);
    memcpy(h + 36, "data", 4); put32(h + 40, 4 * frames);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, f);
}

int sound_option(Nds *s, const char *name, const char *value) {
    if (strcmp(name, "--wav")) return 0;
    Sound *d = &s->sound;
    if (d->wav) fclose(d->wav);
    d->wav_frames = 0;
    d->wav = fopen(value, "wb");
    if (d->wav) wav_header(d->wav, 0);      /* the sizes are filled in at shutdown */
    else fprintf(stderr, "cannot write %s\n", value);
    return 1;
}

void sound_shutdown(Nds *s) {
    Sound *d = &s->sound;
    run_to(s, s->time);
    if (d->wav) {
        wav_header(d->wav, d->wav_frames);
        fclose(d->wav);
        d->wav = 0;
    }
}

u32 nds_audio_take(Nds *s, s16 *out, u32 max_frames) {
    Sound *d = &s->sound;
    u32 n = d->ring_count < max_frames ? d->ring_count : max_frames;
    for (u32 i = 0; i < n; i++) {
        u32 k = (d->ring_head + i) % SOUND_RING;
        out[2 * i] = d->ring[k][0];
        out[2 * i + 1] = d->ring[k][1];
    }
    d->ring_head = (d->ring_head + n) % SOUND_RING;
    d->ring_count -= n;
    return n;
}
