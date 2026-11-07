OUTPUT_ARCH(arm)
ENTRY(_start)
SECTIONS
{
    . = 0x02000000;
    .text : {
        *(.crt0)
        *(.text .text.*)
        *(.glue_7 .glue_7t .vfp11_veneer .v4_bx)
        *(.rodata .rodata.*)
        *(.data .data.*)
        . = ALIGN(4);
    }
    .ARM.exidx : { *(.ARM.exidx*) }
    . = ALIGN(4);
    __autoload_start = .;
    .itcm 0x01FF8000 : AT(__autoload_start) {
        __itcm_start = .;
        *(.itcm .itcm.*)
        . = ALIGN(4);
        __itcm_end = .;
    }
    __autoload_list = __autoload_start + SIZEOF(.itcm);
    .autoload_list __autoload_list : AT(__autoload_list) {
        LONG(0x01FF8000) LONG(__itcm_end - __itcm_start) LONG(0)
    }
    __autoload_list_end = __autoload_list + 12;
    .bss 0x02200000 (NOLOAD) : {
        __bss_start = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(4);
        __bss_end = .;
    }
    /DISCARD/ : { *(.ARM.attributes) *(.comment) }
}
