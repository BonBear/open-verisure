/*
 * ppi_snap.c - Grab one 160x120 grayscale picture from the OV7955 camera via PPI + DMA0.
 *
 * Camera bus (verified 2026-11-12): PCLK PG5, HREF PG6, VSYNC PG7 (PORTG_MUX function 2,
 * as the factory firmware uses), data D2..D9 on PF2..PF9 (PF0/PF1 unused, read as 1).
 * PPI setup copied from the factory firmware (disassembled from L1): PPI_CONTROL 0x3E0D,
 * PPI_COUNT 1279, PPI_FRAME 480, started right after the VSYNC pulse (PG7 polled).
 * The sensor runs by itself at ~61 fps. Measured with timers 0/1 in WDTH_CAP on PPI_CLK:
 * HREF period 780 PCLK with 640 active, VSYNC period 409500 PCLK (525 lines), so 640
 * samples per line, one sample per pixel, 480 active lines. Nothing is written to the sensor.
 *
 * The frame is taken in 40 bands of 12 lines, one band per camera frame: DMA0 runs 2D with
 * one "row" = 12 lines and a negative Y_MODIFY, so the band buffer is overwritten until
 * band k arrives. Each band's samples are mapped onto the 160x120 output (see the loop).
 *
 * Needs DMEM_CONTROL with data bank A as SRAM (0x00001001), set over JTAG before running.
 * L1 results:
 * 0xFF800000  magic 0x5A9C0DE5 when done
 * 0xFF800004  bands done
 * 0xFF800008  band that timed out + 1 (0 = none)
 * 0xFF800400  160 x 120 bytes, row after row
 */
#define R16(a) (*(volatile unsigned short *)(a))
#define R32(a) (*(volatile unsigned long *)(a))
#define SSYNC() __asm__ volatile("ssync;")

#define PORTF_FER   0xFFC03200
#define PORTG_FER   0xFFC03204
#define PORTF_MUX   0xFFC03210
#define PORTG_MUX   0xFFC03214
#define PPI_CONTROL 0xFFC01000
#define PPI_STATUS  0xFFC01004
#define PPI_COUNT   0xFFC01008
#define PPI_DELAY   0xFFC0100C
#define PPI_FRAME   0xFFC01010
#define DMA0_START  0xFFC00C04
#define DMA0_CONFIG 0xFFC00C08
#define DMA0_XCOUNT 0xFFC00C10
#define DMA0_XMOD   0xFFC00C14
#define DMA0_YCOUNT 0xFFC00C18
#define DMA0_YMOD   0xFFC00C1C
#define DMA0_STAT   0xFFC00C28
#define PORTGIO     0xFFC01500
#define PORTGIO_INEN 0xFFC01540

#define BAND_LINES  12
#define LINE_SAMP   640             /* stored per line (1280 counted, SKIP_EN keeps every other) */
#define BAND_SAMP   (BAND_LINES * LINE_SAMP)
#define BANDS       21
#define OUT_H       120
#define LINE_STRIDE 335
#define OUT_W       160

__asm__(
    ".text\n"
    ".global _start\n"
    "_start:\n"
    "    SP.L = 0x0FF0;\n"          /* stack in L1 scratchpad */
    "    SP.H = 0xFFB0;\n"
    "    FP = SP;\n"
    "    CALL _main;\n"
    "1:  JUMP 1b;\n");

int main(void)
{
    volatile unsigned long *st = (volatile unsigned long *)0xFF800000;
    volatile unsigned short *buf = (volatile unsigned short *)0xFF900000;
    volatile unsigned char *out = (volatile unsigned char *)0xFF800400;
    unsigned int k, l, x;

    st[0] = 0;
    st[1] = 0;
    st[2] = 0;

    R16(PORTF_MUX) = 0x0055;        /* PF0..PF9 PPI data */
    R16(PORTF_FER) = 0x03FF;
    R16(PORTG_MUX) = 0x0480;        /* as the factory firmware: PG5..PG7 function 2, PG9/PG10 function 1 */
    R16(PORTGIO_INEN) |= 0x0080;    /* PG7 (VSYNC) readable for the software frame sync */
    R16(PORTG_FER) = 0x0460;        /* PG5, PG6, PG10 peripheral; PG7 (VSYNC) stays GPIO */
    SSYNC();

    for (k = 0; k < BANDS; k++) {
        unsigned long t;

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
        /* Factory sequence (L1 code at 0xFFA02A5E): wait for the VSYNC pulse to end, then
         * enable PPI with one frame sync (HREF), SKIP_EN | SKIP_EO, DLEN 16. */
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
        if (!(R16(DMA0_STAT) & 0x0001)) {
            st[2] = k + 1;
            break;
        }

        /* Measured: PPI does not wait for HREF between chunks, so the samples after the
         * VSYNC wait form one continuous stream with a line stride of 335 stored samples
         * (left/right check with half the lens covered). Global sample g = k * BAND_SAMP + i
         * lies on line g / 335 at x = g % 335. Output: every 4th line, every 2nd sample. */
        {
            unsigned long g0 = (unsigned long)k * BAND_SAMP, g1 = g0 + BAND_SAMP;
            unsigned int r;
            for (r = 0; r < OUT_H; r++) {
                unsigned long g = (unsigned long)r * 4 * LINE_STRIDE;
                if (g + 2 * OUT_W <= g0 || g >= g1)
                    continue;
                for (x = 0; x < OUT_W; x++) {
                    unsigned long gx = g + 2 * x;
                    if (gx >= g0 && gx < g1)
                        out[r * OUT_W + x] = (unsigned char)((buf[gx - g0] >> 2) & 0xFF);
                }
            }
        }
        st[1] = k + 1;
    }

    st[0] = 0x5A9C0DE5;
    return 0;
}
