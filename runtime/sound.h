/* dsnative runtime: the ARM7's sound channels, capture and mixer (0x04000400-0x0400051F). */
#ifndef SOUND_H
#define SOUND_H
#include <stdio.h>
#include "dsr.h"

struct Nds;

#define SOUND_RATE 32768            /* mixed frames per second of emulated time */
#define SOUND_RING 8192             /* frames kept for nds_audio_take (a quarter second) */

typedef struct {
    u32 cnt;                    /* SOUNDxCNT; bit 31 is the busy bit as the CPU reads it */
    u32 sad, len;               /* SOUNDxSAD, SOUNDxLEN as written */
    u16 tmr, pnt;               /* SOUNDxTMR, SOUNDxPNT */
    int active;                 /* its timer runs: started and not yet ended or stopped */
    int held;                   /* a one-shot ended with Hold set: its last sample stays out */
    u32 src;                    /* SAD latched at the start */
    s32 pos;                    /* sample index; negative during the start delay */
    u32 left;                   /* bus cycles to the next timer overflow */
    s32 sample;                 /* what it outputs, as PCM16 */
    s32 pcm, idx;               /* IMA-ADPCM state */
    s32 loop_pcm, loop_idx;     /* the ADPCM state at the loop start, first time through */
    u16 lfsr;                   /* noise */
} SoundChannel;

typedef struct {
    u8 cnt;                     /* SNDCAPxCNT; bit 7 is the busy bit */
    u32 dad;                    /* SNDCAPxDAD */
    u16 len;                    /* SNDCAPxLEN, in words */
    int active;
    u32 left;                   /* bus cycles to the next capture (channel 1/3's timer) */
    u32 pos;                    /* bytes written since the start or the last wrap */
    u32 word, nbytes;           /* the FIFO: samples collect into words */
} SoundCapture;

typedef struct {
    SoundChannel ch[16];
    SoundCapture cap[2];
    u16 soundcnt, bias;         /* SOUNDCNT, SOUNDBIAS */
    u64 time;                   /* bus cycles the sound has been run to */
    u64 frames;                 /* frames mixed so far */
    s16 ring[SOUND_RING][2];    /* mixed frames nds_audio_take hasn't taken */
    u32 ring_head, ring_count;
    FILE *wav;                  /* --wav: the recording, finished at shutdown */
    u32 wav_frames;
} Sound;

void sound_init(struct Nds *s);
/* An I/O access (offset from 0x04000000, size 1, 2 or 4) this subsystem handles: returns 1
 * (with *v for a read), or 0 to leave it to nds.c. */
int sound_io_read(struct Nds *s, int cpu, u32 off, int size, u32 *v);
int sound_io_write(struct Nds *s, int cpu, u32 off, int size, u32 v);
/* A command-line style option ("--name", "value"): returns 1 if it is this subsystem's.
 * --wav PATH records everything mixed to PATH. */
int sound_option(struct Nds *s, const char *name, const char *value);
void sound_shutdown(struct Nds *s);
/* Runs the sound to the machine's time (s->time). */
void sound_line(struct Nds *s, u32 line);
/* What channel n outputs now, as PCM16 before its volume and panning (tests, meters). */
s32 sound_channel_output(const Sound *d, int n);
/* nds_audio_take (nds.h) is defined in sound.c */
#endif
