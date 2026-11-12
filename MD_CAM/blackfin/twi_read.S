/*
 * twi_read.S - Read registers from a TWI (I2C/SCCB) device: write the register pointer
 * (1 byte, then STOP), then read N bytes. Only the pointer is written, no register data.
 *
 * Parameters (set in L1 over JTAG before running):
 * 0xFF800020  7-bit device address
 * 0xFF800024  first register (8-bit pointer)
 * 0xFF800028  bytes to read per register (1..8)
 * 0xFF80002C  number of consecutive registers to read (1..32)
 * 0xFF800030  pointer width: 2 = 16-bit register address (high byte first), else 8-bit
 * Results:
 * 0xFF800000  magic 0x7171EAD0 when done
 * 0xFF800004  status of the last transfer (0 ok, 1 pointer NAK, 2 read error, 3 timeout)
 * 0xFF800100  data, N bytes per register, register after register
 */
    .text
    .global _start
_start:
    P0.L = 0x0000;
    P0.H = 0xFF80;
    P1.L = 0x1400;              /* TWI base */
    P1.H = 0xFFC0;
    P2.L = 0x0100;              /* output */
    P2.H = 0xFF80;

    R0 = 0;
    [P0] = R0;
    W[P1 + 0x04] = R0;          /* TWI off */
    SSYNC;
    R0 = 0x3232 (Z);
    W[P1 + 0x00] = R0;          /* CLKDIV */
    R0 = 0x0083 (Z);
    W[P1 + 0x04] = R0;          /* TWI_ENA | PRESCALE 3 */
    R0 = 0;
    W[P1 + 0x24] = R0;
    SSYNC;

    R5 = [P0 + 0x24];           /* register */
    R6 = [P0 + 0x2C];           /* registers left */
.Lreg:
    /* --- pointer write --- */
    CALL .Lprep;
    R0 = [P0 + 0x20];
    W[P1 + 0x1C] = R0;          /* MASTER_ADDR */
    R0 = [P0 + 0x30];
    CC = R0 == 2;
    IF !CC JUMP .Lp8;
    R0 = R5 >> 8;
    W[P1 + 0x80] = R0;          /* XMT_DATA8 = register high byte */
    R0 = 0xFF (Z);
    R0 = R5 & R0;
    W[P1 + 0x80] = R0;          /* register low byte */
    SSYNC;
    R0 = 0x0081 (Z);            /* MEN | DCNT 2, transmit, stop at end */
    JUMP .Lpgo;
.Lp8:
    W[P1 + 0x80] = R5;          /* XMT_DATA8 = register (MMRs need 16/32-bit access) */
    SSYNC;
    R0 = 0x0041 (Z);            /* MEN | DCNT 1, transmit, stop at end */
.Lpgo:
    W[P1 + 0x14] = R0;
    SSYNC;
    CALL .Lwait;
    CC = R4 == 0;
    IF CC JUMP .Lrd;
    R4 = 1;
    JUMP .Lfail;
.Lrd:
    /* --- read N bytes --- */
    CALL .Lprep;
    R0 = [P0 + 0x20];
    W[P1 + 0x1C] = R0;
    R3 = [P0 + 0x28];           /* N */
    R0 = R3 << 6;
    R1 = 0x0005 (Z);            /* MEN | MDIR */
    R0 = R0 | R1;
    W[P1 + 0x14] = R0;
    SSYNC;
    R2.L = 0x0000;              /* timeout */
    R2.H = 0x0008;
.Lrbyte:
    R0 = W[P1 + 0x2C] (Z);      /* FIFO_STAT */
    R1 = 0x000C (Z);
    R1 = R0 & R1;
    CC = R1 == 0;
    IF CC JUMP .Lrchk;
    R0 = W[P1 + 0x88] (Z);      /* RCV_DATA8 */
    B[P2++] = R0;
    R3 += -1;
    CC = R3 == 0;
    IF CC JUMP .Lrfin;
    JUMP .Lrbyte;
.Lrchk:
    R0 = W[P1 + 0x20] (Z);
    CC = BITTST(R0, 5);         /* MERR */
    IF CC JUMP .Lrerr;
    R2 += -1;
    CC = R2 == 0;
    IF !CC JUMP .Lrbyte;
    R4 = 3;
    JUMP .Lfail;
.Lrerr:
    R4 = 2;
    JUMP .Lfail;
.Lrfin:
    CALL .Lwait;                /* MCOMP after the last byte */
    R5 += 1;
    R6 += -1;
    CC = R6 == 0;
    IF !CC JUMP .Lreg;
    R4 = 0;
.Lfail:
    [P0 + 4] = R4;
    R0 = 0;
    W[P1 + 0x14] = R0;
    R0.L = 0xEAD0;
    R0.H = 0x7171;
    [P0] = R0;
.Lend:
    JUMP .Lend;

/* clear status, flush FIFOs, leave a bus-free gap */
.Lprep:
    R0 = 0;
    W[P1 + 0x14] = R0;
    R0 = 0x0003 (Z);
    W[P1 + 0x28] = R0;
    SSYNC;
    R0 = 0;
    W[P1 + 0x28] = R0;
    R0 = 0x00FF (Z);
    W[P1 + 0x20] = R0;
    R0 = 0x003E (Z);
    W[P1 + 0x18] = R0;
    SSYNC;
    R2 = 0x4000 (Z);
.Lgap:
    R2 += -1;
    CC = R2 == 0;
    IF !CC JUMP .Lgap;
    RTS;

/* wait for MCOMP/MERR: R4 = 0 ok, 1 error, 3 timeout */
.Lwait:
    R2.L = 0x0000;
    R2.H = 0x0008;
.Lw:
    R0 = W[P1 + 0x20] (Z);
    CC = BITTST(R0, 5);
    IF CC JUMP .Lwerr;
    CC = BITTST(R0, 4);
    IF CC JUMP .Lwok;
    R2 += -1;
    CC = R2 == 0;
    IF !CC JUMP .Lw;
    R4 = 3;
    RTS;
.Lwerr:
    R4 = 1;
    RTS;
.Lwok:
    R4 = 0;
    R0 = 0x0010 (Z);
    W[P1 + 0x20] = R0;
    RTS;
