ENTRY(_start)
SECTIONS
{
    . = 0x10000;
    .text  : { *(.text) *(.text.*) }
    .rdata : { *(.rdata) *(.rdata.*) *(.rodata) *(.rodata.*) }
    .data  : { *(.data) *(.data.*) }
    __bss_start = .;
    .bss   : { *(.bss) *(.bss.*) *(COMMON) }
    __bss_end = .;
    /DISCARD/ : { *(.eh_frame) *(.comment) *(.note*) *(.drectve) *(.pdata) *(.xdata) }
}
