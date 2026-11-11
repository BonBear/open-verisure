/*
 * ph_sniff.S - Sample PH0..PH3 (CC1110 SPI path: PH0 SS, PH1 SCK, PH2 MISO, PH3 MOSI)
 * in a tight loop and log every change to L1 data bank A, as a small logic analyzer.
 *
 * Set up over JTAG before running: PORTH_FER bits 0..3 = 0, PORTHIO_DIR = 0,
 * PORTHIO_INEN bits 0..3 = 1. Drives no pins.
 *
 * 0xFF800000  magic 0x5A1FF000
 * 0xFF800004  number of logged entries
 * 0xFF800008  loop counter (time base)
 * 0xFF800010  entries: bits 0..3 = PH3..PH0 level, bits 16..31 = loop counter low half
 */
    .text
    .global _start
_start:
    P0.L = 0x0000;              /* L1 data bank A */
    P0.H = 0xFF80;
    P1.L = 0x1700;              /* PORTHIO */
    P1.H = 0xFFC0;
    P2.L = 0x0010;              /* log write pointer */
    P2.H = 0xFF80;

    R0.L = 0xF000;
    R0.H = 0x5A1F;
    [P0] = R0;                  /* magic */
    R0 = 0;
    [P0 + 4] = R0;              /* entries */

    R1 = 0;                     /* loop counter */
    R3 = 0xFF (Z);              /* last value: impossible, forces a first entry */
    R4 = 0;                     /* entries */
    R5.L = 0x0FFC;              /* max entries (0xFF800010 + 4 * 0xFFC < 0xFF804000) */
    R5.H = 0x0000;
    R6 = 0x0F (Z);              /* PH0..PH3 mask */
.Lloop:
    R1 += 1;
    R2 = W[P1] (Z);
    R2 = R2 & R6;
    CC = R2 == R3;
    IF CC JUMP .Lloop;
    R3 = R2;
    CC = R4 < R5;
    IF !CC JUMP .Lloop;         /* buffer full: keep running, stop logging */
    R0 = R1 << 16;
    R0 = R0 | R2;
    [P2++] = R0;
    R4 += 1;
    [P0 + 4] = R4;
    [P0 + 8] = R1;
    JUMP .Lloop;
