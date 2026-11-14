/* jpeg_enc.h - Minimal baseline JPEG encoder, grayscale or YCbCr 4:2:0 (see jpeg_enc.c). */
#ifndef JPEG_ENC_H
#define JPEG_ENC_H

/* Start a w x h picture into out[0..max-1]; quality 1..100; colour 0 = grayscale (w, h
 * multiples of 8), 1 = YCbCr 4:2:0 (w, h multiples of 16); dri = restart interval in MCUs
 * (0 = one MCU row); comment_id >= 0 adds a COM marker 'I', id. */
void jpeg_begin(unsigned char *out, unsigned long max, unsigned int w, unsigned int h,
                unsigned int quality, unsigned int colour, unsigned int dri, int comment_id);
/* Tables and output buffer only, no header (for blobs of separately encoded MCUs). */
void jpeg_tables(unsigned char *out, unsigned long max, unsigned int w, unsigned int quality, unsigned int colour);
/* One MCU as an independent restart interval (DC reset, bits flushed, no marker). */
void jpeg_mcu(const unsigned char *y, unsigned int ystride,
              const unsigned char *cb, const unsigned char *cr, unsigned int cstride);
/* RSTn marker, raw byte, current output length. */
void jpeg_rst(unsigned int n);
void jpeg_put(unsigned int b);
unsigned long jpeg_len(void);
/* Grayscale: encode the next 8 pixel rows; strip points at row 0, stride = bytes per row. */
void jpeg_rows(const unsigned char *strip, unsigned int stride);
/* Colour: encode the next 16 Y rows and the matching 8 rows of Cb and Cr (half width). */
void jpeg_rows_color(const unsigned char *y, unsigned int ystride,
                     const unsigned char *cb, const unsigned char *cr, unsigned int cstride);
/* Finish; returns the JPEG length, or 0 if the output buffer was too small. */
unsigned long jpeg_end(void);

#endif
