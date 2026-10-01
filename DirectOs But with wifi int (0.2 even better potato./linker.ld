ENTRY(_start)
OUTPUT_FORMAT(elf32-i386)
OUTPUT_ARCH(i386)

SECTIONS
{
	. = 0x10000;
	.text : ALIGN(16) { KEEP(*(.text.entry)) *(.text*) }
	.rodata : ALIGN(16) { *(.rodata*) }
	.data : ALIGN(16) { *(.data*) }
	.bss (NOLOAD) : ALIGN(16)
	{
		__bss_start = .;
		*(COMMON)
		*(.bss*)
		__bss_end = .;
	}
	/DISCARD/ : { *(.eh_frame*) *(.comment*) *(.note*) }
}
