/*
 * rom_spiboot.S - Call the BF51x boot ROM's SPI boot routine (0xEF00000A, bfrom_SpiBoot)
 * to load and start a boot stream from a chosen address in the SPI boot flash.
 * Used to test multi-boot without writing anything: address 0 starts the factory firmware.
 *
 * long bfrom_SpiBoot(long dSpiAddress, long dFlags, long dBlockCount, void *pCallHook)
 *
 * Parameters (set in L1 over JTAG before running; the routine does not return on success):
 * 0xFF800010  flash address of the boot stream
 * 0xFF800014  dFlags. Use 0x03A00000 = BFLAG_PERIPHERAL (0x02000000) | BFLAG_NOAUTO
 *             (0x01000000) | BFLAG_ALTERNATE (0x00800000, external flash on PG15 instead
 *             of the default PH8) | BFLAG_TYPE_3 (0x00200000, 3 address bytes).
 * The ROM does no pin muxing. Set up before running (as after a normal boot):
 * PORTG_FER 0x7C60, PORTG_MUX 0x0480, PG15 GPIO output high, PORTH_FER bit 8 set,
 * SPI0_CTL and DMA7_CONFIG off.
 * Verified 2026-11-14: address 0 with these flags loads and starts the factory firmware.
 * Marker: 0xFF800000 = 0xB0075B17 just before the call (overwritten if the stream loads there).
 */
    .text
    .global _start
_start:
    SP.L = 0x0FF0;              /* stack in L1 scratchpad */
    SP.H = 0xFFB0;
    FP = SP;
    P0.L = 0x0000;
    P0.H = 0xFF80;
    R0.L = 0x5B17;
    R0.H = 0xB007;
    [P0] = R0;
    R0 = [P0 + 0x10];           /* dSpiAddress */
    R1 = [P0 + 0x14];           /* dFlags */
    R2 = 0;                     /* dBlockCount: until the final block */
    R3 = 0;
    SP += -16;
    [SP + 12] = R3;             /* pCallHook = NULL */
    P1.L = 0x000A;
    P1.H = 0xEF00;
    CALL (P1);
    P0.L = 0x0000;              /* only reached if the ROM returns */
    P0.H = 0xFF80;
    [P0 + 4] = R0;
    R1.L = 0xBAD0;
    R1.H = 0xB007;
    [P0] = R1;
1:  JUMP 1b;
