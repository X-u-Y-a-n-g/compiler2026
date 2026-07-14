OUTPUT_ARCH(riscv)
ENTRY(_start)

SECTIONS
{
  . = 0x90000000;

  .text : ALIGN(16) {
    KEEP(*(.text.start))
    *(.text .text.*)
  }

  .rodata : ALIGN(16) {
    *(.rodata .rodata.*)
  }

  .data : ALIGN(16) {
    *(.data .data.*)
    *(.sdata .sdata.*)
  }

  __global_pointer$ = . + 0x800;

  .bss (NOLOAD) : ALIGN(16) {
    __bss_start = .;
    *(.sbss .sbss.*)
    *(.bss .bss.*)
    *(COMMON)
    . = ALIGN(8);
    __bss_end = .;
  }
}
