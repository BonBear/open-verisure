/*
 * sw_spi_slave.S - Software SPI slave on PH0..PH3 for the CC1110 link (mode 0, MSB first).
 *
 * The SPI1 controller does not respond as slave on this board, but the pins carry the
 * CC1110 transfer correctly (see ph_sniff.S), so this polls them instead:
 *   PH0 SS (input, low = selected; follows CC1110 P1_2 = 1)
 *   PH1 SCK (input, from CC1110 P1_4)
 *   PH2 MISO (output, to CC1110 P1_3)
 *   PH3 MOSI (input, from CC1110 P1_5)
 * Each received byte is answered in the next transfer with its complement, so the
 * CC1110 can check the return path: send 0x5A, get ~previous byte back.
 *
 * 0xFF800000  magic 0x5B1F5A7E
 * 0xFF800004  number of bytes received
 * 0xFF800008  last byte received
 * 0xFF80000C  byte being sent
 * 0xFF800010  last 64 received bytes (ring, index = count & 63)
 */
    .text
    .global _start
_start:
    P0.L = 0x0000;              /* L1 data bank A */
    P0.H = 0xFF80;
    P1.L = 0x1700;              /* PORTHIO */
    P1.H = 0xFFC0;
    P4.L = 0x0010;              /* ring base */
    P4.H = 0xFF80;
    P5.L = 0x3208;              /* PORTH_FER */
    P5.H = 0xFFC0;

    R0 = 0;
    W[P5] = R0;                 /* PH0..PH3 as GPIO */
    R0 = 0x0004 (Z);
    W[P1 + 0x30] = R0;          /* DIR: PH2 output */
    R0 = 0x000B (Z);
    W[P1 + 0x40] = R0;          /* INEN: PH0, PH1, PH3 */
    SSYNC;

    R0.L = 0x5A7E;
    R0.H = 0x5B1F;
    [P0] = R0;
    R7 = 0;                     /* count */
    [P0 + 4] = R7;
    R6 = 0xA5 (Z);              /* first byte to send */
    [P0 + 12] = R6;
    R1 = 0x0004 (Z);            /* PH2 bit for SET/CLEAR */

.Lidle:                         /* wait for SS high (deselected) */
    R0 = W[P1] (Z);
    CC = BITTST(R0, 0);
    IF !CC JUMP .Lidle;
.Lwait_sel:                     /* wait for SS low */
    R0 = W[P1] (Z);
    CC = BITTST(R0, 0);
    IF CC JUMP .Lwait_sel;

    R4 = R6;                    /* shift-out copy */
    R5 = 0;                     /* shift-in */
    R3 = 8;
.Lbit:
    CC = BITTST(R4, 7);         /* put next MISO bit before the rising edge */
    IF CC JUMP .Lset;
    W[P1 + 4] = R1;             /* CLEAR PH2 */
    JUMP .Lrise;
.Lset:
    W[P1 + 8] = R1;             /* SET PH2 */
.Lrise:                         /* wait SCK high, abort on SS high */
    R0 = W[P1] (Z);
    CC = BITTST(R0, 0);
    IF CC JUMP .Lidle;
    CC = BITTST(R0, 1);
    IF !CC JUMP .Lrise;
    R5 <<= 1;                   /* sample MOSI */
    CC = BITTST(R0, 3);
    IF !CC JUMP .Lfall;
    BITSET(R5, 0);
.Lfall:                         /* wait SCK low, abort on SS high */
    R0 = W[P1] (Z);
    CC = BITTST(R0, 0);
    IF CC JUMP .Lidle;
    CC = BITTST(R0, 1);
    IF CC JUMP .Lfall;
    R4 <<= 1;
    R3 += -1;
    CC = R3 == 0;
    IF !CC JUMP .Lbit;

    /* byte complete */
    R7 += 1;
    [P0 + 4] = R7;
    [P0 + 8] = R5;
    R0 = 63 (Z);
    R0 = R7 & R0;
    P3 = R0;
    P3 = P3 + P4;
    B[P3] = R5;
    R0 = 0xFF (Z);
    R6 = R5 ^ R0;               /* answer next transfer with the complement */
    [P0 + 12] = R6;
    JUMP .Lidle;
