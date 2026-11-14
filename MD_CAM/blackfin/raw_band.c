/*
 * raw_band.c - Capture one raw block of the camera stream at full rate (no SKIP_EN), for
 * analysis over JTAG. The sensor must already be set up (snap_link's sensor_init() ran
 * at boot). PPI setup as snap_link but PPI_CONTROL without SKIP_EN|SKIP_EO, so every
 * PCLK sample is stored (16-bit words, data = bits 9..2).
 *
 * Parameters: 0xFF800014 PPI_COUNT (0 = 1279); 0xFF800010 block index k: the block holds stream samples
 *             k*BLOCK .. k*BLOCK+BLOCK-1 counted from the end of the VSYNC pulse.
 * 0xFF800018 PORTG_MUX, 0xFF80001C PORTG_FER, 0xFF800020 PPI_CONTROL (0 = defaults; a
 * PPI_CONTROL override skips the software VSYNC wait unless 0xFF800024 = 1).
 * Results: 0xFF800000 magic 0x8A3BA4D0 when done, 0xFF800004 1 = ok, 0 = timeout,
 *          0xFF900000 BLOCK 16-bit samples.
 */
#define R16(a) (*(volatile unsigned short *)(a))
#define R32(a) (*(volatile unsigned long *)(a))
#define SSYNC() __asm__ volatile("ssync;")

#define PORTF_FER    0xFFC03200
#define PORTG_FER    0xFFC03204
#define PORTF_MUX    0xFFC03210
#define PORTG_MUX    0xFFC03214
#define PORTGIO      0xFFC01500
#define PORTGIO_INEN 0xFFC01540
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

#define BLOCK 9600

__asm__(
    ".text\n"
    ".global _start\n"
    "_start:\n"
    "    SP.L = 0x0FF0;\n"
    "    SP.H = 0xFFB0;\n"
    "    FP = SP;\n"
    "    CALL _main;\n"
    "1:  JUMP 1b;\n");

int main(void)
{
    volatile unsigned long *st = (volatile unsigned long *)0xFF800000;
    unsigned long t, k = st[4];

    st[0] = 0;
    st[1] = 0;
    R16(PORTF_MUX) = 0x0055;
    R16(PORTF_FER) = 0x03FF;
    R16(PORTG_MUX) = st[6] ? st[6] : 0x0480;     /* 0xFF800018: PORTG_MUX override */
    R16(PORTGIO_INEN) |= 0x0080;
    R16(PORTG_FER) = st[7] ? st[7] : 0x0460;     /* 0xFF80001C: PORTG_FER override */
    R16(PPI_CONTROL) = 0;
    R16(DMA0_CONFIG) = 0;
    R16(DMA0_STAT) = 0x0003;
    R16(PPI_STATUS) = 0xFFFF;
    SSYNC();
    R32(DMA0_START) = 0xFF900000;
    R16(DMA0_XCOUNT) = BLOCK;
    R16(DMA0_XMOD) = 2;
    R16(DMA0_YCOUNT) = k + 1;
    R16(DMA0_YMOD) = (unsigned short)(-(BLOCK * 2 - 2));
    R16(PPI_COUNT) = st[5] ? st[5] : 1279;   /* 0xFF800014: PPI_COUNT override */
    R16(PPI_DELAY) = 0;
    R16(PPI_FRAME) = 480;
    R16(DMA0_CONFIG) = 0x0097;
    SSYNC();
    if (!st[8] || st[9]) {          /* software VSYNC wait unless PPI syncs itself (0xFF800024 = 1 forces it) */
        for (t = 0; t < 2000000UL && !(R16(PORTGIO) & 0x0080); t++)
            ;
        for (t = 0; t < 2000000UL && (R16(PORTGIO) & 0x0080); t++)
            ;
    }
    R16(PPI_CONTROL) = st[8] ? st[8] : 0x380D;   /* 0xFF800020: PPI_CONTROL override */
    SSYNC();
    for (t = 0; t < 40000000UL; t++)
        if (R16(DMA0_STAT) & 0x0001)
            break;
    R16(PPI_CONTROL) = 0;
    SSYNC();
    st[1] = (R16(DMA0_STAT) & 0x0001) ? 1 : 0;
    st[0] = 0x8A3BA4D0;
    return 0;
}
