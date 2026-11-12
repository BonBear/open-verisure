/*
 * mdma_test.S - Memory DMA stream 0: copy 4 words from 0xFF800040 to 0xFF800200 (DI_EN set so DMA_DONE is reported),
 * set up and polled by the core (not over JTAG), after a ~10 s pause so the JTAG
 * emulator can be disconnected first. Results in L1 bank A:
 * 0xFF800000  magic 0x3D3A7E57 when done
 * 0xFF800004  MDMA_D0_IRQ_STATUS at the end
 * 0xFF800008  polls until DMA_DONE (0 = timed out)
 */
    .text
    .global _start
_start:
    P0.L = 0x0000;
    P0.H = 0xFF80;
    P1.L = 0x0F00;              /* MDMA_D0 base */
    P1.H = 0xFFC0;

    R0 = 0;
    [P0] = R0;
    R1 = CYCLES;                /* ~10 s pause (CCLK ~40 MHz) */
    R2.L = 0x0000;
    R2.H = 0x0001;              /* short pause */
.Lpause:
    R0 = CYCLES;
    R0 = R0 - R1;
    CC = R0 < R2;
    IF CC JUMP .Lpause (BP);
    R0 = 0;
    R0.L = 0x1234;
    R0.H = 0xCAFE;
    [P0 + 0x40] = R0;
    R0.L = 0x5678;
    R0.H = 0xBEEF;
    [P0 + 0x44] = R0;

    R0 = 0;
    [P0 + 0x200] = R0;
    [P0 + 0x204] = R0;
    W[P1 + 0x08] = R0;          /* D0 CONFIG off */
    W[P1 + 0x48] = R0;          /* S0 CONFIG off */
    R0 = 3;
    W[P1 + 0x28] = R0;          /* clear status */
    W[P1 + 0x68] = R0;
    SSYNC;

    R0.L = 0x0200;
    R0.H = 0xFF80;
    [P1 + 0x04] = R0;           /* D0 START */
    R0.L = 0x0040;
    R0.H = 0xFF80;
    [P1 + 0x44] = R0;           /* S0 START */
    R0 = 4;
    W[P1 + 0x10] = R0;          /* X_COUNT */
    W[P1 + 0x50] = R0;
    R0 = 2;
    W[P1 + 0x14] = R0;          /* X_MODIFY */
    W[P1 + 0x54] = R0;
    SSYNC;
    R0 = 0x0085 (Z);            /* source first (HRM Listing 6-2): DMAEN | WDSIZE_16 | DI_EN */
    W[P1 + 0x48] = R0;
    R0 = 0x0087 (Z);            /* then destination: DMAEN | WNR | WDSIZE_16 | DI_EN */
    W[P1 + 0x08] = R0;
    SSYNC;

    R2 = 0;
    R3.L = 0x0000;
    R3.H = 0x0010;
.Lpoll:
    R2 += 1;
    R0 = W[P1 + 0x28] (Z);
    CC = BITTST(R0, 0);
    IF CC JUMP .Ldone;
    CC = R2 < R3;
    IF CC JUMP .Lpoll;
    R2 = 0;
.Ldone:
    [P0 + 4] = R0;
    [P0 + 8] = R2;
    R0.L = 0x7E57;
    R0.H = 0x3D3A;
    [P0] = R0;
.Lend:
    JUMP .Lend;
