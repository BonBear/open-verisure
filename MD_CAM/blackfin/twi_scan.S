/*
 * twi_scan.S - Scan the Blackfin TWI (I2C/SCCB) bus for devices, e.g. the camera sensor.
 *
 * For every 7-bit address 0x08..0x77 a 1-byte master read is issued (read only, so
 * nothing is written to any device). Results go to L1 data bank A:
 *
 * 0xFF800000  magic 0x7171CA11
 * 0xFF800004  addresses done
 * 0xFF800008  number of devices that acknowledged
 * 0xFF800100  128 bytes: per address 0 = no ack, 1 = ack, 2 = other error, 3 = timeout
 * 0xFF800180  128 bytes: byte read from each acking address
 *
 * TWI timing: PRESCALE 3 (assumes SCLK ~27 MHz -> ~9 MHz reference), CLKDIV 0x3232
 * (~90 kHz SCL; slower if SCLK is lower).
 */
    .text
    .global _start
_start:
    P0.L = 0x0000;              /* L1 data bank A */
    P0.H = 0xFF80;
    P1.L = 0x1400;              /* TWI base */
    P1.H = 0xFFC0;
    P2.L = 0x0100;              /* status table */
    P2.H = 0xFF80;
    P3.L = 0x0180;              /* data table */
    P3.H = 0xFF80;

    R0.L = 0xCA11;
    R0.H = 0x7171;
    [P0] = R0;
    R0 = 0;
    [P0 + 4] = R0;
    [P0 + 8] = R0;
    R1 = 0;                     /* clear both tables */
    P4 = 64;
    P5 = P2;
    LSETUP(.Lclr, .Lclr) LC0 = P4;
.Lclr: [P5++] = R1;

    R0 = 0;
    W[P1 + 0x04] = R0;          /* TWI off */
    SSYNC;
    R0 = 0x3232 (Z);
    W[P1 + 0x00] = R0;          /* CLKDIV */
    R0 = 0x0083 (Z);
    W[P1 + 0x04] = R0;          /* TWI_ENA | PRESCALE 3 */
    R0 = 0;
    W[P1 + 0x24] = R0;          /* no interrupts */
    SSYNC;

    R7 = 0x08 (Z);              /* address */
    R6 = 0;                     /* ack count */
.Laddr:
    R0 = 0x0003 (Z);
    W[P1 + 0x28] = R0;          /* flush FIFOs */
    SSYNC;
    R0 = 0;
    W[P1 + 0x28] = R0;
    R0 = 0x00FF (Z);
    W[P1 + 0x20] = R0;          /* clear INT_STAT */
    R0 = 0x003E (Z);
    W[P1 + 0x18] = R0;          /* clear MASTER_STAT errors */
    W[P1 + 0x1C] = R7;          /* MASTER_ADDR */
    SSYNC;
    R0 = 0x0045 (Z);            /* MEN | MDIR (read) | DCNT = 1 */
    W[P1 + 0x14] = R0;
    SSYNC;

    R2.L = 0x0000;              /* timeout ~ a few ms */
    R2.H = 0x0004;
.Lpoll:
    R0 = W[P1 + 0x20] (Z);
    R3 = 0x0030 (Z);            /* MCOMP | MERR */
    R3 = R0 & R3;
    CC = R3 == 0;
    IF !CC JUMP .Ldone;
    R2 += -1;
    CC = R2 == 0;
    IF !CC JUMP .Lpoll;
    R4 = 3;                     /* timeout */
    R0 = 0;
    W[P1 + 0x14] = R0;          /* drop master mode */
    JUMP .Lstore;
.Ldone:
    CC = BITTST(R0, 5);         /* MERR */
    IF CC JUMP .Lerr;
    R4 = 1;                     /* ack */
    R6 += 1;
    R0 = W[P1 + 0x88] (Z);      /* RCV_DATA8 */
    P5 = R7;
    P5 = P5 + P3;
    B[P5] = R0;
    JUMP .Lstore;
.Lerr:
    R0 = W[P1 + 0x18] (Z);      /* MASTER_STAT */
    R4 = 2;
    CC = BITTST(R0, 2);         /* ANAK */
    IF !CC JUMP .Lstore;
    R4 = 0;
.Lstore:
    P5 = R7;
    P5 = P5 + P2;
    B[P5] = R4;
    [P0 + 8] = R6;
    R0 = R7;
    [P0 + 4] = R0;
    R0 = 0;
    W[P1 + 0x14] = R0;          /* MASTER_CTL off between addresses */
    SSYNC;
    R2 = 0x4000 (Z);            /* short gap (bus free time) */
.Lgap:
    R2 += -1;
    CC = R2 == 0;
    IF !CC JUMP .Lgap;
    R7 += 1;
    R0 = 0x78 (Z);
    CC = R7 < R0;
    IF CC JUMP .Laddr;
.Lend:
    JUMP .Lend;
