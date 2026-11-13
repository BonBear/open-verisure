/*
 * flash_read.c - Read-only access to the Blackfin SPI boot flash (BMODE 011, SPI0 pins),
 * to back up the factory firmware. Only the read commands 0x9F (JEDEC ID) and 0x03 (read)
 * are ever sent; nothing can be written or erased by this program.
 *
 * The bus is bit-banged as GPIO (mode 0): PG15 CS, PG12 SCK, PG14 MOSI, PG13 MISO.
 * The pins are returned to inputs when done.
 *
 * Parameters (set in L1 over JTAG before running):
 * 0xFF800010  mode: 0 = ID + walk the boot stream headers, 1 = read a range,
 *             2 = scan the whole flash (0xFF80001C = size in bytes) in 4 KB pages
 * 0xFF800014  flash address (mode 1)
 * 0xFF800018  length in bytes (mode 1, at most 0x8000)
 * Results:
 * 0xFF800000  magic 0xF1A50D0E when done
 * 0xFF800004  JEDEC ID (manufacturer << 16 | type << 8 | capacity)
 * 0xFF800008  mode 0: number of boot stream blocks walked
 * 0xFF80000C  mode 0: flash offset just after the final block (stream length)
 * 0xFF800020  mode 0: first 64 flash bytes
 * 0xFF800100  mode 0: up to 120 block headers, 16 bytes each (code, address, count, argument)
 * 0xFF800300  mode 2: per 4 KB page two words: bytes that are not 0xFF, byte sum
 * 0xFF900000  mode 1: the bytes read
 *
 * Boot stream format (BF51x): 16-byte block headers; block code bits 31..24 = 0xAD,
 * bit 8 = FILL (no payload follows), bit 15 = FINAL.
 */
#define R16(a) (*(volatile unsigned short *)(a))
#define SSYNC() __asm__ volatile("ssync;")

#define PORTG_FER    0xFFC03204
#define PORTGIO      0xFFC01500
#define PORTGIO_CLR  0xFFC01504
#define PORTGIO_SET  0xFFC01508
#define PORTGIO_DIR  0xFFC01530
#define PORTGIO_INEN 0xFFC01540

#define PIN_SCK  0x1000   /* PG12 */
#define PIN_MISO 0x2000   /* PG13 */
#define PIN_MOSI 0x4000   /* PG14 */
#define PIN_CS   0x8000   /* PG15 */

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

static void read_at(unsigned long addr, volatile unsigned char *dst, unsigned long len)
{
    unsigned long i;
    cs(1);
    xfer(0x03);                     /* READ */
    xfer((addr >> 16) & 0xFF);
    xfer((addr >> 8) & 0xFF);
    xfer(addr & 0xFF);
    for (i = 0; i < len; i++)
        dst[i] = (unsigned char)xfer(0);
    cs(0);
}

int main(void)
{
    volatile unsigned long *st = (volatile unsigned long *)0xFF800000;
    unsigned long id;

    st[0] = 0;
    R16(PORTG_FER) &= ~(PIN_SCK | PIN_MISO | PIN_MOSI | PIN_CS);
    R16(PORTGIO_SET) = PIN_CS;      /* CS high before it becomes an output */
    R16(PORTGIO_CLR) = PIN_SCK | PIN_MOSI;
    R16(PORTGIO_INEN) |= PIN_MISO;
    R16(PORTGIO_DIR) |= PIN_SCK | PIN_MOSI | PIN_CS;
    SSYNC();

    cs(1);
    xfer(0x9F);                     /* JEDEC ID */
    id = xfer(0) << 16;
    id |= xfer(0) << 8;
    id |= xfer(0);
    cs(0);
    st[1] = id;

    if (st[4] == 1) {
        unsigned long len = st[6];
        if (len > 0x8000)
            len = 0x8000;
        read_at(st[5], (volatile unsigned char *)0xFF900000, len);
    } else if (st[4] == 2) {
        volatile unsigned long *res = (volatile unsigned long *)0xFF800300;
        unsigned long pages = st[7] / 4096, pg, i;
        if (pages > 256)
            pages = 256;
        for (pg = 0; pg < pages; pg++) {
            unsigned long used = 0, sum = 0;
            cs(1);
            xfer(0x03);
            xfer(((pg * 4096) >> 16) & 0xFF);
            xfer(((pg * 4096) >> 8) & 0xFF);
            xfer(0);
            for (i = 0; i < 4096; i++) {
                unsigned int b = xfer(0);
                if (b != 0xFF)
                    used++;
                sum += b;
            }
            cs(0);
            res[pg * 2] = used;
            res[pg * 2 + 1] = sum;
            st[2] = pg + 1;
        }
    } else {
        volatile unsigned long *hdr = (volatile unsigned long *)0xFF800100;
        unsigned long off = 0, n = 0;
        read_at(0, (volatile unsigned char *)0xFF800020, 64);
        while (n < 4000) {
            unsigned long h[4];
            read_at(off, (volatile unsigned char *)h, 16);
            if (n < 120) {
                hdr[n * 4 + 0] = h[0];
                hdr[n * 4 + 1] = h[1];
                hdr[n * 4 + 2] = h[2];
                hdr[n * 4 + 3] = h[3];
            }
            n++;
            if ((h[0] >> 24) != 0xAD)
                break;              /* not a block header: stop */
            off += 16;
            if (!(h[0] & 0x0100))
                off += h[2];        /* payload follows unless FILL */
            if (h[0] & 0x8000)
                break;              /* FINAL */
        }
        st[2] = n;
        st[3] = off;
    }

    R16(PORTGIO_SET) = PIN_CS;
    SSYNC();
    R16(PORTGIO_DIR) &= ~(PIN_SCK | PIN_MOSI | PIN_CS);
    SSYNC();
    st[0] = 0xF1A50D0E;
    return 0;
}
