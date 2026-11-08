/* dsnative app core: the runtime and one recompiled game behind a small C interface, for the
 * Swift app. Swift never sees Nds: its large fixed arrays (page tables, framebuffers) import
 * as enormous tuples.
 *
 * Threads: everything but dsn_core_audio_read runs on one thread at a time (the emulation
 * thread once the game runs). dsn_core_audio_read is for the audio render thread: it only
 * touches a lock-free ring the frame loop fills.
 *
 * One core per process: the dispatcher's caches are static.
 */
#ifndef DSN_CORE_H
#define DSN_CORE_H
#include <stdint.h>

#define DSN_SCREEN_W 256
#define DSN_SCREEN_H 192
#define DSN_AUDIO_RATE 32768
/* the DS frame: 263 lines of 2130 bus cycles at 33.513982 MHz (59.8261 Hz) */
#define DSN_BUS_HZ 33513982
#define DSN_FRAME_CYCLES (263 * 2130)

/* buttons, as nds.h's KEY_* (KEYINPUT bits 0-9, then X and Y) */
#define DSN_KEY_A      0x0001
#define DSN_KEY_B      0x0002
#define DSN_KEY_SELECT 0x0004
#define DSN_KEY_START  0x0008
#define DSN_KEY_RIGHT  0x0010
#define DSN_KEY_LEFT   0x0020
#define DSN_KEY_UP     0x0040
#define DSN_KEY_DOWN   0x0080
#define DSN_KEY_R      0x0100
#define DSN_KEY_L      0x0200
#define DSN_KEY_X      0x0400
#define DSN_KEY_Y      0x0800

typedef struct DsnCore DsnCore;

/* Loads the ROM and the decompressed ARM9 image the recompiler wrote (arm9.bin). Returns NULL
 * with a message in err when a file can't be read. */
DsnCore *dsn_core_create(const char *rom_path, const char *arm9_path, char *err, int err_len);
/* A runtime option ("--save", path), before dsn_core_boot: 0 when no subsystem knows it. */
int dsn_core_option(DsnCore *c, const char *name, const char *value);
/* Direct boot: the game's code tables, the binaries in RAM, what the firmware leaves. */
void dsn_core_boot(DsnCore *c);
/* Input for the next frame: KEY_* bits, and the touch screen in screen pixels. */
void dsn_core_set_input(DsnCore *c, uint16_t keys, int touch_down, int touch_x, int touch_y);
/* One frame (263 lines); then moves the frame's mixed sound into the audio ring. */
void dsn_core_run_frame(DsnCore *c);
/* Both screens, top then bottom, 256x192 each, 0xAARRGGBB. */
void dsn_core_copy_screens(const DsnCore *c, uint32_t *out);
uint32_t dsn_core_frame_count(const DsnCore *c);
/* Bit 0: the ARM9 stopped (code nothing was compiled for, an unknown BIOS call), bit 1: the ARM7. */
int dsn_core_stopped(const DsnCore *c);
/* A line of statistics, like dsn_headless --stats. */
void dsn_core_describe(const DsnCore *c, char *buf, int len);
/* Audio thread: up to `frames` stereo frames at DSN_AUDIO_RATE as floats, the rest of the
 * buffers zeroed. Returns how many were real. */
uint32_t dsn_core_audio_read(DsnCore *c, float *left, float *right, uint32_t frames);
/* Writes the save file now if the game changed it (the machine must not be running: call it
 * between frames, as the app goes to the background; iOS may kill it from there). 0 on success. */
int dsn_core_flush_save(DsnCore *c);
/* Lets the subsystems write what they keep (the save file). The core can't run afterwards. */
void dsn_core_shutdown(DsnCore *c);
#endif
