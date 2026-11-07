OUTPUT_ARCH(arm)
ENTRY(_start)
SECTIONS
{
    . = 0x02380000;
    .text : {
        *(.crt0)
        *(.text .text.*)
        *(.glue_7 .glue_7t .vfp11_veneer .v4_bx)
        *(.rodata .rodata.*)
        *(.data .data.*)
        . = ALIGN(4);
    }
    .ARM.exidx : { *(.ARM.exidx*) }
    .bss 0x03800000 (NOLOAD) : {
        __bss_start = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(4);
        __bss_end = .;
    }
    /DISCARD/ : { *(.ARM.attributes) *(.comment) }
}
