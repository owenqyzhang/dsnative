/* Sound test, ARM9 side: runs the ARM7's (sound_7.c) and prints its results. */
#include "regs.h"

void sound_test9(void) {
    arm7_command(CMD7_SOUND, 0);
    u32 pcm = arm7_command(CMD7_SOUND, 1), psg = arm7_command(CMD7_SOUND, 2), cap = arm7_command(CMD7_SOUND, 3);
    /* PCM starts after 3 samples and a one-shot's busy bit clears at the start of its last
     * sample: (3 + 31) * 1024 bus cycles, give or take the time slices the ARM7 polls in */
    s32 d = (s32)pcm - (3 + 31) * 1024;
    dbg("sound_pcm "); dbg(d > -512 && d < 512 ? "ok" : "FAIL"); dbg("\n");
    dbg("sound_pcm_cycles "); dbg_hex(pcm); dbg("\n");
    dbg("sound_psg "); dbg_hex(psg); dbg("\n");
    dbg("sound_capture "); dbg_hex(cap); dbg("\n");
    /* the ARM7 BIOS's sound calls: GetPitchTable(2FFh) and (180h), GetVolumeTable(2D3h) and (0),
     * SoundBias(1) then SoundBias(0) */
    dbg("sound_bios ");
    dbg_hex(arm7_command(CMD7_SOUND, 0x10000 + 0x2FF)); dbg(" ");
    dbg_hex(arm7_command(CMD7_SOUND, 0x10000 + 0x180)); dbg(" ");
    dbg_hex(arm7_command(CMD7_SOUND, 0x20000 + 0x2D3)); dbg(" ");
    dbg_hex(arm7_command(CMD7_SOUND, 0x20000)); dbg(" ");
    dbg_hex(arm7_command(CMD7_SOUND, 0x30000 + 1)); dbg(" ");
    dbg_hex(arm7_command(CMD7_SOUND, 0x30000)); dbg("\n");
}
