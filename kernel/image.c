/*
 * TinyOS Image Support Implementation
 */

#include "image.h"
#include "memory.h"

/* Draw image at position (no scaling) */
void image_draw(uint32_t* fb, uint32_t fb_width, uint32_t fb_height,
                const image_t* img, int x, int y) {
    if (!img || !img->data) return;

    for (uint32_t iy = 0; iy < img->height; iy++) {
        int dy = y + iy;
        if (dy < 0 || dy >= (int)fb_height) continue;

        for (uint32_t ix = 0; ix < img->width; ix++) {
            int dx = x + ix;
            if (dx < 0 || dx >= (int)fb_width) continue;

            fb[dy * fb_width + dx] = img->data[iy * img->width + ix];
        }
    }
}

/* Fixed-point precision for bilinear interpolation (16 bits) */
#define FP_SHIFT 16
#define FP_ONE   (1 << FP_SHIFT)
#define FP_MASK  (FP_ONE - 1)

/* Extract RGB components */
static inline void rgb_split(uint32_t c, uint32_t* r, uint32_t* g, uint32_t* b) {
    *r = (c >> 16) & 0xFF;
    *g = (c >> 8) & 0xFF;
    *b = c & 0xFF;
}

/* Combine RGB components */
static inline uint32_t rgb_combine(uint32_t r, uint32_t g, uint32_t b) {
    return (r << 16) | (g << 8) | b;
}

/* Bilinear interpolation between 4 pixels using fixed-point math */
static uint32_t bilinear_sample(const uint32_t* data, uint32_t img_w, uint32_t img_h,
                                 uint32_t fx, uint32_t fy) {
    /* Get integer and fractional parts */
    uint32_t x0 = fx >> FP_SHIFT;
    uint32_t y0 = fy >> FP_SHIFT;
    uint32_t x1 = x0 + 1;
    uint32_t y1 = y0 + 1;

    /* Clamp to image bounds */
    if (x1 >= img_w) x1 = img_w - 1;
    if (y1 >= img_h) y1 = img_h - 1;

    /* Fractional parts (0-255 range for easier math) */
    uint32_t xf = (fx & FP_MASK) >> 8;  /* 0-255 */
    uint32_t yf = (fy & FP_MASK) >> 8;  /* 0-255 */
    uint32_t xf_inv = 256 - xf;
    uint32_t yf_inv = 256 - yf;

    /* Get 4 corner pixels */
    uint32_t c00 = data[y0 * img_w + x0];
    uint32_t c10 = data[y0 * img_w + x1];
    uint32_t c01 = data[y1 * img_w + x0];
    uint32_t c11 = data[y1 * img_w + x1];

    /* Split into components */
    uint32_t r00, g00, b00, r10, g10, b10, r01, g01, b01, r11, g11, b11;
    rgb_split(c00, &r00, &g00, &b00);
    rgb_split(c10, &r10, &g10, &b10);
    rgb_split(c01, &r01, &g01, &b01);
    rgb_split(c11, &r11, &g11, &b11);

    /* Bilinear interpolation for each channel */
    /* Top row interpolation */
    uint32_t r_top = (r00 * xf_inv + r10 * xf) >> 8;
    uint32_t g_top = (g00 * xf_inv + g10 * xf) >> 8;
    uint32_t b_top = (b00 * xf_inv + b10 * xf) >> 8;

    /* Bottom row interpolation */
    uint32_t r_bot = (r01 * xf_inv + r11 * xf) >> 8;
    uint32_t g_bot = (g01 * xf_inv + g11 * xf) >> 8;
    uint32_t b_bot = (b01 * xf_inv + b11 * xf) >> 8;

    /* Vertical interpolation */
    uint32_t r = (r_top * yf_inv + r_bot * yf) >> 8;
    uint32_t g = (g_top * yf_inv + g_bot * yf) >> 8;
    uint32_t b = (b_top * yf_inv + b_bot * yf) >> 8;

    return rgb_combine(r, g, b);
}

/* Draw image scaled using bilinear interpolation */
void image_draw_scaled(uint32_t* fb, uint32_t fb_width, uint32_t fb_height,
                       const image_t* img, int x, int y, int w, int h) {
    if (!img || !img->data || w <= 0 || h <= 0) return;

    /* Calculate fixed-point step sizes */
    uint32_t x_step = ((img->width - 1) << FP_SHIFT) / (w > 1 ? w - 1 : 1);
    uint32_t y_step = ((img->height - 1) << FP_SHIFT) / (h > 1 ? h - 1 : 1);

    uint32_t src_y = 0;
    for (int dy = 0; dy < h; dy++) {
        int fy = y + dy;
        if (fy < 0 || fy >= (int)fb_height) {
            src_y += y_step;
            continue;
        }

        uint32_t src_x = 0;
        for (int dx = 0; dx < w; dx++) {
            int fx = x + dx;
            if (fx < 0 || fx >= (int)fb_width) {
                src_x += x_step;
                continue;
            }

            /* Bilinear sample from source image */
            fb[fy * fb_width + fx] = bilinear_sample(img->data, img->width,
                                                      img->height, src_x, src_y);
            src_x += x_step;
        }
        src_y += y_step;
    }
}

/* Draw image as fullscreen background (cover mode - fills screen) */
void image_draw_background(uint32_t* fb, uint32_t fb_width, uint32_t fb_height,
                           const image_t* img) {
    if (!img || !img->data) return;

    /* COVER mode: Scale to fill entire screen (may crop edges) */
    uint32_t scaled_w, scaled_h;
    if (fb_width * img->height > fb_height * img->width) {
        /* Scale based on width */
        scaled_w = fb_width;
        scaled_h = (img->height * fb_width) / img->width;
    } else {
        /* Scale based on height */
        scaled_h = fb_height;
        scaled_w = (img->width * fb_height) / img->height;
    }

    /* Center the image */
    int x = ((int)fb_width - (int)scaled_w) / 2;
    int y = ((int)fb_height - (int)scaled_h) / 2;

    image_draw_scaled(fb, fb_width, fb_height, img, x, y, scaled_w, scaled_h);
}

/* BMP file format structures */
#pragma pack(push, 1)
typedef struct {
    uint16_t type;          /* "BM" */
    uint32_t size;
    uint16_t reserved1;
    uint16_t reserved2;
    uint32_t offset;        /* Offset to pixel data */
} bmp_file_header_t;

typedef struct {
    uint32_t size;          /* Header size (40 for BITMAPINFOHEADER) */
    int32_t width;
    int32_t height;         /* Negative = top-down */
    uint16_t planes;
    uint16_t bpp;           /* Bits per pixel */
    uint32_t compression;
    uint32_t image_size;
    int32_t x_ppm;
    int32_t y_ppm;
    uint32_t colors_used;
    uint32_t colors_important;
} bmp_info_header_t;
#pragma pack(pop)

/* Parse BMP from memory */
int bmp_parse(const uint8_t* data, uint32_t size, bmp_image_t* out) {
    if (!data || !out || size < 54) {
        if (out) out->valid = 0;
        return -1;
    }

    out->valid = 0;
    out->data = NULL;

    /* Check magic */
    if (data[0] != 'B' || data[1] != 'M') {
        return -1;  /* Not a BMP */
    }

    bmp_file_header_t* fh = (bmp_file_header_t*)data;
    bmp_info_header_t* ih = (bmp_info_header_t*)(data + 14);

    /* Only support common formats */
    if (ih->bpp != 24 && ih->bpp != 32) {
        return -1;  /* Only 24 or 32 bit BMPs */
    }

    if (ih->compression != 0) {
        return -1;  /* No compression support */
    }

    int width = ih->width;
    int height = ih->height;
    int top_down = 0;

    if (height < 0) {
        height = -height;
        top_down = 1;
    }

    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) {
        return -1;  /* Invalid dimensions */
    }

    /* Allocate pixel buffer */
    out->data = (uint32_t*)malloc(width * height * 4);
    if (!out->data) {
        return -1;  /* Out of memory */
    }

    /* Row stride (BMP rows are padded to 4-byte boundary) */
    int bytes_per_pixel = ih->bpp / 8;
    int row_stride = ((width * bytes_per_pixel + 3) / 4) * 4;

    const uint8_t* pixels = data + fh->offset;

    for (int y = 0; y < height; y++) {
        int src_y = top_down ? y : (height - 1 - y);
        const uint8_t* row = pixels + src_y * row_stride;

        for (int x = 0; x < width; x++) {
            uint8_t b = row[x * bytes_per_pixel + 0];
            uint8_t g = row[x * bytes_per_pixel + 1];
            uint8_t r = row[x * bytes_per_pixel + 2];
            /* BMP is BGR, framebuffer is 0x00RRGGBB */
            out->data[y * width + x] = (r << 16) | (g << 8) | b;
        }
    }

    out->width = width;
    out->height = height;
    out->valid = 1;

    return 0;
}

/* Free BMP image data */
void bmp_free(bmp_image_t* img) {
    if (img && img->data) {
        free(img->data);
        img->data = NULL;
        img->valid = 0;
    }
}
