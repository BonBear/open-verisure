/*
 * pin_activity.S - Count level changes on every PORTF and PORTG pin (all as GPIO inputs)
 * to find which Blackfin pins carry the camera's PCLK, HREF, VSYNC and data.
 *
 * Sets PORTF/PORTG FER = 0, DIR = 0, INEN = 0xFFFF (inputs only, drives nothing).
 * 0xFF800000  magic 0xAC71F17E when done
 * 0xFF800004  samples taken
 * 0xFF800008  CYCLES at start, 0xFF80000C  CYCLES at end
 * 0xFF800010  32 counters: PF0..PF15 then PG0..PG15 (changes seen per pin)
 * 0xFF800090  last PORTF, 0xFF800094 last PORTG
 */
    .text
    .global _start
_start:
    P0.L = 0x0000;
    P0.H = 0xFF80;
    P1.L = 0x0700;              /* PORTFIO */
    P1.H = 0xFFC0;
    P2.L = 0x1500;              /* PORTGIO */
    P2.H = 0xFFC0;
    P3.L = 0x3200;              /* PORTF_FER, PORTG_FER at +4 */
    P3.H = 0xFFC0;

    R0 = 0;
    [P0] = R0;
    W[P3] = R0;
    W[P3 + 4] = R0;
    W[P1 + 0x30] = R0;          /* DIR = inputs */
    W[P2 + 0x30] = R0;
    R0 = -1;
    W[P1 + 0x40] = R0;          /* INEN all */
    W[P2 + 0x40] = R0;
    SSYNC;

    R0 = 0;                     /* clear counters */
    P4.L = 0x0010;
    P4.H = 0xFF80;
    P5 = 32;
    LSETUP(.Lclr, .Lclr) LC0 = P5;
.Lclr: [P4++] = R0;

    R0 = CYCLES;
    [P0 + 8] = R0;
    R6 = W[P1] (Z);             /* previous F | G << 16 */
    R1 = W[P2] (Z);
    R1 <<= 16;
    R6 = R6 | R1;
    R7.L = 0x0000;              /* samples to take: 0x00400000 */
    R7.H = 0x0040;
    R5 = 0;                     /* samples done */
.Lsample:
    R0 = W[P1] (Z);
    R1 = W[P2] (Z);
    R1 <<= 16;
    R0 = R0 | R1;
    R2 = R0 ^ R6;               /* changed bits */
    R6 = R0;
    CC = R2 == 0;
    IF CC JUMP .Lnext;
    P4.L = 0x0010;
    P4.H = 0xFF80;
    P5 = 32;
    LSETUP(.Lbit_s, .Lbit_e) LC0 = P5;
.Lbit_s:
    CC = BITTST(R2, 0);
    R3 = [P4];
    R4 = R3;
    R4 += 1;
    IF CC R3 = R4;
    [P4++] = R3;
.Lbit_e:
    R2 >>= 1;
.Lnext:
    R5 += 1;
    CC = R5 < R7;
    IF CC JUMP .Lsample;

    R0 = CYCLES;
    [P0 + 12] = R0;
    [P0 + 4] = R5;
    R0 = W[P1] (Z);
    [P0 + 0x90] = R0;
    R0 = W[P2] (Z);
    [P0 + 0x94] = R0;
    R0.L = 0xF17E;
    R0.H = 0xAC71;
    [P0] = R0;
.Lend:
    JUMP .Lend;
