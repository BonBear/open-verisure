/* jpeg_enc.h - Minimal baseline JPEG encoder, grayscale or YCbCr 4:2:0 (see jpeg_enc.c). */
#ifndef JPEG_ENC_H
#define JPEG_ENC_H

/* Start a w x h picture into out[0..max-1]; quality 1..100; colour 0 = grayscale (w, h
 * multiples of 8), 1 = YCbCr 4:2:0 (w, h multiples of 16). */
void jpeg_begin(unsigned char *out, unsigned long max, unsigned int w, unsigned int h,
                unsigned int quality, unsigned int colour);
/* Grayscale: encode the next 8 pixel rows; strip points at row 0, stride = bytes per row. */
void jpeg_rows(const unsigned char *strip, unsigned int stride);
/* Colour: encode the next 16 Y rows and the matching 8 rows of Cb and Cr (half width). */
void jpeg_rows_color(const unsigned char *y, unsigned int ystride,
                     const unsigned char *cb, const unsigned char *cr, unsigned int cstride);
/* Finish; returns the JPEG length, or 0 if the output buffer was too small. */
unsigned long jpeg_end(void);

#endif
