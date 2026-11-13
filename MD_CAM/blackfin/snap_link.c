/*
 * snap_link.c - Camera capture + software SPI slave towards the CC1110 (RF path).
 *
 * The CC1110 is SPI master (bit-banged mode 0, P1_2 = select) and pulls a 320x240
 * grayscale picture, one 320x60 quarter at a time, then sends it over RF. Blackfin side
 * on port H as GPIO (verified 2026-11-11): PH0 SS (low = selected), PH1 SCK, PH2 MISO
 * (output), PH3 MOSI. Every byte received decides the byte answered in the NEXT transfer:
 *
 *   0xC0 | q  grab quarter q (0..3) now, then answer 0x5A
 *   0xB0      answer 0x5A if a quarter is ready, else 0x00
 *   0xA0      reset the read pointer (next answer is byte 0)
 *   other     answer the next picture byte (CC sends 0x00 while reading)
 *
 * Capture: factory PPI setup (PPI_CONTROL 0x3E0D, PPI_COUNT 1279, software VSYNC wait on
 * PG7), continuous stream with 335 stored samples per line, see ppi_snap.c. A capture
 * takes a few camera frames (~0.2 s); the CC1110 waits before asking for the status.
 *
 * Needs DMEM_CONTROL 0x00001001 (data bank A all SRAM), set over JTAG before running.
 * L1: 0xFF800000 magic 0x5B1F11CC once running, 0xFF800004 quarters grabbed,
 *     0xFF800008 bytes sent, 0xFF80000C last command, 0xFF800400 320x60 quarter.
 */
#define R16(a) (*(volatile unsigned short *)(a))
#define R32(a) (*(volatile unsigned long *)(a))
#define SSYNC() __asm__ volatile("ssync;")

#define PORTF_FER    0xFFC03200
#define PORTG_FER    0xFFC03204
#define PORTH_FER    0xFFC03208
#define PORTF_MUX    0xFFC03210
#define PORTG_MUX    0xFFC03214
#define PORTGIO      0xFFC01500
#define PORTGIO_INEN 0xFFC01540
#define PORTHIO      0xFFC01700
#define PORTHIO_DIR  0xFFC01730
#define PORTHIO_INEN 0xFFC01740
#define PPI_CONTROL  0xFFC01000
#define PPI_STATUS   0xFFC01004
#define PPI_COUNT    0xFFC01008
#define PPI_DELAY    0xFFC0100C
#define PPI_FRAME    0xFFC01010
#define DMA0_START   0xFFC00C04
#define DMA0_CONFIG  0xFFC00C08
#define DMA0_XCOUNT  0xFFC00C10
#define DMA0_XMOD    0xFFC00C14
#define DMA0_YCOUNT  0xFFC00C18
#define DMA0_YMOD    0xFFC00C1C
#define DMA0_STAT    0xFFC00C28

#define BAND_LINES   12
#define LINE_SAMP    640
#define BAND_SAMP    (BAND_LINES * LINE_SAMP)
#define LINE_STRIDE  335
#define OUT_W        320
#define OUT_H        60
#define OUT_SIZE     (OUT_W * OUT_H)

__asm__(
    ".text\n"
    ".global _start\n"
    "_start:\n"
    "    SP.L = 0x0FF0;\n"          /* stack in L1 scratchpad */
    "    SP.H = 0xFFB0;\n"
    "    FP = SP;\n"
    "    CALL _main;\n"
    "1:  JUMP 1b;\n"

    /* unsigned int spi_xfer(unsigned int tx): wait for SS low, shift one byte (mode 0,
     * MSB first) with tx on PH2, return the byte from PH3; returns 0x100 if SS went high
     * early. Uses R0-R3, P0 only (caller-saved). */
    ".global _spi_xfer\n"
    "_spi_xfer:\n"
    "    P0.L = 0x1700;\n"          /* PORTHIO */
    "    P0.H = 0xFFC0;\n"
    "    R1 = 0x0004 (Z);\n"        /* PH2 */
    "2:  R2 = W[P0] (Z);\n"         /* wait SS high (idle) */
    "    CC = BITTST(R2, 0);\n"
    "    IF !CC JUMP 2b;\n"
    "3:  R2 = W[P0] (Z);\n"         /* wait SS low */
    "    CC = BITTST(R2, 0);\n"
    "    IF CC JUMP 3b;\n"
    "    R3 = 8;\n"
    "    [--SP] = R4;\n"
    "    R4 = 0;\n"
    "4:  CC = BITTST(R0, 7);\n"     /* next MISO bit before the rising edge */
    "    IF CC JUMP 5f;\n"
    "    W[P0 + 4] = R1;\n"         /* CLEAR PH2 */
    "    JUMP 6f;\n"
    "5:  W[P0 + 8] = R1;\n"         /* SET PH2 */
    "6:  R2 = W[P0] (Z);\n"         /* wait SCK high */
    "    CC = BITTST(R2, 0);\n"
    "    IF CC JUMP 9f;\n"
    "    CC = BITTST(R2, 1);\n"
    "    IF !CC JUMP 6b;\n"
    "    R4 <<= 1;\n"
    "    CC = BITTST(R2, 3);\n"
    "    IF !CC JUMP 7f;\n"
    "    BITSET(R4, 0);\n"
    "7:  R2 = W[P0] (Z);\n"         /* wait SCK low */
    "    CC = BITTST(R2, 0);\n"
    "    IF CC JUMP 9f;\n"
    "    CC = BITTST(R2, 1);\n"
    "    IF CC JUMP 7b;\n"
    "    R0 <<= 1;\n"
    "    R3 += -1;\n"
    "    CC = R3 == 0;\n"
    "    IF !CC JUMP 4b;\n"
    "    R0 = R4;\n"
    "    R4 = [SP++];\n"
    "    RTS;\n"
    "9:  R0 = 0x100 (Z);\n"
    "    R4 = [SP++];\n"
    "    RTS;\n");

unsigned int spi_xfer(unsigned int tx);

static void capture(volatile unsigned char *out, unsigned int q)
{
    volatile unsigned short *buf = (volatile unsigned short *)0xFF900000;
    unsigned int k, x, r, kmin, kmax;

    R16(PORTF_MUX) = 0x0055;        /* PF0..PF9 PPI data */
    R16(PORTF_FER) = 0x03FF;
    R16(PORTG_MUX) = 0x0480;        /* as the factory firmware */
    R16(PORTGIO_INEN) |= 0x0080;    /* PG7 (VSYNC) readable */
    R16(PORTG_FER) = 0x0460;        /* PG5, PG6, PG10 peripheral; PG7 GPIO */
    SSYNC();

    kmin = (unsigned int)(((unsigned long)q * OUT_H * 2 * LINE_STRIDE) / BAND_SAMP);
    kmax = (unsigned int)((((unsigned long)(q * OUT_H + OUT_H - 1) * 2) * LINE_STRIDE + OUT_W) / BAND_SAMP);
    for (k = kmin; k <= kmax; k++) {
        unsigned long t, g0, g1;

        R16(PPI_CONTROL) = 0;
        R16(DMA0_CONFIG) = 0;
        R16(DMA0_STAT) = 0x0003;
        R16(PPI_STATUS) = 0xFFFF;
        SSYNC();
        R32(DMA0_START) = 0xFF900000;
        R16(DMA0_XCOUNT) = BAND_SAMP;
        R16(DMA0_XMOD) = 2;
        R16(DMA0_YCOUNT) = k + 1;
        R16(DMA0_YMOD) = (unsigned short)(-(BAND_SAMP * 2 - 2));
        R16(PPI_COUNT) = 2 * LINE_SAMP - 1;
        R16(PPI_DELAY) = 0;
        R16(PPI_FRAME) = 480;
        R16(DMA0_CONFIG) = 0x0097;  /* DMAEN | WNR | WDSIZE_16 | DMA2D | DI_EN */
        SSYNC();
        for (t = 0; t < 2000000UL && !(R16(PORTGIO) & 0x0080); t++)
            ;
        for (t = 0; t < 2000000UL && (R16(PORTGIO) & 0x0080); t++)
            ;
        R16(PPI_CONTROL) = 0x3E0D;
        SSYNC();
        for (t = 0; t < 20000000UL; t++)
            if (R16(DMA0_STAT) & 0x0001)
                break;
        R16(PPI_CONTROL) = 0;
        SSYNC();
        if (!(R16(DMA0_STAT) & 0x0001))
            return;

        g0 = (unsigned long)k * BAND_SAMP;
        g1 = g0 + BAND_SAMP;
        for (r = 0; r < OUT_H; r++) {
            unsigned long g = (unsigned long)(q * OUT_H + r) * 2 * LINE_STRIDE;
            if (g + OUT_W <= g0 || g >= g1)
                continue;
            for (x = 0; x < OUT_W; x++) {
                unsigned long gx = g + x;
                if (gx >= g0 && gx < g1)
                    out[r * OUT_W + x] = (unsigned char)((buf[gx - g0] >> 2) & 0xFF);
            }
        }
    }
}

int main(void)
{
    volatile unsigned long *st = (volatile unsigned long *)0xFF800000;
    volatile unsigned char *out = (volatile unsigned char *)0xFF800400;
    unsigned int tx = 0, rx, ptr = 0, ready = 0;

    st[1] = 0;
    st[2] = 0;
    st[3] = 0;

    R16(PORTH_FER) &= ~0x000F;      /* PH0..PH3 GPIO */
    R16(PORTHIO_DIR) = 0x0004;      /* PH2 output */
    R16(PORTHIO_INEN) = 0x000B;     /* PH0, PH1, PH3 inputs */
    SSYNC();
    st[0] = 0x5B1F11CC;

    for (;;) {
        rx = spi_xfer(tx);
        if (rx & 0x100)
            continue;               /* aborted transfer: keep the same answer */
        st[3] = rx;
        if ((rx & 0xF0) == 0xC0) {
            capture(out, rx & 3);
            ready = 1;
            ptr = 0;
            st[1]++;
            tx = 0x5A;
        } else if (rx == 0xB0) {
            tx = ready ? 0x5A : 0x00;
        } else if (rx == 0xA0) {
            ptr = 0;
            tx = out[ptr++];
        } else {
            tx = (ptr < OUT_SIZE) ? out[ptr++] : 0;
            st[2]++;
        }
    }
}
