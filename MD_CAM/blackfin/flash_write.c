/*
 * flash_write.c - Program a block into the EMPTY part of the Blackfin SPI boot flash
 * (EN25F20, 256 KB), leaving the factory firmware at 0x0000..0x7FD3 untouched.
 *
 * Safety rules, enforced in code:
 *  - only addresses 0x8000 .. 0x3FFFF are accepted (FLASH_MIN / FLASH_MAX);
 *  - erase only on request (0xFF80001C = 0xE7A5E000) and only 4 KB sector erase (0x20) of
 *    the sectors the range touches, and only inside our own area 0x8000 .. 0xFFFF
 *    (ERASE_MIN / ERASE_MAX): never sector 0 (selector) or 0x10000.. (factory firmware copy);
 *  - the target range must read back all 0xFF before anything is written;
 *  - the range is read back and compared afterwards.
 *
 * Bit-banged SPI as in flash_read.c: PG15 CS, PG12 SCK, PG14 MOSI, PG13 MISO.
 * Parameters (set in L1 over JTAG before running):
 * 0xFF800010  flash address        0xFF800014  length (1..0x4000)
 * 0xFF80001C  0xE7A5E000 = erase the 4 KB sectors of the range first (own area only)
 * 0xFF900000  the data (loaded over JTAG)
 * Results:
 * 0xFF800000  magic 0xF1A5D07E when finished
 * 0xFF800004  result: 0 ok, 1 address/length refused, 2 target not blank,
 *             3 verify failed, 4 timeout waiting for the flash, 5 erase refused
 * 0xFF800008  status register before, 0xFF80000C  status register after
 * 0xFF800018  first mismatching offset (result 2 or 3)
 */
#define R16(a) (*(volatile unsigned short *)(a))
#define SSYNC() __asm__ volatile("ssync;")

#define PORTG_FER    0xFFC03204
#define PORTGIO      0xFFC01500
#define PORTGIO_CLR  0xFFC01504
#define PORTGIO_SET  0xFFC01508
#define PORTGIO_DIR  0xFFC01530
#define PORTGIO_INEN 0xFFC01540

#define PIN_SCK  0x1000
#define PIN_MISO 0x2000
#define PIN_MOSI 0x4000
#define PIN_CS   0x8000

#define FLASH_MIN 0x8000UL
#define FLASH_MAX 0x40000UL
#define ERASE_MIN 0x8000UL
#define ERASE_MAX 0x10000UL
#define ERASE_OK  0xE7A5E000UL

__asm__(
    ".text\n"
    ".global _start\n"
    "_start:\n"
    "    SP.L = 0x0FF0;\n"
    "    SP.H = 0xFFB0;\n"
    "    FP = SP;\n"
    "    CALL _main;\n"
    "1:  JUMP 1b;\n");

static unsigned int xfer(unsigned int out)
{
    unsigned int i, in = 0;
    for (i = 0; i < 8; i++) {
        if (out & 0x80)
            R16(PORTGIO_SET) = PIN_MOSI;
        else
            R16(PORTGIO_CLR) = PIN_MOSI;
        out <<= 1;
        SSYNC();
        R16(PORTGIO_SET) = PIN_SCK;
        SSYNC();
        in = (in << 1) | ((R16(PORTGIO) & PIN_MISO) ? 1 : 0);
        R16(PORTGIO_CLR) = PIN_SCK;
        SSYNC();
    }
    return in;
}

static void cs(int low)
{
    if (low)
        R16(PORTGIO_CLR) = PIN_CS;
    else
        R16(PORTGIO_SET) = PIN_CS;
    SSYNC();
}

static void addr3(unsigned long a)
{
    xfer((a >> 16) & 0xFF);
    xfer((a >> 8) & 0xFF);
    xfer(a & 0xFF);
}

static unsigned int rdsr(void)
{
    unsigned int s;
    cs(1);
    xfer(0x05);
    s = xfer(0);
    cs(0);
    return s;
}

/* returns the first offset where flash != expected, or len if all equal;
 * expect == 0 compares against 0xFF (blank check) */
static unsigned long compare(unsigned long a, const volatile unsigned char *expect, unsigned long len)
{
    unsigned long i;
    cs(1);
    xfer(0x03);
    addr3(a);
    for (i = 0; i < len; i++) {
        unsigned int b = xfer(0);
        if (b != (expect ? expect[i] : 0xFFu))
            break;
    }
    cs(0);
    return i;
}

/* 4 KB sector erase of every sector touched by [a, a+len); callers check the area */
static int erase_range(unsigned long a, unsigned long len)
{
    unsigned long sec, t;
    for (sec = a & ~0xFFFUL; sec < a + len; sec += 0x1000) {
        if (sec < ERASE_MIN || sec >= ERASE_MAX)
            return 0;
        cs(1);
        xfer(0x06);                 /* WREN */
        cs(0);
        cs(1);
        xfer(0x20);                 /* 4 KB sector erase */
        addr3(sec);
        cs(0);
        for (t = 0; t < 20000000UL && (rdsr() & 1); t++)
            ;
        if (rdsr() & 1)
            return 0;
    }
    return 1;
}

int main(void)
{
    volatile unsigned long *st = (volatile unsigned long *)0xFF800000;
    const volatile unsigned char *data = (const volatile unsigned char *)0xFF900000;
    unsigned long a = st[4], len = st[5], off, t, r;
    unsigned int result = 0;

    st[0] = 0;
    R16(PORTG_FER) &= ~(PIN_SCK | PIN_MISO | PIN_MOSI | PIN_CS);
    R16(PORTGIO_SET) = PIN_CS;
    R16(PORTGIO_CLR) = PIN_SCK | PIN_MOSI;
    R16(PORTGIO_INEN) |= PIN_MISO;
    R16(PORTGIO_DIR) |= PIN_SCK | PIN_MOSI | PIN_CS;
    SSYNC();

    st[2] = rdsr();
    if (a < FLASH_MIN || len == 0 || len > 0x4000 || a + len > FLASH_MAX) {
        result = 1;
    } else if (st[7] == ERASE_OK && (a < ERASE_MIN || a + len > ERASE_MAX)) {
        result = 5;
    } else if (st[7] == ERASE_OK && !erase_range(a, len)) {
        result = 4;
    } else if ((r = compare(a, 0, len)) != len) {
        result = 2;
        st[6] = r;
    } else {
        for (off = 0; off < len && result == 0;) {
            unsigned long n = 256 - ((a + off) & 0xFF), i;
            if (n > len - off)
                n = len - off;
            cs(1);
            xfer(0x06);             /* WREN */
            cs(0);
            cs(1);
            xfer(0x02);             /* page program */
            addr3(a + off);
            for (i = 0; i < n; i++)
                xfer(data[off + i]);
            cs(0);
            for (t = 0; t < 2000000UL && (rdsr() & 1); t++)
                ;                   /* wait while write in progress */
            if (rdsr() & 1)
                result = 4;
            off += n;
        }
        if (result == 0 && (r = compare(a, data, len)) != len) {
            result = 3;
            st[6] = r;
        }
    }
    st[3] = rdsr();
    st[1] = result;
    st[7] = 0;                      /* consume the erase request */

    R16(PORTGIO_SET) = PIN_CS;
    SSYNC();
    R16(PORTGIO_DIR) &= ~(PIN_SCK | PIN_MOSI | PIN_CS);
    SSYNC();
    st[0] = 0xF1A5D07E;
    return 0;
}
