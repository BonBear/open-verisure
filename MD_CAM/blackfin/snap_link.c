/*
 * snap_link.c - Camera capture + software SPI slave towards the CC1110 (RF path).
 *
 * The CC1110 is SPI master (bit-banged mode 0, P1_2 = select) and pulls a 320x240
 * grayscale picture, either as JPEG (0xD0) or raw, one 320x60 quarter at a time (0xC0|q),
 * then sends it over RF. Blackfin side
 * on port H as GPIO (verified 2026-11-11): PH0 SS (low = selected), PH1 SCK, PH2 MISO
 * (output), PH3 MOSI. Every byte received decides the byte answered in the NEXT transfer:
 *
 *   0xD0      grab the whole picture and encode it as grayscale JPEG (jpeg_enc.c, quality
 *             JPEG_QUALITY), then answer 0x5A; the bytes read are length (2 bytes, little
 *             endian) followed by the JPEG
 *   0xD1      the same in colour (YCbCr 4:2:0)
 *   0xC0 | q  grab raw quarter q (0..3) now, then answer 0x5A
 *   0xB0      answer 0x5A if a picture is ready, else 0x00
 *   0xA0      reset the read pointer (next answer is byte 0)
 *   other     answer the next picture byte (CC sends 0x00 while reading)
 *
 * Capture: PPI without frame syncs (PPI_CONTROL 0x383D) after a software VSYNC wait on PG7,
 * gap-free stream, duplicate samples dropped and luma picked in software (see capture()). A capture
 * takes a few camera frames (~0.2 s); the CC1110 waits before asking for the status.
 *
 * Needs DMEM_CONTROL 0x00001001 (data bank A all SRAM), set over JTAG before running.
 * L1: 0xFF800000 magic 0x5B1F11CC once running, 0xFF800004 quarters grabbed,
 *     0xFF800008 bytes sent, 0xFF80000C last command, 0xFF800010 sensor setup TWI errors,
 *     0xFF800014 last band, 0xFF800018 duplicate/luma phase, 0xFF800020 SEQSTAT and
 *     0xFF800024 RETX after an exception, 0xFF800028 last JPEG length,
 *     0xFF801000 16-row strip, 0xFF802800 picture to send (raw quarter or length + JPEG).
 * Tables and variables: 0xFF800100..0xFF800FFF (bf512_l1.ld).
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
#define PORTGIO_CLEAR 0xFFC01504
#define PORTGIO_SET  0xFFC01508
#define PORTGIO_DIR  0xFFC01530
#define PORTFIO_CLEAR 0xFFC00704
#define PORTFIO_DIR  0xFFC00730
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

/* Capture (2026-11-14): with the factory clocks (pll_init) the PPI follows the pixel clock.
 * PPI without frame syncs (PPI_CONTROL 0x383D) started right after the VSYNC pulse gives
 * YUYV at one byte per sample: a sensor line is LINE_SAMP = 1560 samples, of which samples
 * 0..1279 are the 640 active pixels (Y on even samples, U on 1 mod 4, V on 3 mod 4) and
 * 1280..1559 blanking at a constant level; 480 lines per frame.
 * Output 320x240: row R = sensor line 2R, pixel x = Y of sensor pixel 2x; colour: U/V at
 * 160x120 from sensor pixel pair 2cx on sensor line 2R (R even). Raw bands at the start,
 * the middle and the end of the frame all had line starts at exact multiples of LINE_SAMP
 * from the PPI start, so no per-band alignment is done. */
#define BAND_SAMP    15360   /* multiple of 4 */
#define LINE_SAMP    1560
#define ACTIVE_SAMP  1280
#define ROW_STRIDE   (2 * LINE_SAMP)
#define OUT_W        320
#define OUT_H        60
#define OUT_SIZE     (OUT_W * OUT_H)
#define IMG_H        240
#define STRIP_H      16
#define STRIP_BUF    0xFF801000  /* 16 x 320 Y */
#define STRIP_U      0xFF802400  /* 8 x 160 Cb */
#define STRIP_V      0xFF802900  /* 8 x 160 Cr */
#define SEND_BUF     0xFF802E00
#define SEND_MAX     0x5100UL    /* up to 0xFF807F00 */
#define JPEG_QUALITY 50

#include "jpeg_enc.h"

__asm__(
    ".text\n"
    ".global _start\n"
    "_start:\n"
    "    SP.L = 0x0FF0;\n"          /* stack in L1 scratchpad */
    "    SP.H = 0xFFB0;\n"
    "    FP = SP;\n"
    "    CLI R0;\n"                 /* no interrupts: the boot path leaves IVG15 pending */
    "    P0.L = 0x200C;\n"          /* EVT3 (exceptions) -> _exc, not stale L1 content */
    "    P0.H = 0xFFE0;\n"
    "    R0.L = _exc;\n"
    "    R0.H = _exc;\n"
    "    [P0] = R0;\n"
    "    CALL _main;\n"
    "1:  JUMP 1b;\n"
    /* exception: log SEQSTAT and RETX at 0xFF800020/24 and stop here */
    "_exc:\n"
    "    P0.L = 0x0020;\n"
    "    P0.H = 0xFF80;\n"
    "    R0 = SEQSTAT;\n"
    "    [P0] = R0;\n"
    "    R0 = RETX;\n"
    "    [P0 + 4] = R0;\n"
    "2:  JUMP 2b;\n"

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

/* ---- OV7955 setup over TWI (sensor at 7-bit address 0x40, 16-bit registers) ----------
 * Without the factory firmware the sensor powers up with its output pads off and other
 * clock/timing settings, so PCLK/HREF/VSYNC/data stay silent. sensor_init() replays the
 * factory firmware's own init sequence: software reset 0x0103, then 108 register writes
 * (clock, pads, exposure, window 640x480, timing, flip/mirror, gamma, colour), logged over
 * JTAG with a hardware breakpoint on its TWI write routine (0xFFA03F5A) on 2026-11-14. */
#define TWI_CLKDIV      0xFFC01400
#define TWI_CONTROL     0xFFC01404
#define TWI_MASTER_CTL  0xFFC01414
#define TWI_MASTER_STAT 0xFFC01418
#define TWI_MASTER_ADDR 0xFFC0141C
#define TWI_INT_STAT    0xFFC01420
#define TWI_FIFO_CTL    0xFFC01428
#define TWI_FIFO_STAT   0xFFC0142C
#define TWI_XMT_DATA8   0xFFC01480

static int twi_write16(unsigned int reg, unsigned int val)
{
    unsigned long t;
    R16(TWI_MASTER_CTL) = 0;
    R16(TWI_FIFO_CTL) = 3;          /* flush */
    SSYNC();
    R16(TWI_FIFO_CTL) = 0;
    R16(TWI_INT_STAT) = 0xFF;
    R16(TWI_MASTER_STAT) = 0x3E;
    R16(TWI_MASTER_ADDR) = 0x40;
    R16(TWI_XMT_DATA8) = (reg >> 8) & 0xFF;
    R16(TWI_XMT_DATA8) = reg & 0xFF;
    SSYNC();
    R16(TWI_MASTER_CTL) = 0x00C1;   /* MEN | DCNT 3, transmit, stop at end */
    SSYNC();
    for (t = 0; t < 2000000UL && (R16(TWI_FIFO_STAT) & 3) == 3; t++)
        ;                           /* TX FIFO holds 2 bytes: wait for room */
    R16(TWI_XMT_DATA8) = val & 0xFF;
    SSYNC();
    for (t = 0; t < 2000000UL; t++) {
        unsigned int st = R16(TWI_INT_STAT);
        if (st & 0x0020)
            return -1;              /* MERR */
        if (st & 0x0010)
            return 0;               /* MCOMP */
    }
    return -2;
}

/* Clocks as the factory boot stream's init block (run by the boot ROM before the factory
 * program): PLL_CTL 0x1400 (MSEL 10, VCO = 10 x CLKIN 24 MHz = 240 MHz), PLL_DIV 2
 * (CCLK 240 MHz, SCLK 120 MHz). Our boot stream has no init block, so the ROM leaves
 * PLL_CTL 0x0C00 / PLL_DIV 4 (SCLK 36 MHz): the PPI (max SCLK / 2) then cannot follow the
 * sensor's pixel clock, which caused the 1.5x duplicate pattern (found 2026-11-14).
 * The PLL change takes effect in IDLE, woken by the PLL wakeup (SIC_IWR0 bit 0). */
#define PLL_CTL     0xFFC00000
#define PLL_DIV     0xFFC00004
#define PLL_LOCKCNT 0xFFC00010
#define SIC_IWR0    0xFFC00124

static unsigned long pll_init(void)
{
    unsigned long iwr;
    if (R16(PLL_CTL) != 0x1400) {
        iwr = R32(SIC_IWR0);
        R32(SIC_IWR0) = 1;          /* only the PLL wakes the core */
        R16(PLL_LOCKCNT) = 0x1000;
        R16(PLL_CTL) = 0x1400;
        SSYNC();
        __asm__ volatile("idle;");
        R32(SIC_IWR0) = iwr;
    }
    R16(PLL_DIV) = 0x0002;
    SSYNC();
    return ((unsigned long)R16(PLL_CTL) << 16) | R16(PLL_DIV);
}

static unsigned int sensor_init(void)
{
    /* explicit calls, no table: .rodata would land in L1 instruction memory, which the
     * core cannot read with data loads */
    unsigned int errors = 0;
    unsigned long t;

    /* board setup as the factory firmware (0xFFA012CE, 0xFFA01492): PF10, PF11, PF13 outputs
     * low, PG4 output low (sensor PWDN?), PG8 output high (sensor RESETB?), then a hardware
     * reset pulse on PG8 before the register setup */
    R16(PORTFIO_CLEAR) = 0x2C00;
    R16(PORTFIO_DIR) |= 0x2C00;
    R16(PORTGIO_SET) = 0x0100;
    R16(PORTGIO_CLEAR) = 0x0010;
    R16(PORTGIO_DIR) |= 0x0110;
    SSYNC();
    for (t = 0; t < 400000UL; t++)
        __asm__ volatile("nop;");
    R16(PORTGIO_CLEAR) = 0x0100;    /* RESETB low */
    SSYNC();
    for (t = 0; t < 400000UL; t++)
        __asm__ volatile("nop;");
    R16(PORTGIO_SET) = 0x0100;      /* RESETB high */
    SSYNC();
    for (t = 0; t < 2000000UL; t++)
        __asm__ volatile("nop;");

    R16(TWI_CONTROL) = 0;
    SSYNC();
    R16(TWI_CLKDIV) = 0x3232;
    R16(TWI_CONTROL) = 0x008C;      /* TWI_ENA | PRESCALE 12 (SCLK 120 MHz / 10 MHz) */
    SSYNC();
    /* generated from the factory firmware's TWI writes (build/ov7955_factory_init.txt),
     * logged over JTAG on 2026-11-14; entries 0..109 (ends with 0x3503 = 0x00) */
    errors += twi_write16(0x0103, 0x01) != 0;   /* software reset */
    for (t = 0; t < 400000UL; t++)
        __asm__ volatile("nop;");
    errors += twi_write16(0x3105, 0x11) != 0;
    errors += twi_write16(0x3000, 0x07) != 0;
    errors += twi_write16(0x3001, 0xFF) != 0;
    errors += twi_write16(0x3003, 0x1C) != 0;
    errors += twi_write16(0x3004, 0x02) != 0;
    errors += twi_write16(0x3503, 0x03) != 0;
    errors += twi_write16(0x380F, 0x0D) != 0;
    errors += twi_write16(0x380E, 0x02) != 0;
    errors += twi_write16(0x3A03, 0x0D) != 0;
    errors += twi_write16(0x3A02, 0x02) != 0;
    errors += twi_write16(0x3A27, 0x0D) != 0;
    errors += twi_write16(0x3A26, 0x02) != 0;
    errors += twi_write16(0x3502, 0xD0) != 0;
    errors += twi_write16(0x3501, 0x20) != 0;
    errors += twi_write16(0x350B, 0x13) != 0;
    errors += twi_write16(0x350A, 0x00) != 0;
    errors += twi_write16(0x3609, 0xA0) != 0;
    errors += twi_write16(0x3705, 0x15) != 0;
    errors += twi_write16(0x3709, 0x50) != 0;
    errors += twi_write16(0x370D, 0x10) != 0;
    errors += twi_write16(0x3715, 0x92) != 0;
    errors += twi_write16(0x3716, 0x39) != 0;
    errors += twi_write16(0x3717, 0x1B) != 0;
    errors += twi_write16(0x3800, 0x00) != 0;
    errors += twi_write16(0x3801, 0x00) != 0;
    errors += twi_write16(0x3802, 0x00) != 0;
    errors += twi_write16(0x3803, 0x00) != 0;
    errors += twi_write16(0x3804, 0x02) != 0;
    errors += twi_write16(0x3805, 0x9F) != 0;
    errors += twi_write16(0x3806, 0x01) != 0;
    errors += twi_write16(0x3807, 0xEB) != 0;
    errors += twi_write16(0x3808, 0x02) != 0;
    errors += twi_write16(0x3809, 0x80) != 0;
    errors += twi_write16(0x380A, 0x01) != 0;
    errors += twi_write16(0x380B, 0xE0) != 0;
    errors += twi_write16(0x380C, 0x03) != 0;
    errors += twi_write16(0x380D, 0x0C) != 0;
    errors += twi_write16(0x3810, 0x00) != 0;
    errors += twi_write16(0x3811, 0x0C) != 0;
    errors += twi_write16(0x3812, 0x00) != 0;
    errors += twi_write16(0x3813, 0x02) != 0;
    errors += twi_write16(0x3820, 0x06) != 0;
    errors += twi_write16(0x3821, 0x06) != 0;
    errors += twi_write16(0x3A05, 0x38) != 0;
    errors += twi_write16(0x3A08, 0x00) != 0;
    errors += twi_write16(0x3A09, 0x9A) != 0;
    errors += twi_write16(0x3A0A, 0x00) != 0;
    errors += twi_write16(0x3A0B, 0x80) != 0;
    errors += twi_write16(0x3A0D, 0x08) != 0;
    errors += twi_write16(0x3A0E, 0x06) != 0;
    errors += twi_write16(0x3A0F, 0x68) != 0;
    errors += twi_write16(0x3A10, 0x58) != 0;
    errors += twi_write16(0x3A1B, 0x68) != 0;
    errors += twi_write16(0x3A1E, 0x58) != 0;
    errors += twi_write16(0x3A0D, 0x10) != 0;
    errors += twi_write16(0x3A0E, 0x0C) != 0;
    errors += twi_write16(0x4004, 0x00) != 0;
    errors += twi_write16(0x4005, 0x02) != 0;
    errors += twi_write16(0x4006, 0x04) != 0;
    errors += twi_write16(0x4007, 0x02) != 0;
    errors += twi_write16(0x4008, 0x08) != 0;
    errors += twi_write16(0x4009, 0x04) != 0;
    errors += twi_write16(0x4022, 0x01) != 0;
    errors += twi_write16(0x4023, 0xF0) != 0;
    errors += twi_write16(0x4024, 0x00) != 0;
    errors += twi_write16(0x4026, 0x21) != 0;
    errors += twi_write16(0x4027, 0xA0) != 0;
    errors += twi_write16(0x4040, 0x40) != 0;
    errors += twi_write16(0x4300, 0xFF) != 0;
    errors += twi_write16(0x4700, 0x04) != 0;
    errors += twi_write16(0x5000, 0xFD) != 0;
    errors += twi_write16(0x5003, 0xD5) != 0;
    errors += twi_write16(0x5217, 0x01) != 0;
    errors += twi_write16(0x5301, 0x0A) != 0;
    errors += twi_write16(0x5302, 0x15) != 0;
    errors += twi_write16(0x5303, 0x29) != 0;
    errors += twi_write16(0x5304, 0x49) != 0;
    errors += twi_write16(0x5305, 0x58) != 0;
    errors += twi_write16(0x5306, 0x63) != 0;
    errors += twi_write16(0x5307, 0x6E) != 0;
    errors += twi_write16(0x5308, 0x78) != 0;
    errors += twi_write16(0x5309, 0x80) != 0;
    errors += twi_write16(0x530A, 0x89) != 0;
    errors += twi_write16(0x530B, 0x98) != 0;
    errors += twi_write16(0x530C, 0xA5) != 0;
    errors += twi_write16(0x530D, 0xBB) != 0;
    errors += twi_write16(0x530E, 0xCF) != 0;
    errors += twi_write16(0x530F, 0xE3) != 0;
    errors += twi_write16(0x5310, 0x13) != 0;
    errors += twi_write16(0x5601, 0x12) != 0;
    errors += twi_write16(0x5602, 0x72) != 0;
    errors += twi_write16(0x5603, 0x04) != 0;
    errors += twi_write16(0x5604, 0x0F) != 0;
    errors += twi_write16(0x5605, 0x99) != 0;
    errors += twi_write16(0x5606, 0xA8) != 0;
    errors += twi_write16(0x5607, 0x76) != 0;
    errors += twi_write16(0x5608, 0x4F) != 0;
    errors += twi_write16(0x5609, 0x27) != 0;
    errors += twi_write16(0x560A, 0x01) != 0;
    errors += twi_write16(0x560B, 0x9C) != 0;
    errors += twi_write16(0x580A, 0xA0) != 0;
    errors += twi_write16(0x6700, 0x05) != 0;
    errors += twi_write16(0x6701, 0x19) != 0;
    errors += twi_write16(0x6702, 0xFD) != 0;
    errors += twi_write16(0x6703, 0xD7) != 0;
    errors += twi_write16(0x6704, 0xFF) != 0;
    errors += twi_write16(0x6705, 0xFF) != 0;
    errors += twi_write16(0x3011, 0x00) != 0;
    /* the factory firmware writes 0x3503 = 0x00 here (on-chip AEC/AGC on), lets it settle and
     * then freezes it with 0x03; we leave the sensor's AEC/AGC running */
    errors += twi_write16(0x3503, 0x00) != 0;
    R16(TWI_MASTER_CTL) = 0;
    return errors;
}

#define SMP(i) ((int)((buf[i] >> 2) & 0xFF))

/* grab output rows row0 .. row0 + n - 1 (320 pixels each) into yout; if uout/vout are
 * given, also the 160-pixel U/V rows of the even output rows into uout/vout (row R/2 - row0/2) */
static void capture(volatile unsigned char *yout, volatile unsigned char *uout, volatile unsigned char *vout,
                    unsigned int row0, unsigned int n)
{
    volatile unsigned short *buf = (volatile unsigned short *)0xFF900000;
    volatile unsigned long *st = (volatile unsigned long *)0xFF800000;
    unsigned int k, x, r, kmin, kmax;

    R16(PORTF_MUX) = 0x0055;        /* PF0..PF9 PPI data */
    R16(PORTF_FER) = 0x03FF;
    R16(PORTG_MUX) = 0x0480;        /* as the factory firmware */
    R16(PORTGIO_INEN) |= 0x0080;    /* PG7 (VSYNC) readable */
    R16(PORTG_FER) = 0x0460;        /* PG5, PG6, PG10 peripheral; PG7 GPIO */
    SSYNC();

    kmin = (unsigned int)(((unsigned long)row0 * ROW_STRIDE) / BAND_SAMP);
    kmax = (unsigned int)((((unsigned long)(row0 + n - 1)) * ROW_STRIDE + ACTIVE_SAMP + LINE_SAMP) / BAND_SAMP);
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
        R16(PPI_COUNT) = 1279;
        R16(PPI_DELAY) = 0;
        R16(PPI_FRAME) = 480;
        R16(DMA0_CONFIG) = 0x0097;  /* DMAEN | WNR | WDSIZE_16 | DMA2D | DI_EN */
        SSYNC();
        for (t = 0; t < 4000000UL && !(R16(PORTGIO) & 0x0080); t++)
            ;
        for (t = 0; t < 4000000UL && (R16(PORTGIO) & 0x0080); t++)
            ;
        R16(PPI_CONTROL) = 0x383D;  /* no frame syncs, continuous from the end of VSYNC */
        SSYNC();
        for (t = 0; t < 80000000UL; t++)
            if (R16(DMA0_STAT) & 0x0001)
                break;
        R16(PPI_CONTROL) = 0;
        SSYNC();
        if (!(R16(DMA0_STAT) & 0x0001))
            return;

        g0 = (unsigned long)k * BAND_SAMP;
        g1 = g0 + BAND_SAMP;
        st[5] = k;                  /* 0xFF800014: last band */

        for (r = 0; r < n; r++) {
            unsigned long g = (unsigned long)(row0 + r) * ROW_STRIDE;
            if (g + ACTIVE_SAMP <= g0 || g >= g1)
                continue;
            for (x = 0; x < OUT_W; x++) {
                unsigned long gx = g + 4UL * x;
                if (gx >= g0 && gx < g1)
                    yout[r * OUT_W + x] = (unsigned char)SMP(gx - g0);
            }
            if (uout && !((row0 + r) & 1)) {
                unsigned int cr = r >> 1;
                for (x = 0; x < OUT_W / 2; x++) {
                    unsigned long gx = g + 8UL * x;
                    if (gx + 3 >= g0 && gx + 3 < g1 && gx + 1 >= g0) {
                        uout[cr * (OUT_W / 2) + x] = (unsigned char)SMP(gx + 1 - g0);
                        vout[cr * (OUT_W / 2) + x] = (unsigned char)SMP(gx + 3 - g0);
                    }
                }
            }
        }
    }
}

/* whole picture as JPEG into SEND_BUF: 2 length bytes (little endian), then the JPEG;
 * colour 1 = YCbCr 4:2:0, 0 = grayscale; returns the number of bytes to send */
static unsigned int grab_jpeg(unsigned int colour)
{
    volatile unsigned long *st = (volatile unsigned long *)0xFF800000;
    unsigned char *strip = (unsigned char *)STRIP_BUF;
    unsigned char *su = (unsigned char *)STRIP_U;
    unsigned char *sv = (unsigned char *)STRIP_V;
    unsigned char *send = (unsigned char *)SEND_BUF;
    unsigned int row;
    unsigned long len, w = st[13];

    /* test: 0xFF800034 = 0x80000000 | reg << 8 | value writes one sensor register before
     * the next picture (set over JTAG) */
    if (w & 0x80000000UL) {
        st[14] = (unsigned long)twi_write16((unsigned int)((w >> 8) & 0xFFFF), (unsigned int)(w & 0xFF));
        st[13] = 0;
    }
    jpeg_begin(send + 2, SEND_MAX - 2, OUT_W, IMG_H, JPEG_QUALITY, colour);
    for (row = 0; row < IMG_H; row += STRIP_H) {
        if (colour) {
            capture(strip, su, sv, row, STRIP_H);
            jpeg_rows_color(strip, OUT_W, su, sv, OUT_W / 2);
        } else {
            capture(strip, 0, 0, row, STRIP_H);
            jpeg_rows(strip, OUT_W);
            jpeg_rows(strip + 8 * OUT_W, OUT_W);
        }
    }
    len = jpeg_end();
    st[10] = len;                   /* 0xFF800028 */
    send[0] = (unsigned char)(len & 0xFF);
    send[1] = (unsigned char)(len >> 8);
    return (unsigned int)len + 2;
}

int main(void)
{
    volatile unsigned long *st = (volatile unsigned long *)0xFF800000;
    volatile unsigned char *out = (volatile unsigned char *)SEND_BUF;
    unsigned int tx = 0, rx, ptr = 0, ready = 0, size = 0, hold = 0;

    st[1] = 0;
    st[2] = 0;
    st[3] = 0;
    st[8] = 0;
    st[9] = 0;
    st[10] = 0;
    st[17] = pll_init();            /* 0xFF800044: PLL_CTL << 16 | PLL_DIV afterwards */
    st[4] = sensor_init();          /* 0xFF800010: TWI errors during sensor setup */

    R16(PORTH_FER) &= ~0x000F;      /* PH0..PH3 GPIO */
    R16(PORTHIO_DIR) = 0x0004;      /* PH2 output */
    R16(PORTHIO_INEN) = 0x000B;     /* PH0, PH1, PH3 inputs */
    SSYNC();
    st[0] = 0x5B1F11CC;

    if (st[11] == 0x1D6E0000UL) {   /* 0xFF80002C: JTAG test, grab one JPEG at start */
        unsigned long t;
        st[11] = 0;
        for (t = 0; t < 20000000UL; t++)    /* let the sensor's AEC settle */
            __asm__ volatile("nop;");
        size = grab_jpeg(st[18] == 1);
        ready = 1;
        tx = 0x5A;
        st[11] = 0x1D6E00D0UL;
        hold = 1;                   /* keep it for JTAG: ignore capture commands */
    }

    for (;;) {
        rx = spi_xfer(tx);
        if (rx & 0x100)
            continue;               /* aborted transfer: keep the same answer */
        st[3] = rx;
        if (hold && (rx == 0xD0 || rx == 0xD1 || (rx & 0xF0) == 0xC0)) {
            tx = 0x5A;
        } else if (rx == 0xD0 || rx == 0xD1) {
            /* 0xD1 colour; 0xD0 grayscale unless 0xFF800048 = 1 (JTAG test) */
            size = grab_jpeg(rx == 0xD1 || st[18] == 1);
            ready = 1;
            ptr = 0;
            st[1]++;
            tx = 0x5A;
        } else if ((rx & 0xF0) == 0xC0) {
            capture(out, 0, 0, (rx & 3) * OUT_H, OUT_H);
            size = OUT_SIZE;
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
            tx = (ptr < size) ? out[ptr++] : 0;
            st[2]++;
        }
    }
}
