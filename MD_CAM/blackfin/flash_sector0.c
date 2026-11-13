/*
 * flash_sector0.c - Erase ONLY sector 0 (0x0000..0x0FFF) of the Blackfin SPI boot flash
 * (EN25F20) and program up to 4 KB there. Used to install the boot selector
 * (boot_select.ldr) and, if needed, to put the factory firmware's first 4 KB back from the
 * backup (build/fw_dump/bootflash_0000_8000.bin, bytes 0..0xFFF).
 *
 * Safety rules, enforced in code:
 *  - the only erase sent is 4 KB sector erase (0x20) at address 0; never chip or block erase;
 *  - nothing happens unless 0xFF800010 holds the confirmation word 0x5EC70000;
 *  - length 1..4096; the sector is checked blank after the erase and compared after writing.
 *
 * Bit-banged SPI as flash_read.c: PG15 CS, PG12 SCK, PG14 MOSI, PG13 MISO.
 * Parameters: 0xFF800010 confirmation 0x5EC70000, 0xFF800014 length, data at 0xFF900000.
 * Results: 0xFF800000 magic 0xF1A55EC0 when finished, 0xFF800004 result:
 *   0 ok, 1 not confirmed / bad length, 2 not blank after erase, 3 verify failed,
 *   4 timeout, 5 flash write-protected (status BP bits set)
 * 0xFF800008 status before, 0xFF80000C status after, 0xFF800018 first bad offset.
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

#define CONFIRM  0x5EC70000UL

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

static int wait_ready(void)
{
    unsigned long t;
    for (t = 0; t < 20000000UL; t++)
        if (!(rdsr() & 1))
            return 1;
    return 0;
}

static unsigned long compare(const volatile unsigned char *expect, unsigned long len)
{
    unsigned long i;
    cs(1);
    xfer(0x03);
    addr3(0);
    for (i = 0; i < len; i++) {
        unsigned int b = xfer(0);
        if (b != (expect ? expect[i] : 0xFFu))
            break;
    }
    cs(0);
    return i;
}

int main(void)
{
    volatile unsigned long *st = (volatile unsigned long *)0xFF800000;
    const volatile unsigned char *data = (const volatile unsigned char *)0xFF900000;
    unsigned long len = st[5], off, r;
    unsigned int result = 0;

    st[0] = 0;
    R16(PORTG_FER) &= ~(PIN_SCK | PIN_MISO | PIN_MOSI | PIN_CS);
    R16(PORTGIO_SET) = PIN_CS;
    R16(PORTGIO_CLR) = PIN_SCK | PIN_MOSI;
    R16(PORTGIO_INEN) |= PIN_MISO;
    R16(PORTGIO_DIR) |= PIN_SCK | PIN_MOSI | PIN_CS;
    SSYNC();

    st[2] = rdsr();
    if (st[4] != CONFIRM || len == 0 || len > 4096) {
        result = 1;
    } else if (st[2] & 0x1C) {
        result = 5;
    } else {
        cs(1);
        xfer(0x06);                 /* WREN */
        cs(0);
        cs(1);
        xfer(0x20);                 /* 4 KB sector erase, address 0 only */
        addr3(0);
        cs(0);
        if (!wait_ready()) {
            result = 4;
        } else if ((r = compare(0, 4096)) != 4096) {
            result = 2;
            st[6] = r;
        } else {
            for (off = 0; off < len && result == 0;) {
                unsigned long n = 256 - (off & 0xFF), i;
                if (n > len - off)
                    n = len - off;
                cs(1);
                xfer(0x06);
                cs(0);
                cs(1);
                xfer(0x02);
                addr3(off);
                for (i = 0; i < n; i++)
                    xfer(data[off + i]);
                cs(0);
                if (!wait_ready())
                    result = 4;
                off += n;
            }
            if (result == 0 && (r = compare(data, len)) != len) {
                result = 3;
                st[6] = r;
            }
        }
    }
    st[3] = rdsr();
    st[1] = result;
    st[4] = 0;                      /* consume the confirmation */

    R16(PORTGIO_SET) = PIN_CS;
    SSYNC();
    R16(PORTGIO_DIR) &= ~(PIN_SCK | PIN_MOSI | PIN_CS);
    SSYNC();
    st[0] = 0xF1A55EC0;
    return 0;
}
