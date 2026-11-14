/* ADSP-BF512 L1 layout for our own programs.
 * Code in L1 instruction SRAM bank A (16 KB). Constant tables and variables in L1 data
 * bank A at 0xFF800100..0xFF800FFF: .rodata placed in instruction memory cannot be read
 * with data loads. 0xFF800000..0xFF8000FF is left for the status words that the programs
 * and JTAG scripts use. Needs DMEM_CONTROL 0x1001 (data bank A all SRAM). .bss is not
 * zeroed by gdb's load: programs must initialise their variables in code. */
ENTRY(_start)

MEMORY
{
    L1_CODE : ORIGIN = 0xFFA00000, LENGTH = 0x4000
    L1_DATA : ORIGIN = 0xFF800100, LENGTH = 0x0F00
}

SECTIONS
{
    .text : { *(.text .text.*) } > L1_CODE
    .rodata : { *(.rodata .rodata.*) . = ALIGN(4); } > L1_DATA
    .data : ALIGN(4) { *(.data .data.*) . = ALIGN(4); } > L1_DATA
    .bss : ALIGN(4) { *(.bss .bss.* COMMON) . = ALIGN(4); } > L1_DATA
}
