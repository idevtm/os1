ENTRY(_start)

PHDRS
{
  text PT_LOAD FLAGS(5);
  rodata PT_LOAD FLAGS(4);
  data PT_LOAD FLAGS(6);
}

SECTIONS
{
  . = 0x0000008000400000;

  .text :
  {
    *(.text*)
  } :text

  . = ALIGN(4096);

  .rodata :
  {
    *(.rodata*)
  } :rodata

  .os.interface : { KEEP(*(.os.interface)) } :rodata
  .os.constructor : { KEEP(*(.os.constructor)) } :rodata

  . = ALIGN(4096);

  .data :
  {
    *(.data*)
  } :data

  . = ALIGN(4096);

  .bss :
  {
    *(.bss*)
    *(COMMON)
  } :data

  . = ALIGN(4096);
}
