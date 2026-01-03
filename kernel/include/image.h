/*
 * TinyOS Image Support
 * Handles embedded images and BMP parsing
 */

#ifndef IMAGE_H
#define IMAGE_H

#include "types.h"

/* Image structure */
typedef struct {
    uint32_t width;
    uint32_t height;
    const uint32_t* data;  /* BGRX pixel data */
} image_t;

/* Draw image to framebuffer at position (x, y) */
void image_draw(uint32_t* fb, uint32_t fb_width, uint32_t fb_height,
                const image_t* img, int x, int y);

/* Draw image scaled to fill area */
void image_draw_scaled(uint32_t* fb, uint32_t fb_width, uint32_t fb_height,
                       const image_t* img, int x, int y, int w, int h);

/* Draw image as background (scaled to fit screen) */
void image_draw_background(uint32_t* fb, uint32_t fb_width, uint32_t fb_height,
                           const image_t* img);

/* BMP file parsing (from filesystem) */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t* data;        /* Allocated pixel data (caller must free) */
    int valid;
} bmp_image_t;

/* Parse BMP from memory buffer */
int bmp_parse(const uint8_t* data, uint32_t size, bmp_image_t* out);

/* Free BMP image data */
void bmp_free(bmp_image_t* img);

#endif /* IMAGE_H */
