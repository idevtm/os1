/* os1 EFI pre-loader: linked at 0x1000 (PE ImageBase 0) and converted to
   PE32+ by cmake/scripts/elf_to_pe.py. Code and read-only data come first,
   writable data starts on its own page. */
ENTRY(efi_main)

SECTIONS
{
	. = 0x1000;
	.text : { *(.text .text.*) }
	.rodata : ALIGN(16) { *(.rodata .rodata.*) }
	. = ALIGN(0x1000);
	.data : { *(.data .data.*) *(.bss .bss.*) *(COMMON) }

	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.interp) }
}
