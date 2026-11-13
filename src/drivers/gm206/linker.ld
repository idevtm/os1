OUTPUT_FORMAT(elf64-x86-64)
ENTRY(os1_driver_init)
PHDRS { text PT_LOAD FLAGS(5); rodata PT_LOAD FLAGS(4); }
SECTIONS
{
    . = 0xffffffffa0040000;
    .text : { *(.text .text.*) } :text
    . = ALIGN(4096);
    .rodata : { *(.rodata .rodata.*) KEEP(*(.os1_driver)) } :rodata
    . = ALIGN(4096);
    /DISCARD/ : { *(.comment .note* .eh_frame*) }
}
