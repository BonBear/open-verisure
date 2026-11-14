/*
 * jpeg_enc.c - Minimal baseline JPEG encoder for the Blackfin in the MD CAM (no libraries,
 * no heap, no division in the per-pixel path).
 *
 * Grayscale (one component) or colour YCbCr 4:2:0 (Y full size, Cb/Cr half size in both
 * directions, MCU 16x16 = 4 Y blocks + Cb + Cr). Standard quantisation tables (ITU T.81
 * Annex K.1/K.2) scaled by a quality factor, standard Huffman tables (Annex K.3), and a
 * restart marker after every MCU row (DRI), so a lost RF packet only damages one row.
 *
 * Use: jpeg_begin(out, max, w, h, quality, colour); then per MCU row, top to bottom,
 * jpeg_rows() (grayscale, 8 rows) or jpeg_rows_color() (16 Y rows + 8 Cb/Cr rows); then
 * jpeg_end() returns the length (0 if the output buffer overflowed). The tables live in L1
 * data memory (see bf512_l1.ld) and are built by jpeg_begin().
 */
#include "jpeg_enc.h"

/* ITU T.81 Annex K.1 / K.2 quantisation tables, natural (row-major) order */
static const unsigned char std_lum_q[64] = {
    16, 11, 10, 16, 24, 40, 51, 61,
    12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56,
    14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68, 109, 103, 77,
    24, 35, 55, 64, 81, 104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101,
    72, 92, 95, 98, 112, 100, 103, 99
};
static const unsigned char std_chr_q[64] = {
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99
};

/* zigzag index -> natural index */
static const unsigned char zigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

/* ITU T.81 Annex K.3 Huffman tables: code counts per length 1..16, then the symbols */
static const unsigned char dc_lum_bits[16] = { 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
static const unsigned char dc_chr_bits[16] = { 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 };
static const unsigned char dc_vals[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const unsigned char ac_lum_bits[16] = { 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7D };
static const unsigned char ac_lum_vals[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08, 0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0A, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
    0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5,
    0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2,
    0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8,
    0xF9, 0xFA
};
static const unsigned char ac_chr_bits[16] = { 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77 };
static const unsigned char ac_chr_vals[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xA1, 0xB1, 0xC1, 0x09, 0x23, 0x33, 0x52, 0xF0,
    0x15, 0x62, 0x72, 0xD1, 0x0A, 0x16, 0x24, 0x34, 0xE1, 0x25, 0xF1, 0x17, 0x18, 0x19, 0x1A, 0x26,
    0x27, 0x28, 0x29, 0x2A, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5,
    0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3,
    0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA,
    0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8,
    0xF9, 0xFA
};

/* cos(k * pi / 16) * 4096, k = 0..8 */
static const short cos16[9] = { 4096, 4017, 3784, 3406, 2896, 2276, 1567, 799, 0 };

/* one Huffman table pair (DC + AC) and quantisation table per component class */
struct jclass {
    unsigned short dc_code[12];
    unsigned char dc_size[12];
    unsigned short ac_code[256];
    unsigned char ac_size[256];
    unsigned long qrecip[64];        /* (1 << 19) / q, natural order */
    unsigned char qtab[64];          /* natural order */
};
static struct jclass cls[2];         /* 0 luma, 1 chroma */
static short dctm[64];               /* dctm[u * 8 + x] = c(u) cos((2x+1) u pi / 16) * 4096 */

static unsigned char *o_buf;
static unsigned long o_len, o_max;
static int o_overflow;
static unsigned long bit_acc;
static int bit_cnt;
static int dc_pred[3];
static unsigned int mcus_x, restart_n, color;

/* shift-subtract division, only used while building the tables */
static unsigned long udiv(unsigned long n, unsigned long d)
{
    unsigned long q = 0, r = 0;
    int i;
    for (i = 31; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) {
            r -= d;
            q |= 1UL << i;
        }
    }
    return q;
}

static void put_byte(unsigned int b)
{
    if (o_len < o_max)
        o_buf[o_len++] = (unsigned char)b;
    else
        o_overflow = 1;
}

static void put_word(unsigned int w)
{
    put_byte(w >> 8);
    put_byte(w & 0xFF);
}

static void put_bits(unsigned long code, int size)
{
    bit_acc = (bit_acc << size) | (code & ((1UL << size) - 1));
    bit_cnt += size;
    while (bit_cnt >= 8) {
        unsigned int b = (unsigned int)(bit_acc >> (bit_cnt - 8)) & 0xFF;
        put_byte(b);
        if (b == 0xFF)
            put_byte(0x00);         /* byte stuffing */
        bit_cnt -= 8;
    }
}

static void flush_bits(void)
{
    if (bit_cnt > 0)
        put_bits(0x7F, 8 - bit_cnt);  /* pad with 1 bits */
    bit_acc = 0;
    bit_cnt = 0;
}

static void build_huff(const unsigned char *bits, const unsigned char *vals,
                       unsigned short *code, unsigned char *size)
{
    unsigned int l, i, k = 0, c = 0;
    for (l = 1; l <= 16; l++) {
        for (i = 0; i < bits[l - 1]; i++) {
            code[vals[k]] = (unsigned short)c;
            size[vals[k]] = (unsigned char)l;
            k++;
            c++;
        }
        c <<= 1;
    }
}

static void put_dht(unsigned int cls_id, const unsigned char *bits, const unsigned char *vals, unsigned int n)
{
    unsigned int i;
    put_word(0xFFC4);
    put_word(2 + 1 + 16 + n);
    put_byte(cls_id);
    for (i = 0; i < 16; i++)
        put_byte(bits[i]);
    for (i = 0; i < n; i++)
        put_byte(vals[i]);
}

static void put_dqt(unsigned int id, const unsigned char *q)
{
    unsigned int i;
    put_word(0xFFDB);               /* DQT, 8-bit, zigzag order */
    put_word(67);
    put_byte(id);
    for (i = 0; i < 64; i++)
        put_byte(q[zigzag[i]]);
}

static int cos_pi16(unsigned int m)   /* cos(m * pi / 16) * 4096 for any m */
{
    m &= 31;
    if (m <= 8)
        return cos16[m];
    if (m <= 16)
        return -cos16[16 - m];
    if (m <= 24)
        return -cos16[m - 16];
    return cos16[32 - m];
}

static void build_class(struct jclass *c, const unsigned char *stdq, unsigned int scale,
                        const unsigned char *dcb, const unsigned char *acb, const unsigned char *acv)
{
    unsigned int i;
    for (i = 0; i < 64; i++) {
        unsigned long q = udiv((unsigned long)stdq[i] * scale + 50, 100);
        if (q < 2)
            q = 2;                  /* keeps |8F| * qrecip inside 32 bits */
        if (q > 255)
            q = 255;
        c->qtab[i] = (unsigned char)q;
        c->qrecip[i] = udiv(1UL << 19, q);
    }
    for (i = 0; i < 256; i++)
        c->ac_size[i] = 0;
    build_huff(dcb, dc_vals, c->dc_code, c->dc_size);
    build_huff(acb, acv, c->ac_code, c->ac_size);
}

void jpeg_begin(unsigned char *out, unsigned long max, unsigned int w, unsigned int h,
                unsigned int quality, unsigned int colour)
{
    unsigned int u, x, scale;

    o_buf = out;
    o_len = 0;
    o_max = max;
    o_overflow = 0;
    bit_acc = 0;
    bit_cnt = 0;
    dc_pred[0] = dc_pred[1] = dc_pred[2] = 0;
    color = colour ? 1 : 0;
    mcus_x = color ? w >> 4 : w >> 3;
    restart_n = 0;

    /* IJG quality scaling */
    if (quality < 1)
        quality = 1;
    if (quality > 100)
        quality = 100;
    scale = quality < 50 ? udiv(5000, quality) : 200 - quality * 2;
    build_class(&cls[0], std_lum_q, scale, dc_lum_bits, ac_lum_bits, ac_lum_vals);
    if (color)
        build_class(&cls[1], std_chr_q, scale, dc_chr_bits, ac_chr_bits, ac_chr_vals);
    for (u = 0; u < 8; u++)
        for (x = 0; x < 8; x++)
            dctm[u * 8 + x] = (short)(u == 0 ? 2896 : cos_pi16((2 * x + 1) * u));

    put_word(0xFFD8);               /* SOI */
    put_word(0xFFE0);               /* APP0 JFIF 1.01, no thumbnail */
    put_word(16);
    put_byte('J'); put_byte('F'); put_byte('I'); put_byte('F'); put_byte(0);
    put_word(0x0101);
    put_byte(0);
    put_word(1);
    put_word(1);
    put_byte(0);
    put_byte(0);
    put_dqt(0, cls[0].qtab);
    if (color)
        put_dqt(1, cls[1].qtab);
    put_word(0xFFC0);               /* SOF0, 8 bit */
    put_word(color ? 17 : 11);
    put_byte(8);
    put_word(h);
    put_word(w);
    if (color) {
        put_byte(3);
        put_byte(1); put_byte(0x22); put_byte(0);   /* Y  2x2, table 0 */
        put_byte(2); put_byte(0x11); put_byte(1);   /* Cb 1x1, table 1 */
        put_byte(3); put_byte(0x11); put_byte(1);   /* Cr 1x1, table 1 */
    } else {
        put_byte(1);
        put_byte(1); put_byte(0x11); put_byte(0);
    }
    put_dht(0x00, dc_lum_bits, dc_vals, 12);
    put_dht(0x10, ac_lum_bits, ac_lum_vals, 162);
    if (color) {
        put_dht(0x01, dc_chr_bits, dc_vals, 12);
        put_dht(0x11, ac_chr_bits, ac_chr_vals, 162);
    }
    put_word(0xFFDD);               /* DRI: restart after every MCU row */
    put_word(4);
    put_word(mcus_x);
    put_word(0xFFDA);               /* SOS */
    if (color) {
        put_word(12);
        put_byte(3);
        put_byte(1); put_byte(0x00);
        put_byte(2); put_byte(0x11);
        put_byte(3); put_byte(0x11);
    } else {
        put_word(8);
        put_byte(1);
        put_byte(1); put_byte(0x00);
    }
    put_byte(0);
    put_byte(63);
    put_byte(0);
}

static void encode_value(int v, unsigned int *size, unsigned long *bits)
{
    unsigned int a = v < 0 ? -v : v, n = 0;
    while (a) {
        n++;
        a >>= 1;
    }
    *size = n;
    *bits = (unsigned long)(v < 0 ? v - 1 : v) & ((1UL << n) - 1);
}

static void encode_block(const unsigned char *px, unsigned int stride, const struct jclass *c, int *pred)
{
    int t[64], q[64];
    int x, y, u, v, run, k, diff;
    unsigned int size;
    unsigned long bits;

    /* rows: t[y][u] = sum_x (p - 128) * dctm[u][x], scaled down by 2^8 */
    for (y = 0; y < 8; y++) {
        const unsigned char *p = px + y * stride;
        for (u = 0; u < 8; u++) {
            long s = 0;
            for (x = 0; x < 8; x++)
                s += (long)((int)p[x] - 128) * dctm[u * 8 + x];
            t[y * 8 + u] = (int)((s + 128) >> 8);
        }
    }
    /* columns: F(v,u) * 8 = (1/4) sum_y t * dctm[v][y] / 2^4 / 2^12 * 8 = sum >> 15 */
    for (u = 0; u < 8; u++)
        for (v = 0; v < 8; v++) {
            long s = 0;
            for (y = 0; y < 8; y++)
                s += (long)t[y * 8 + u] * dctm[v * 8 + y];
            q[v * 8 + u] = (int)((s + (1L << 14)) >> 15);
        }
    /* quantise: round(F / q) = (|8F| * (2^19 / q) + 2^21) >> 22 */
    for (k = 0; k < 64; k++) {
        int f = q[k];
        unsigned long a = (unsigned long)(f < 0 ? -f : f);
        int r = (int)((a * c->qrecip[k] + (1UL << 21)) >> 22);
        q[k] = f < 0 ? -r : r;
    }

    diff = q[0] - *pred;
    *pred = q[0];
    encode_value(diff, &size, &bits);
    put_bits(c->dc_code[size], c->dc_size[size]);
    if (size)
        put_bits(bits, size);

    run = 0;
    for (k = 1; k < 64; k++) {
        int a = q[zigzag[k]];
        if (a == 0) {
            run++;
            continue;
        }
        while (run > 15) {
            put_bits(c->ac_code[0xF0], c->ac_size[0xF0]);
            run -= 16;
        }
        encode_value(a, &size, &bits);
        put_bits(c->ac_code[(run << 4) | size], c->ac_size[(run << 4) | size]);
        put_bits(bits, size);
        run = 0;
    }
    if (run)
        put_bits(c->ac_code[0x00], c->ac_size[0x00]);   /* EOB */
}

static void restart(void)
{
    if (restart_n > 0) {             /* restart marker before every MCU row but the first */
        flush_bits();
        put_word(0xFFD0 + ((restart_n - 1) & 7));
        dc_pred[0] = dc_pred[1] = dc_pred[2] = 0;
    }
    restart_n++;
}

void jpeg_rows(const unsigned char *strip, unsigned int stride)
{
    unsigned int bx;
    restart();
    for (bx = 0; bx < mcus_x; bx++)
        encode_block(strip + bx * 8, stride, &cls[0], &dc_pred[0]);
}

void jpeg_rows_color(const unsigned char *y, unsigned int ystride,
                     const unsigned char *cb, const unsigned char *cr, unsigned int cstride)
{
    unsigned int mx;
    restart();
    for (mx = 0; mx < mcus_x; mx++) {
        const unsigned char *py = y + mx * 16;
        encode_block(py, ystride, &cls[0], &dc_pred[0]);
        encode_block(py + 8, ystride, &cls[0], &dc_pred[0]);
        encode_block(py + 8 * ystride, ystride, &cls[0], &dc_pred[0]);
        encode_block(py + 8 * ystride + 8, ystride, &cls[0], &dc_pred[0]);
        encode_block(cb + mx * 8, cstride, &cls[1], &dc_pred[1]);
        encode_block(cr + mx * 8, cstride, &cls[1], &dc_pred[2]);
    }
}

unsigned long jpeg_end(void)
{
    flush_bits();
    put_word(0xFFD9);               /* EOI */
    return o_overflow ? 0 : o_len;
}
