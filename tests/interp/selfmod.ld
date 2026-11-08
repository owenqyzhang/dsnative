/* The self-modifying-code test program: main code, a patchable function (the dispatcher
 * checks the bytes of code in an overlay-kind segment), and the code that is copied to RAM
 * at run time and never compiled. */
SECTIONS {
    . = 0x02000000;
    .text : { *(.text) }
    . = 0x02001000;
    .ovl : { *(.ovl) }
    . = 0x02001800;
    .data : { *(.ramcode_a) *(.ramcode_t) *(.ramcode_f) *(.data) }
}
