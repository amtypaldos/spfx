MEMORY { FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 1M
         RAM (rwx)  : ORIGIN = 0x20000000, LENGTH = 1M }
ENTRY(_start)
SECTIONS {
  .text : { *(.text*) *(.rodata*) } > FLASH
  .ARM.exidx : { *(.ARM.exidx*) } > FLASH
  .data : { *(.data*) } > RAM AT > FLASH
  .bss (NOLOAD) : { *(.bss*) *(COMMON) } > RAM
  /DISCARD/ : { *(.ARM.attributes) }
}
