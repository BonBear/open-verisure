/*
 * boot_select.S - Boot selector for the Blackfin SPI boot flash (EN25F20).
 *
 * Flash layout (Kort #1, 2026-11-14):
 *   0x00000  this selector (boot stream started by the boot ROM at reset)
 *   0x08000  our program (snap_link.ldr)
 *   0x10000  factory firmware, copied unchanged from 0x00000 (0x7FD4 bytes)
 *
 * Choice: PH0 low = boot our program. PH0 sits behind the bus switch that the CC1110
 * enables with P1_2: P1_2 = 1 gives PH0 = 0, P1_2 = 0 leaves PH0 pulled high. Our CC1110
 * firmware (BF_JTAG_HOLD) drives P1_2 = 1 before it releases /RESET, so it gets our
 * program; anything else gets the factory firmware. PH0 must read low 16 times in a row.
 *
 * The boot ROM's bfrom_SpiBoot (0xEF00000A) does the load, flags 0x03A00000
 * (PERIPHERAL | NOAUTO | ALTERNATE (flash on PG15) | TYPE_3). It does no pin muxing, so
 * the SPI0 pins are set up here as after a normal boot. It does not return.
 * Also clears DMA7 and sets DMEM_CONTROL = 0x1001 (no data cache): the ROM DMAs headers
 * into 0xFF807FF0, which fails with DMA_ERR if the data cache is on there.
 * Debug: 0xFFB00000 = chosen flash address, 0xFFB00004 = PH0 low count.
 */
    .text
    .global _start
_start:
    SP.L = 0x0FF0;              /* stack in L1 scratchpad, as in the JTAG test */
    SP.H = 0xFFB0;
    FP = SP;

    P1.L = 0x1700;              /* PORTHIO */
    P1.H = 0xFFC0;
    P2.L = 0x3200;              /* PORTF_FER, PORTG_FER +4, PORTH_FER +8, PORTG_MUX +0x14 */
    P2.H = 0xFFC0;

    R0 = 0x0100 (Z);
    W[P2 + 8] = R0;             /* PORTH_FER: PH8 peripheral (needed for SPI0), PH0 GPIO */
    R0 = W[P1 + 0x30] (Z);
    BITCLR(R0, 0);
    W[P1 + 0x30] = R0;          /* PH0 input */
    R0 = W[P1 + 0x40] (Z);
    BITSET(R0, 0);
    W[P1 + 0x40] = R0;          /* PH0 input enable */
    SSYNC;

    R2 = 0;                     /* low count */
    R3 = 16;
.Lsample:
    R1 = 0x400 (Z);             /* settle between samples */
.Lwait:
    R1 += -1;
    CC = R1 == 0;
    IF !CC JUMP .Lwait;
    R0 = W[P1] (Z);
    CC = BITTST(R0, 0);
    IF CC JUMP .Lnext;
    R2 += 1;
.Lnext:
    R3 += -1;
    CC = R3 == 0;
    IF !CC JUMP .Lsample;

    R0 = 0;                     /* factory firmware at 0x10000 ... */
    R0.H = 0x0001;
    R1 = 16;
    CC = R2 == R1;
    IF !CC JUMP .Lboot;
    R0 = 0x8000 (Z);            /* ... or our program at 0x8000 */
.Lboot:
    P0.L = 0x0000;
    P0.H = 0xFFB0;
    [P0] = R0;
    [P0 + 4] = R2;

    /* SPI0 pins as after a normal boot: PG12..PG14 SPI0, PG15 GPIO chip select high */
    P3.L = 0x1500;              /* PORTGIO */
    P3.H = 0xFFC0;
    R1 = 0x8000 (Z);
    W[P3 + 8] = R1;             /* PORTGIO_SET PG15 */
    R1 = W[P3 + 0x30] (Z);
    BITSET(R1, 15);
    W[P3 + 0x30] = R1;          /* PG15 output */
    R1 = 0x0480 (Z);
    W[P2 + 0x14] = R1;          /* PORTG_MUX */
    R1 = 0x7C60 (Z);
    W[P2 + 4] = R1;             /* PORTG_FER */
    P3.L = 0x0500;              /* SPI0_CTL off */
    P3.H = 0xFFC0;
    R1 = 0;
    W[P3] = R1;
    P3.L = 0x0DC8;              /* DMA7_CONFIG off */
    P3.H = 0xFFC0;
    W[P3] = R1;
    R1 = 3;
    W[P3 + 0x20] = R1;          /* DMA7_IRQ_STATUS: clear DMA_DONE/DMA_ERR */
    SSYNC;

    /* The ROM loads headers through a buffer at 0xFF807FF0 (data bank A, upper half).
     * If a previous program enabled the data cache there (factory firmware: DMEM_CONTROL
     * 0x101B), DMA7 fails with DMA_ERR. Make L1 data plain SRAM, as after reset. */
    P3.L = 0x0004;              /* DMEM_CONTROL */
    P3.H = 0xFFE0;
    R1 = 0x1001 (Z);
    [P3] = R1;
    CSYNC;

    R1.L = 0x0000;              /* dFlags 0x03A00000 */
    R1.H = 0x03A0;
    R2 = 0;                     /* dBlockCount */
    R3 = 0;
    SP += -16;
    [SP + 12] = R3;             /* pCallHook = NULL */
    P1.L = 0x000A;
    P1.H = 0xEF00;
    CALL (P1);                  /* bfrom_SpiBoot(address, flags, 0, NULL), no return */
1:  IDLE;
    JUMP 1b;
