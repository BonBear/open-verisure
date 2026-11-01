/*
 * spi_probe.S - Minimal ADSP-BF512 proof-of-life, loaded by the CC1110 over SPI0 slave boot.
 *
 * After the boot kernel jumps here, SPI0 is re-armed as a slave (mode 1, MSB first)
 * that answers every byte the CC1110 clocks with 0xA5. The CC1110 reads MISO back:
 * 0xA5 means the Blackfin booted our code; 0x00 means it is still in the ROM/SENDZ state.
 */
    .text
    .global _start
_start:
    P0.L = 0x0500;              /* SPI0 register block 0xFFC00500 */
    P0.H = 0xFFC0;

    R0 = 0 (Z);
    W[P0 + 0x00] = R0;          /* SPI0_CTL: disable before reconfiguring */
    SSYNC;

    /* SPE | CPHA | EMISO | TIMOD=01 (transfer on TDBR write); SZ=0 repeats the last byte */
    R0 = 0x4421 (Z);
    W[P0 + 0x00] = R0;
    SSYNC;

    R1 = 0xA5 (Z);
.Lloop:
    W[P0 + 0x0C] = R1;          /* SPI0_TDBR */
    R2 = W[P0 + 0x10] (Z);      /* SPI0_RDBR: read to clear RXS */
    SSYNC;
    JUMP .Lloop;
