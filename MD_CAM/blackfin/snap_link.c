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
 *   0xD2      640x480 colour (needs the SDRAM; length 0 if the SDRAM test failed)
 *   0xD3      320x240 colour as a delta: only the 16x16 blocks that changed since the last
 *             picture (see grab_color_delta), or a full picture when needed
 *   0xD4      320x240 grayscale as 0xD0, lit by the flash (PF10 + PF12 for FLASH_PIC_T x 0.1 ms)
 *   0xD5      alarm picture: as 0xD3 with light, as 0xD4 in the dark (scene mean below DARK_Y)
 *   0xD6      alarm picture: as 0xD2 with light, as 0xD4 in the dark
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
 *     0xFF800014 last band, 0xFF800018 last grab + encode time (CCLK/256), 0xFF800020 SEQSTAT and
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
#define PORTFIO_SET  0xFFC00708
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
#define OUT_W        320
#define OUT_H        60
#define OUT_SIZE     (OUT_W * OUT_H)
#define IMG_H        240
#define STRIP_H      16
#define STRIP_BUF    0xFF801000  /* 16 x 640 Y (16 x 320 at low resolution) */
#define STRIP_U      0xFF803800  /* 8 x 320 Cb */
#define STRIP_V      0xFF804200  /* 8 x 320 Cr */
#define SEND_L1      0xFF804C00  /* picture to send if the SDRAM test failed */
#define SEND_L1_MAX  0x3300UL    /* up to 0xFF807F00 */
#define SEND_SDRAM   0x00100000  /* picture to send in SDRAM (1 MB in) */
#define SEND_SDRAM_MAX 0xFFF0UL  /* the length field is 16 bit */
/* delta pictures (mode 3): current and reference 320x240 picture in SDRAM */
#define CUR_Y        0x00200000
#define CUR_U        0x00213000
#define CUR_V        0x00218000
#define REF_Y        0x00220000
#define REF_U        0x00233000
#define REF_V        0x00238000
#define MCU_X        (OUT_W / 16)
#define MCU_Y        (IMG_H / 16)
/* mean |difference| per 8x8 block that counts as a change. Calibrated 2026-11-15 with the
 * histogram at 0xFF800080: a still, dim scene (Y ~60) 5 min apart gave MCU maxima up to 9
 * (most 5..8), so 6 flagged 167 of 300 MCUs; 12 leaves a margin over that noise */
#define DELTA_T      12
#define FULL_EVERY   120         /* a full picture at least every this many delta pictures */
#define JPEG_QUALITY 65       /* 2026-11-16: Q50 -> Q65 +17 % bytes, clearly less blocking (patch noise 0.80 -> 0.52); Q80 +56 % for little more */

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

/* External SDRAM as the factory init block (EBIU_SDGCTL 0, pause, SDRRC 0x26B, SDBCTL 0x11,
 * SDGCTL 0xC0911109, dummy read to start the power-up sequence); needs SCLK 120 MHz.
 * Then a pattern test over the send buffer; returns the number of bad words (0 = usable). */
#define EBIU_SDGCTL 0xFFC00A10
#define EBIU_SDBCTL 0xFFC00A14
#define EBIU_SDRRC  0xFFC00A18
#define SDRAM_TEST_WORDS 0x50000UL  /* 1.25 MB: send buffer, current and reference picture */

static unsigned long sdram_init(void)
{
    volatile unsigned long *m = (volatile unsigned long *)SEND_SDRAM;
    unsigned long i, bad = 0, t;
    R32(EBIU_SDGCTL) = 0;
    SSYNC();
    for (t = 0; t < 10000UL; t++)
        __asm__ volatile("nop;");
    R16(EBIU_SDRRC) = 0x026B;
    SSYNC();
    R16(EBIU_SDBCTL) = 0x0011;
    SSYNC();
    R32(EBIU_SDGCTL) = 0xC0911109UL;
    SSYNC();
    (void)R16(0x00000000);          /* first access starts the power-up sequence */
    SSYNC();
    for (i = 0; i < SDRAM_TEST_WORDS; i++)
        m[i] = (i * 0x9E3779B1UL) ^ 0xA5A5A5A5UL;
    for (i = 0; i < SDRAM_TEST_WORDS; i++)
        if (m[i] != ((i * 0x9E3779B1UL) ^ 0xA5A5A5A5UL))
            bad++;
    return bad;
}

static unsigned int sensor_init(void)
{
    /* explicit calls, no table: .rodata would land in L1 instruction memory, which the
     * core cannot read with data loads */
    unsigned int errors = 0;
    unsigned long t;

    /* board setup as the factory firmware (0xFFA012CE, 0xFFA01492, 0xFFA016CC): PF10..PF13
     * outputs low (PF10/PF12 = flash LED groups, off), PG4 output low (sensor PWDN?), PG8
     * output high (sensor RESETB?), then a hardware reset pulse on PG8 before the register
     * setup */
    R16(PORTFIO_CLEAR) = 0x3C00;
    R16(PORTFIO_DIR) |= 0x3C00;
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
#define SMP10(i) ((int)(buf[i] & 0x3FF))

/* White balance in software (grey world): the sensor output leans blue, so a constant
 * offset is added to Cb and Cr. Every colour capture sums the raw Cb/Cr samples; awb_update()
 * turns the sums into a pending offset that moves the mean 3/4 of the way to 128 (at most
 * +-AWB_MAX). The pending offset is only applied (awb_apply()) before a picture that becomes
 * a new delta reference, so delta pictures use the same offset as their reference.
 * 0xFF80005C (set over JTAG): 0 = auto, 0x40000000 = off, 0x80000000 | dv << 8 | du = fixed
 * offsets (signed bytes). 0xFF800060: applied offsets (dv << 8 | du, signed bytes).
 * 0xFF800064: raw means of the last update (mean Cr << 8 | mean Cb). */
#define AWB_MAX 24
static int awb_du, awb_dv, awb_pu, awb_pv;
static int awb_valid;                   /* a pending offset has been measured */
static unsigned long awb_su, awb_sv, awb_n;

static int awb_clamp(int v)
{
    return v < -AWB_MAX ? -AWB_MAX : v > AWB_MAX ? AWB_MAX : v;
}

/* a / b by shift and subtract (no libgcc in this build) */
static unsigned long awb_udiv(unsigned long a, unsigned long b)
{
    unsigned long q = 0, r = 0;
    int i;
    for (i = 31; i >= 0; i--) {
        r = (r << 1) | ((a >> i) & 1);
        if (r >= b) {
            r -= b;
            q |= 1UL << i;
        }
    }
    return q;
}

static void awb_update(void)
{
    volatile unsigned long *st = (volatile unsigned long *)0xFF800000;
    int mu, mv;
    if (awb_n == 0)
        return;
    mu = (int)awb_udiv(awb_su, awb_n);
    mv = (int)awb_udiv(awb_sv, awb_n);
    awb_su = awb_sv = awb_n = 0;
    awb_valid = 1;
    st[25] = (unsigned long)((mv << 8) | mu);
    awb_pu = awb_clamp((128 - mu) * 3 / 4);
    awb_pv = awb_clamp((128 - mv) * 3 / 4);
}

static void awb_apply(void)
{
    volatile unsigned long *st = (volatile unsigned long *)0xFF800000;
    unsigned long c = st[23];
    if (c & 0x80000000UL) {
        awb_du = (signed char)(c & 0xFF);
        awb_dv = (signed char)((c >> 8) & 0xFF);
    } else if (c & 0x40000000UL) {
        awb_du = awb_dv = 0;
    } else {
        awb_du = awb_pu;
        awb_dv = awb_pv;
    }
    st[24] = (unsigned long)(((awb_dv & 0xFF) << 8) | (awb_du & 0xFF));
}

/* Lens shading and colour saturation, in software (the OV7955 has no public register map).
 * The wide lens leaves the corners ~30 % darker: Y is multiplied by 1 + k r^2 with r^2 in
 * half pixels of the 640x480 grid (corner 639^2 + 479^2), gain = 1024 + (r2 * shade_k >> 18),
 * so shade_k 127 gives ~1.30 in the corners. The colour was very weak (Cb/Cr spread 2-5):
 * U/V are scaled by sat_q / 4 around 128 after the white balance offset. Both are the same
 * for every picture, so deltas are not affected. Tuning over JTAG: 0xFF80003C =
 * 0x51A70000 | shade_k << 8 | sat_q before a picture; any other value = the defaults. */
#define SHADE_K  127
#define SAT_Q    7
static unsigned int shade_k = SHADE_K, sat_q = SAT_Q;
/* JPEG quality: JPEG_QUALITY, or 0xFF800040 = 0x0A170000 | quality over JTAG (tuning). A
 * delta must use the quality of its full picture (the gateway splices its MCUs into it with
 * that picture's tables), so the full picture keeps it in ref_q. */
static unsigned int jpeg_q = JPEG_QUALITY, ref_q = JPEG_QUALITY;
#define SHARP_Q 4                   /* sharpening, see sharpen_mcu() */
static unsigned int sharp_q = SHARP_Q;

static void tune_load(void)
{
    volatile unsigned long *st = (volatile unsigned long *)0xFF800000;
    unsigned long w = st[15];
    if ((w >> 16) == 0x51A7) {
        shade_k = (w >> 8) & 0xFF;
        sat_q = w & 0xFF;
    } else {
        shade_k = SHADE_K;
        sat_q = SAT_Q;
    }
    w = st[12];
    sharp_q = (w >> 16) == 0x5AA0 && (w & 0xFF) <= 16 ? (w & 0xFF) : SHARP_Q;
    w = st[16];
    jpeg_q = (w >> 16) == 0x0A17 && (w & 0xFF) >= 10 && (w & 0xFF) <= 95 ? (w & 0xFF) : JPEG_QUALITY;
}

static unsigned char shade_px(int y, unsigned long gain)
{
    unsigned long v = ((unsigned long)y * gain) >> 10;
    return (unsigned char)(v > 255 ? 255 : v);
}

static unsigned char sat_px(int v, int d)
{
    v = ((v + d - 128) * (int)sat_q) / 4 + 128;
    return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* rows of one stream segment: samples g0 .. g1 - 1 (counted from the end of VSYNC) are in
 * buf[0 .. g1 - g0 - 1]; pick the output rows row0 .. row0 + n - 1 that fall into it.
 * 320 wide: every output pixel is the mean of its 2x2 sensor pixels (and U/V of the 2x2
 * pixel pairs), from the 10-bit samples. Picking one pixel of four only (as before) left
 * the sensor noise at full strength and aliased edges; the mean halves the noise. If the
 * 2x2 block is not all in this segment (L1 band fallback), the top-left pixel is used. */
static void extract(volatile unsigned short *buf, unsigned long g0, unsigned long g1,
                    volatile unsigned char *yout, volatile unsigned char *uout, volatile unsigned char *vout,
                    unsigned int row0, unsigned int n, unsigned int hires)
{
    unsigned long rstride = hires ? LINE_SAMP : 2UL * LINE_SAMP;
    unsigned int ow = hires ? 2 * OUT_W : OUT_W, ystep = hires ? 2 : 4;
    unsigned int x, r;

    for (r = 0; r < n; r++) {
        unsigned long g = (unsigned long)(row0 + r) * rstride;
        long dy = hires ? 2L * (row0 + r) - 479 : 4L * (row0 + r) + 1 - 479;   /* shading */
        unsigned long dy2 = (unsigned long)(dy * dy);
        if (g + ACTIVE_SAMP <= g0 || g >= g1)
            continue;
        int all = !hires && g >= g0 && g + LINE_SAMP + ACTIVE_SAMP <= g1;
        for (x = 0; x < ow; x++) {
            unsigned long gx = g + (unsigned long)ystep * x;
            long dx = hires ? 2L * x - 639 : 4L * x + 1 - 639;
            unsigned long gain = 1024 + (((unsigned long)(dx * dx) + dy2) * shade_k >> 18);
            if (all) {
                unsigned long i = gx - g0;
                /* no rounding: 4 x 1023 >> 4 = 255, with +8 a saturated block wrapped to 0
                 * (a lit lamp came out black) */
                yout[r * ow + x] = shade_px((SMP10(i) + SMP10(i + 2) + SMP10(i + LINE_SAMP) +
                                             SMP10(i + LINE_SAMP + 2)) >> 4, gain);
            } else if (gx >= g0 && gx < g1)
                yout[r * ow + x] = shade_px(SMP(gx - g0), gain);
        }
        if (uout && !((row0 + r) & 1)) {
            unsigned int cr = r >> 1;
            for (x = 0; x < ow / 2; x++) {
                unsigned long gx = g + 2UL * ystep * x;     /* pixel pair of output pixel 2x */
                if (all) {
                    /* U/V of the two pixel pairs in both lines of output pixels 2x, 2x + 1 */
                    unsigned long i = gx - g0;
                    int u = (SMP10(i + 1) + SMP10(i + 5) + SMP10(i + LINE_SAMP + 1) + SMP10(i + LINE_SAMP + 5)) >> 4;
                    int v = (SMP10(i + 3) + SMP10(i + 7) + SMP10(i + LINE_SAMP + 3) + SMP10(i + LINE_SAMP + 7)) >> 4;
                    awb_su += (unsigned long)u;
                    awb_sv += (unsigned long)v;
                    awb_n++;
                    uout[cr * (ow / 2) + x] = sat_px(u, awb_du);
                    vout[cr * (ow / 2) + x] = sat_px(v, awb_dv);
                } else if (gx + 3 >= g0 && gx + 3 < g1 && gx + 1 >= g0) {
                    int u = SMP(gx + 1 - g0), v = SMP(gx + 3 - g0);
                    awb_su += (unsigned long)u;
                    awb_sv += (unsigned long)v;
                    awb_n++;
                    uout[cr * (ow / 2) + x] = sat_px(u, awb_du);
                    vout[cr * (ow / 2) + x] = sat_px(v, awb_dv);
                }
            }
        }
    }
}

/* Flash test (JTAG): 0xFF800078 = 0xF1000000 | frames << 16 | mask << 8 | t lights the LED
 * groups in mask (bit 0 = PF10, bit 1 = PF12, as the factory firmware's flash level 1/2)
 * for t x 0.1 ms (at most FLASH_MAX_T = 20 ms, timed with the cycle counter) from the start
 * of the next 'frames' whole-frame grabs; the count goes down per grab and the word is 0
 * when used up. Off at every exit of ppi_dma. */
#define FLASH_MAX_T  200           /* 20 ms */
#define CCLK_PER_T   24000UL        /* 0.1 ms at CCLK 240 MHz (pll_init) */
/* flash picture (0xD4): both LED groups for 5 ms from the start of the frame. In a dark
 * room the exposure spans the whole frame, so one pulse lights every row. Room at 1-4 m,
 * fully dark (Y 5 without flash): PF12 1 ms Y 22, 2 ms 40, 5 ms 76, 10 ms 137; both groups
 * 5 ms Y 141, 10 ms 180 (the top rows get less: their exposure ends early in the pulse).
 * Objects a few cm away saturate from 0.5 ms. With room light the exposure is shorter than
 * a frame and the flash adds nothing measurable. */
#define FLASH_PIC_T     50
#define FLASH_PIC_MASK  0x1400      /* PF10 + PF12 */
#define DARK_Y          40          /* alarm pictures (0xD5/0xD6) use the flash below this scene mean */
static unsigned int flash_pic;      /* the next whole-frame grab fires the flash for this x 0.1 ms */
static unsigned long cycles(void)
{
    unsigned long c;
    __asm__ volatile("%0 = CYCLES;" : "=d"(c));
    return c;
}

/* PPI stream into dst by DMA0 (16 bit, 2D: bands x BAND_SAMP samples, continuous) from the
 * end of the next VSYNC; with ymod = -(2 * BAND_SAMP - 2) every band overwrites the last
 * (only the last band is kept). flash: fire the flash test word or flash_pic (whole-frame
 * grabs only).
 * Returns DMA0_STAT (bit 0 DMA_DONE, bit 1 DMA_ERR). */
static unsigned int ppi_dma(unsigned long dst, unsigned int bands, short ymod, int flash)
{
    volatile unsigned long *st = (volatile unsigned long *)0xFF800000;
    unsigned long t, c0 = 0, on = 0;
    unsigned int s, mask = 0;

    if (flash && (st[30] & 0xFF000000UL) == 0xF1000000UL && (st[30] & 0x00FF0000UL)) {
        unsigned long w = st[30];
        mask = ((w >> 8) & 0x03) << 10;
        mask = (mask & 0x0400) | ((mask & 0x0800) << 1);   /* bit 0 -> PF10, bit 1 -> PF12 */
        on = w & 0xFF;
        if (on > FLASH_MAX_T)
            on = FLASH_MAX_T;
        on *= CCLK_PER_T;
        w -= 0x00010000UL;
        st[30] = (w & 0x00FF0000UL) ? w : 0;
    } else if (flash && flash_pic) {
        mask = FLASH_PIC_MASK;
        on = flash_pic * CCLK_PER_T;
        flash_pic = 0;
    }

    R16(PPI_CONTROL) = 0;
    R16(DMA0_CONFIG) = 0;
    R16(DMA0_STAT) = 0x0003;
    R16(PPI_STATUS) = 0xFFFF;
    SSYNC();
    R32(DMA0_START) = dst;
    R16(DMA0_XCOUNT) = BAND_SAMP;
    R16(DMA0_XMOD) = 2;
    R16(DMA0_YCOUNT) = bands;
    R16(DMA0_YMOD) = (unsigned short)ymod;
    R16(PPI_COUNT) = 1279;
    R16(PPI_DELAY) = 0;
    R16(PPI_FRAME) = 480;
    R16(DMA0_CONFIG) = 0x0097;      /* DMAEN | WNR | WDSIZE_16 | DMA2D | DI_EN */
    SSYNC();
    for (t = 0; t < 4000000UL && !(R16(PORTGIO) & 0x0080); t++)
        ;
    for (t = 0; t < 4000000UL && (R16(PORTGIO) & 0x0080); t++)
        ;
    R16(PPI_CONTROL) = 0x383D;      /* no frame syncs, continuous from the end of VSYNC */
    SSYNC();
    if (mask) {
        R16(PORTFIO_SET) = mask;
        c0 = cycles();
    }
    for (t = 0; t < 80000000UL; t++) {
        if (mask && cycles() - c0 >= on) {
            R16(PORTFIO_CLEAR) = mask;
            mask = 0;
        }
        if (R16(DMA0_STAT) & 0x0001)
            break;
    }
    R16(PORTFIO_CLEAR) = 0x1400;    /* flash off in any case */
    R16(PPI_CONTROL) = 0;
    SSYNC();
    s = R16(DMA0_STAT);
    return s;
}

/* Whole frame in SDRAM: one DMA of FRAME_BANDS bands from one sensor frame, so a picture is
 * one instant instead of ~49 frames (one per L1 band) spread over seconds, which smeared
 * anything that moved. 49 x 15360 = 752640 samples >= the last row (479 x 1560 + 1280).
 * 1.5 MB at 0x00300000, clear of the send buffer and CUR/REF. Falls back to L1 bands if the
 * DMA does not finish or reports an error. 0xFF800070: PPI_STATUS << 16 | DMA0_STAT of the
 * last frame grab; 0xFF800074: frame grabs that failed. */
#define FRAME_SDRAM  0x00300000UL
#define FRAME_BANDS  49
static int frame_sdram;             /* 1: SDRAM usable for whole frames (set in main) */
static int frame_valid;             /* the last frame grab succeeded */
static int frame_new = 1;           /* the next capture() grabs a new frame */

/* grab output rows row0 .. row0 + n - 1 into yout; if uout/vout are given, also the U/V
 * rows (half width) of the even output rows into uout/vout (row R/2 - row0/2).
 * hires 0: 320 wide, row R = sensor line 2R, pixel x = sensor pixel 2x;
 * hires 1: 640 wide, row R = sensor line R, pixel x = sensor pixel x.
 * With SDRAM a new frame is grabbed for row0 == 0 (a new picture) or after frame_new = 1;
 * the other strips of the picture come from the same frame. */
static void capture(volatile unsigned char *yout, volatile unsigned char *uout, volatile unsigned char *vout,
                    unsigned int row0, unsigned int n, unsigned int hires)
{
    unsigned long rstride = hires ? LINE_SAMP : 2UL * LINE_SAMP;
    volatile unsigned long *st = (volatile unsigned long *)0xFF800000;
    unsigned int k, kmin, kmax;

    R16(PORTF_MUX) = 0x0055;        /* PF0..PF9 PPI data */
    R16(PORTF_FER) = 0x03FF;
    R16(PORTG_MUX) = 0x0480;        /* as the factory firmware */
    R16(PORTGIO_INEN) |= 0x0080;    /* PG7 (VSYNC) readable */
    R16(PORTG_FER) = 0x0460;        /* PG5, PG6, PG10 peripheral; PG7 GPIO */
    SSYNC();

    if (frame_sdram) {
        if (frame_new || row0 == 0) {
            unsigned int s = ppi_dma(FRAME_SDRAM, FRAME_BANDS, 2, 1);
            st[28] = ((unsigned long)R16(PPI_STATUS) << 16) | s;
            frame_valid = (s & 0x0003) == 0x0001;
            if (!frame_valid)
                st[29]++;
            frame_new = 0;
        }
        if (frame_valid) {
            extract((volatile unsigned short *)FRAME_SDRAM, 0, (unsigned long)FRAME_BANDS * BAND_SAMP,
                    yout, uout, vout, row0, n, hires);
            return;
        }
    }

    kmin = (unsigned int)(((unsigned long)row0 * rstride) / BAND_SAMP);
    kmax = (unsigned int)((((unsigned long)(row0 + n - 1)) * rstride + ACTIVE_SAMP + LINE_SAMP) / BAND_SAMP);
    for (k = kmin; k <= kmax; k++) {
        unsigned long g0 = (unsigned long)k * BAND_SAMP;
        if (!(ppi_dma(0xFF900000UL, k + 1, (short)(-(BAND_SAMP * 2 - 2)), 0) & 0x0001))
            return;
        st[5] = k;                  /* 0xFF800014: last band */
        extract((volatile unsigned short *)0xFF900000, g0, g0 + BAND_SAMP, yout, uout, vout, row0, n, hires);
    }
}

static unsigned long sdram_bad = 0xFFFFFFFFUL;   /* set in main(); 0 = SDRAM usable */

/* buffer the picture is sent from: SDRAM if it passed the test, else L1 */
static unsigned char *send_buf(unsigned long *max)
{
    if (sdram_bad == 0) {
        *max = SEND_SDRAM_MAX;
        return (unsigned char *)SEND_SDRAM;
    }
    *max = SEND_L1_MAX;
    return (unsigned char *)SEND_L1;
}

/* sum of |a - b| over an 8x8 block */
static unsigned long sad8(const volatile unsigned char *a, const volatile unsigned char *b, unsigned int stride)
{
    unsigned long sum = 0;
    unsigned int x, y;
    for (y = 0; y < 8; y++)
        for (x = 0; x < 8; x++) {
            int d = (int)a[y * stride + x] - (int)b[y * stride + x];
            sum += (unsigned long)(d < 0 ? -d : d);
        }
    return sum;
}

/* Largest sum of |difference| over the six 8x8 blocks (16x16 Y + 8x8 Cb/Cr) of MCU m
 * between the current picture and the reference. The MCU counts as changed when this is
 * above 64 * threshold, i.e. some block's mean |difference| is above the threshold. */
static unsigned long mcu_diff(unsigned int m)
{
    unsigned int mx = m % MCU_X, my = m / MCU_X;
    unsigned long yo = (unsigned long)my * 16 * OUT_W + mx * 16, co = (unsigned long)my * 8 * (OUT_W / 2) + mx * 8;
    const volatile unsigned char *cy = (const volatile unsigned char *)CUR_Y + yo;
    const volatile unsigned char *ry = (const volatile unsigned char *)REF_Y + yo;
    unsigned long d, mx_d = sad8(cy, ry, OUT_W);
    d = sad8(cy + 8, ry + 8, OUT_W);
    if (d > mx_d) mx_d = d;
    d = sad8(cy + 8 * OUT_W, ry + 8 * OUT_W, OUT_W);
    if (d > mx_d) mx_d = d;
    d = sad8(cy + 8 * OUT_W + 8, ry + 8 * OUT_W + 8, OUT_W);
    if (d > mx_d) mx_d = d;
    d = sad8((const volatile unsigned char *)CUR_U + co, (const volatile unsigned char *)REF_U + co, OUT_W / 2);
    if (d > mx_d) mx_d = d;
    d = sad8((const volatile unsigned char *)CUR_V + co, (const volatile unsigned char *)REF_V + co, OUT_W / 2);
    if (d > mx_d) mx_d = d;
    return mx_d;
}

/* encode MCU m of the current picture (independent restart interval) and copy it to the
 * reference */
/* Sharpening against the soft lens, only where an MCU is encoded: y + q/32 * (4y - the four
 * neighbours), i.e. q = 4 adds half of (y - neighbour mean). The reference keeps the
 * unsharpened pixels, so the delta check (CUR vs REF) sees no extra noise. Neighbours come
 * from the whole picture in CUR_Y (outside the picture: the pixel itself). Tuning over JTAG:
 * 0xFF800030 = 0x5AA00000 | q (0 = off); anything else = SHARP_Q (sharp_q, see tune_load). */

static void sharpen_mcu(const unsigned char *cy, unsigned int mx, unsigned int my, unsigned char *out)
{
    unsigned int r, x;
    for (r = 0; r < 16; r++) {
        unsigned int yy = my * 16 + r;
        const unsigned char *row = cy + r * OUT_W;
        for (x = 0; x < 16; x++) {
            unsigned int xx = mx * 16 + x;
            int c = row[x];
            int up = yy > 0 ? row[x - OUT_W] : c, dn = yy < IMG_H - 1 ? row[x + OUT_W] : c;
            int lf = xx > 0 ? row[x - 1] : c, rt = xx < OUT_W - 1 ? row[x + 1] : c;
            int v = c + (((4 * c - up - dn - lf - rt) * (int)sharp_q) >> 5);
            out[r * 16 + x] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
    }
}

static void mcu_encode_and_keep(unsigned int m)
{
    unsigned int mx = m % MCU_X, my = m / MCU_X, r;
    unsigned long yo = (unsigned long)my * 16 * OUT_W + mx * 16, co = (unsigned long)my * 8 * (OUT_W / 2) + mx * 8;
    unsigned char *cy = (unsigned char *)CUR_Y + yo, *cu = (unsigned char *)CUR_U + co, *cv = (unsigned char *)CUR_V + co;
    unsigned char *ry = (unsigned char *)REF_Y + yo, *ru = (unsigned char *)REF_U + co, *rv = (unsigned char *)REF_V + co;
    if (sharp_q) {
        unsigned char sy[256];
        sharpen_mcu(cy, mx, my, sy);
        jpeg_mcu(sy, 16, cu, cv, OUT_W / 2);
    } else {
        jpeg_mcu(cy, OUT_W, cu, cv, OUT_W / 2);
    }
    for (r = 0; r < 16; r++) {
        unsigned int x;
        for (x = 0; x < 16; x++)
            ry[r * OUT_W + x] = cy[r * OUT_W + x];
    }
    for (r = 0; r < 8; r++) {
        unsigned int x;
        for (x = 0; x < 8; x++) {
            ru[r * (OUT_W / 2) + x] = cu[r * (OUT_W / 2) + x];
            rv[r * (OUT_W / 2) + x] = cv[r * (OUT_W / 2) + x];
        }
    }
}

/* After power-up the sensor's AEC/AGC needs a while: the first picture ~3 s after power-on
 * was ~11 % brighter than one taken a minute later (Y 101 -> 90), so the first delta
 * against it flagged most MCUs and fell back to a full picture. The AEC moves by less than
 * one level per grab, so comparing neighbouring grabs is not enough (stopped after 4).
 * Grab the middle strip (luma only, ~0.2 s each) until its mean is within 1 of the mean
 * AEC_SPAN grabs earlier, at most AEC_MAX grabs (the CC1110 waits ~33 s for a picture).
 * 0xFF800068: grabs used << 16 | first mean << 8 | last mean. max = 1: one grab only, to
 * measure the scene with the AEC already settled. Returns the last mean. aec_ok: a full
 * settle has run since power-up (the sensor's AEC keeps tracking after that). */
#define AEC_SPAN     10
#define AEC_MAX      75
static int aec_ok;
static unsigned long aec_settle(unsigned int max)
{
    volatile unsigned long *st = (volatile unsigned long *)0xFF800000;
    const volatile unsigned char *y = (const volatile unsigned char *)CUR_Y;
    unsigned char hist[AEC_SPAN];
    unsigned int n, i;
    unsigned long mean = 0, first = 0, old, sum;

    for (n = 0; n < max; n++) {
        frame_new = 1;              /* a new sensor frame for every grab */
        capture((unsigned char *)CUR_Y, 0, 0, (IMG_H - STRIP_H) / 2, STRIP_H, 0);
        sum = 0;
        for (i = 0; i < STRIP_H * OUT_W; i += 4)
            sum += y[i];
        mean = awb_udiv(sum, STRIP_H * OUT_W / 4);   /* no divide in this build */
        if (n == 0)
            first = mean;
        if (n >= AEC_SPAN) {
            old = hist[n % AEC_SPAN];
            if ((mean > old ? mean - old : old - mean) <= 1) {
                n++;
                break;
            }
        }
        hist[n % AEC_SPAN] = (unsigned char)mean;
    }
    st[26] = ((unsigned long)n << 16) | ((first & 0xFF) << 8) | (mean & 0xFF);
    if (max > 1)
        aec_ok = 1;
    return mean;
}

static unsigned int have_ref, full_id, since_full;

/* 320x240 colour picture as a full JPEG (restart marker after every MCU, COM 'I' id) or,
 * if delta is allowed and a reference exists, as a delta blob:
 *   'D' '1' id  mcus_lo mcus_hi  count_lo count_hi  then per changed MCU:
 *   idx_lo idx_hi  len_lo len_hi  len bytes of entropy-coded data (one restart interval)
 * The receiver replaces those restart intervals in its copy of full picture 'id'.
 * Writes into send (after the 2 length bytes); returns the blob length (0 = overflow). */
static unsigned long grab_color_delta(unsigned char *send, unsigned long max, int allow_delta)
{
    volatile unsigned long *st = (volatile unsigned long *)0xFF800000;
    unsigned int row, m, changed = 0, added = 0, total = MCU_X * MCU_Y, full;
    unsigned long thr = st[20] ? st[20] : DELTA_T, len;
    unsigned char *blob = send + 2;
    static unsigned char chg[MCU_X * MCU_Y];

    if (!allow_delta || !have_ref || since_full >= FULL_EVERY) {
        /* this picture becomes the new reference. Right after power-up nothing has been
         * measured yet: grab once only to measure, so the first picture is balanced too
         * (the CC1110 powers the Blackfin per motion event) */
        if (!awb_valid) {
            if (!aec_ok)
                aec_settle(AEC_MAX);
            for (row = 0; row < IMG_H; row += STRIP_H)
                capture((unsigned char *)CUR_Y + (unsigned long)row * OUT_W, (unsigned char *)CUR_U + (unsigned long)(row / 2) * (OUT_W / 2),
                        (unsigned char *)CUR_V + (unsigned long)(row / 2) * (OUT_W / 2), row, STRIP_H, 0);
            awb_update();
        }
        awb_apply();
    }
    for (row = 0; row < IMG_H; row += STRIP_H)
        capture((unsigned char *)CUR_Y + (unsigned long)row * OUT_W, (unsigned char *)CUR_U + (unsigned long)(row / 2) * (OUT_W / 2),
                (unsigned char *)CUR_V + (unsigned long)(row / 2) * (OUT_W / 2), row, STRIP_H, 0);
    awb_update();
    full = !allow_delta || !have_ref || since_full >= FULL_EVERY;
    if (!full) {
        /* 0xFF800080..0xFF8000FC: histogram of the MCUs' largest block mean |difference|
         * (bucket = mean, 31 = 31 and more), to calibrate DELTA_T against sensor noise */
        volatile unsigned long *hist = st + 32;
        for (m = 0; m < 32; m++)
            hist[m] = 0;
        for (m = 0; m < total; m++) {
            unsigned long d = mcu_diff(m), b = d >> 6;
            hist[b > 31 ? 31 : b]++;
            chg[m] = (unsigned char)(d > 64UL * thr);
            changed += chg[m];
        }
        /* hysteresis: a neighbour (left/right/up/down) of a changed MCU also counts from
         * half the threshold, so the edges of a moving object are not left as stale
         * blocks from the reference. Only first-pass MCUs (1) spread, added ones are 2.
         * mcu_diff is computed again for those neighbours only (no room in L1 for a table). */
        for (m = 0; m < total; m++) {
            unsigned int mx = m % MCU_X;
            if (chg[m])
                continue;
            if (((mx > 0 && chg[m - 1] == 1) || (mx < MCU_X - 1 && chg[m + 1] == 1) ||
                 (m >= MCU_X && chg[m - MCU_X] == 1) || (m + MCU_X < total && chg[m + MCU_X] == 1)) &&
                mcu_diff(m) > 32UL * thr) {
                chg[m] = 2;
                changed++;
                added++;
            }
        }
        st[27] = added;             /* 0xFF80006C: MCUs added by the hysteresis */
        if (changed * 2 > total)
            full = 1;
    }
    st[21] = changed;               /* 0xFF800054: changed MCUs (delta check) */
    if (full) {
        full_id = (full_id + 1) & 0xFF;
        ref_q = jpeg_q;
        jpeg_begin(blob, max - 2, OUT_W, IMG_H, ref_q, 1, 1, (int)full_id);
        for (m = 0; m < total; m++) {
            if (m)
                jpeg_rst(m - 1);
            mcu_encode_and_keep(m);
        }
        len = jpeg_end();
        have_ref = len != 0;
        since_full = 0;
        st[22] = 1;                 /* 0xFF800058: 1 full, 2 delta */
        return len;
    }
    jpeg_tables(blob, max - 2, OUT_W, ref_q, 1);
    jpeg_put('D');
    jpeg_put('1');
    jpeg_put(full_id);
    jpeg_put(total & 0xFF);
    jpeg_put(total >> 8);
    jpeg_put(changed & 0xFF);
    jpeg_put(changed >> 8);
    for (m = 0; m < total; m++) {
        unsigned long pos;
        if (!chg[m])
            continue;
        jpeg_put(m & 0xFF);
        jpeg_put(m >> 8);
        pos = jpeg_len();
        jpeg_put(0);
        jpeg_put(0);
        mcu_encode_and_keep(m);
        len = jpeg_len() - pos - 2;
        if (pos + 1 < max - 2) {
            blob[pos] = (unsigned char)(len & 0xFF);
            blob[pos + 1] = (unsigned char)(len >> 8);
        }
    }
    len = jpeg_len();
    if (jpeg_end() == 0) {          /* overflow: send a full picture next time */
        have_ref = 0;
        return 0;
    }
    since_full++;
    st[22] = 2;
    return len;
}

/* whole picture as JPEG into the send buffer: 2 length bytes (little endian), then the JPEG.
 * mode 0 = 320x240 grayscale, 1 = 320x240 colour (full, one restart interval per MCU),
 * 2 = 640x480 colour, 3 = 320x240 colour as a delta against the last picture when
 * possible (see grab_color_delta), 4 = as 0 lit by the flash, 5/6 = as 3/2 with light and as
 * 4 in the dark; modes 1..6 need the SDRAM.
 * Returns the number of bytes to send (2 with length 0 on failure) */
static unsigned int grab_jpeg(unsigned int mode)
{
    volatile unsigned long *st = (volatile unsigned long *)0xFF800000;
    unsigned char *strip = (unsigned char *)STRIP_BUF;
    unsigned char *su = (unsigned char *)STRIP_U;
    unsigned char *sv = (unsigned char *)STRIP_V;
    unsigned long max, len, w = st[13];
    unsigned char *send = send_buf(&max);
    unsigned int row, hires, ow, oh;
    int settled = 0;

    tune_load();
    /* test: 0xFF800034 = 0x80000000 | reg << 8 | value writes one sensor register before
     * the next picture (set over JTAG) */
    if (w & 0x80000000UL) {
        st[14] = (unsigned long)twi_write16((unsigned int)((w >> 8) & 0xFFFF), (unsigned int)(w & 0xFF));
        st[13] = 0;
    }
    if (mode != 0 && sdram_bad != 0) {
        send[0] = send[1] = 0;
        return 2;
    }
    if (mode == 5 || mode == 6) {
        /* alarm picture: with light as 3 (delta) / 2 (640x480), in the dark as 4 (flash).
         * Right after power-up the AEC has to settle first (the first picture of a motion
         * event); after that one grab of the middle strip tells, and a dark scene gets the
         * full settle below anyway (the light may just have gone off and the exposure is
         * still short). Fully dark room: Y 4-5, room light: ~100.
         * 0xFF80007C: dark << 16 | scene mean. */
        int fresh = !aec_ok;
        unsigned long m = aec_settle(fresh ? AEC_MAX : 1);
        int dark = m < DARK_Y;
        st[31] = ((unsigned long)dark << 16) | m;
        mode = dark ? 4 : mode == 5 ? 3 : 2;
        settled = fresh;
    }
    if (mode == 4) {
        /* flash picture, grayscale: colour in the dark is mostly noise and the white balance
         * of the dark pictures before it gave strong blue/red casts; grayscale is also
         * smaller over RF. Let the AEC settle first (in the dark: the longest exposure, so
         * the pulse lights every row), then only the picture's own frame is lit. Not a
         * delta reference. */
        if (!settled)
            aec_settle(AEC_MAX);
        flash_pic = FLASH_PIC_T;
        mode = 0;
    }
    hires = mode == 2;
    ow = hires ? 2 * OUT_W : OUT_W;
    oh = hires ? 2 * IMG_H : IMG_H;
    if (mode == 1 || mode == 3) {
        len = grab_color_delta(send, max, mode == 3);
        st[10] = len;
        send[0] = (unsigned char)(len & 0xFF);
        send[1] = (unsigned char)(len >> 8);
        return (unsigned int)len + 2;
    }
    if (hires && !awb_valid && !have_ref) {
        /* 640x480 as the first picture after power-up: measure the colour first (no
         * delta reference exists yet, so changing the offset is safe) */
        if (!aec_ok)
            aec_settle(AEC_MAX);
        for (row = 0; row < IMG_H; row += STRIP_H)
            capture((unsigned char *)CUR_Y + (unsigned long)row * OUT_W, (unsigned char *)CUR_U + (unsigned long)(row / 2) * (OUT_W / 2),
                    (unsigned char *)CUR_V + (unsigned long)(row / 2) * (OUT_W / 2), row, STRIP_H, 0);
        awb_update();
        awb_apply();
    }
    jpeg_begin(send + 2, max - 2, ow, oh, jpeg_q, mode != 0, 0, -1);
    for (row = 0; row < oh; row += STRIP_H) {
        if (mode != 0) {
            capture(strip, su, sv, row, STRIP_H, hires);
            jpeg_rows_color(strip, ow, su, sv, ow / 2);
        } else {
            capture(strip, 0, 0, row, STRIP_H, 0);
            jpeg_rows(strip, ow);
            jpeg_rows(strip + 8 * ow, ow);
        }
    }
    flash_pic = 0;                  /* unused without a whole-frame grab */
    if (mode != 0)
        awb_update();
    len = jpeg_end();
    st[10] = len;                   /* 0xFF800028 */
    send[0] = (unsigned char)(len & 0xFF);
    send[1] = (unsigned char)(len >> 8);
    return (unsigned int)len + 2;
}

int main(void)
{
    volatile unsigned long *st = (volatile unsigned long *)0xFF800000;
    volatile unsigned char *out;
    unsigned long out_max;
    unsigned int tx = 0, rx, ptr = 0, ready = 0, size = 0, hold = 0;

    st[1] = 0;
    st[2] = 0;
    st[3] = 0;
    st[8] = 0;
    st[9] = 0;
    st[10] = 0;
    /* control words read as input: after a flash boot they hold whatever L1 kept (seen:
     * 0xFFA00000 in the AWB word = fixed offsets 0, 0x1000 as delta threshold). Set them
     * over JTAG after start; st[11]/st[18] stay, the JTAG start test sets them before. */
    st[13] = 0;                     /* 0xFF800034 sensor register write */
    st[20] = 0;                     /* 0xFF800050 delta threshold (0 = DELTA_T) */
    st[23] = 0;                     /* 0xFF80005C AWB control (0 = auto) */
    st[17] = pll_init();           /* 0xFF800044: PLL_CTL << 16 | PLL_DIV afterwards */
    sdram_bad = sdram_init();
    frame_sdram = sdram_bad == 0;
    frame_valid = 0;
    frame_new = 1;
    st[28] = 0;
    st[29] = 0;
    st[30] = 0;                     /* 0xFF800078 flash test word */
    st[15] = 0;                     /* 0xFF80003C shading/saturation tuning (see tune_load) */
    st[16] = 0;                     /* 0xFF800040 JPEG quality tuning (see tune_load) */
    st[12] = 0;                     /* 0xFF800030 sharpening tuning (see sharpen_mcu) */
    __asm__ volatile("R0 = SYSCFG; BITSET (R0, 1); SYSCFG = R0; CSYNC;" ::: "R0");  /* CCEN: CYCLES runs */
    have_ref = 0;                   /* .bss is not zeroed by a JTAG load */
    awb_valid = 0;
    awb_su = awb_sv = awb_n = 0;
    awb_du = awb_dv = awb_pu = awb_pv = 0;
    full_id = 0;
    since_full = 0;
    st[19] = sdram_bad;             /* 0xFF80004C: bad SDRAM test words (0 = SDRAM in use) */
    out = send_buf(&out_max);
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
        size = grab_jpeg(st[18]);
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
        if (hold && (rx >= 0xD0 && rx <= 0xD6 || (rx & 0xF0) == 0xC0)) {
            tx = 0x5A;
        } else if (rx >= 0xD0 && rx <= 0xD6) {
            /* 0xD0 grayscale, 0xD1 colour, 0xD2 640x480 colour, 0xD3 colour delta, 0xD4
             * grayscale with flash, 0xD5/0xD6 alarm pictures (flash in the dark);
             * 0xFF800048 (JTAG test) overrides the mode of 0xD0 */
            unsigned long c0 = cycles();
            size = grab_jpeg(rx == 0xD0 ? st[18] : rx - 0xD0);
            st[6] = (cycles() - c0) >> 8;   /* 0xFF800018: last grab + encode, CCLK/256 (~1.07 us) */
            ready = 1;
            ptr = 0;
            st[1]++;
            tx = 0x5A;
        } else if ((rx & 0xF0) == 0xC0) {
            if (out_max >= OUT_SIZE) {
                capture(out, 0, 0, (rx & 3) * OUT_H, OUT_H, 0);
                size = OUT_SIZE;
            } else {
                size = 0;           /* raw quarters need the SDRAM send buffer */
            }
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
