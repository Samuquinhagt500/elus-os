ENTRY(_start)
SECTIONS
{
    /* Kernel carregado em 1 MB (0x100000) em modo longo (64-bit) */
    . = 0x100000;
    .text  : { *(.text) *(.text.*) }
    .rdata : { *(.rdata) *(.rdata.*) *(.rodata) *(.rodata.*) }
    .data  : { *(.data) *(.data.*) }
    __bss_start = .;
    .bss   : { *(.bss) *(.bss.*) *(COMMON) }
    __bss_end = .;
    /DISCARD/ : { *(.eh_frame) *(.comment) *(.note*) *(.drectve) *(.pdata) *(.xdata) }
}
