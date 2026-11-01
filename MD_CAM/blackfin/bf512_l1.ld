/* Place everything in ADSP-BF512 L1 instruction SRAM bank A (16 KB). */
ENTRY(_start)

MEMORY
{
    L1_CODE : ORIGIN = 0xFFA00000, LENGTH = 0x4000
}

SECTIONS
{
    .text : { *(.text .text.*) } > L1_CODE
}
