/*
 * jtag_test.S - First ADSP-BF512 program loaded over JTAG (J5) with bfin-gdbproxy.
 *
 * Writes the magic word 0xB1A5C0DE to L1 data bank A (0xFF800000) and then counts
 * forever in 0xFF800004. jtag_run.py stops at _done (counter reached 0x100000) and
 * reads both words back: magic + counter prove that code we loaded actually runs.
 * Drives no pins, so it is safe whatever the Blackfin balls are wired to.
 */
    .text
    .global _start
    .global _done
_start:
    P0.L = 0x0000;              /* L1 data bank A */
    P0.H = 0xFF80;

    R0.L = 0xC0DE;
    R0.H = 0xB1A5;
    [P0] = R0;                  /* magic */

    R1 = 0;
    R2.L = 0x0000;
    R2.H = 0x0010;              /* 0x100000 */
.Lloop:
    R1 += 1;
    [P0 + 4] = R1;              /* counter */
    CC = R1 == R2;
    IF !CC JUMP .Lloop;
_done:
    NOP;
    JUMP .Lloop;
