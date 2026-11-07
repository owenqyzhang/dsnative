/* Subsystem options: names no subsystem claims are refused. */
#include <stdio.h>
#include <stdlib.h>
#include "nds.h"

int main(void) {
    static u8 rom[0x1000];
    Nds *s = nds_create(rom, sizeof rom);
    if (nds_option(s, "--no-such-option", "1")) { printf("an unknown option was taken\n"); return 1; }
    nds_shutdown(s);
    return 0;
}
