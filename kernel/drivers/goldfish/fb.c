#include "goldfish_fb.h"

#ifdef __aarch64__
/*
 * ARM64 Android Emulator uses virtio-gpu
 */

/* External virtio-gpu functions */
extern void virtio_gpu_init(void);
extern void virtio_gpu_flush(void);
extern uint32_t* virtio_gpu_get_framebuffer(void);
extern uint32_t virtio_gpu_get_width(void);
extern uint32_t virtio_gpu_get_height(void);

static uint32_t* framebuffer;

void goldfish_fb_init(void) {
    virtio_gpu_init();
    framebuffer = virtio_gpu_get_framebuffer();
}

void goldfish_fb_clear(uint32_t color) {
    uint32_t w = virtio_gpu_get_width();
    uint32_t h = virtio_gpu_get_height();
    for (uint32_t i = 0; i < w * h; i++) {
        framebuffer[i] = color;
    }
    /* Don't flush here - caller should flush after all drawing is complete */
}

void goldfish_fb_putpixel(int x, int y, uint32_t color) {
    uint32_t w = virtio_gpu_get_width();
    uint32_t h = virtio_gpu_get_height();
    if (x >= 0 && x < (int)w && y >= 0 && y < (int)h) {
        framebuffer[y * w + x] = color;
    }
}

uint32_t* goldfish_fb_get_buffer(void) {
    return framebuffer;
}

uint32_t goldfish_fb_get_width(void) {
    return virtio_gpu_get_width();
}

uint32_t goldfish_fb_get_height(void) {
    return virtio_gpu_get_height();
}

void goldfish_fb_flush(void) {
    virtio_gpu_flush();
}

#else
/* ARM32 VersatilePB uses PL110 CLCD */

static uint32_t* framebuffer = (uint32_t*)FRAMEBUFFER_ADDR;

#define SYS_BASE        0x10000000
#define SYS_CLCD        0x50

static void clcd_write(uint32_t offset, uint32_t value) {
    *(volatile uint32_t*)(PL110_BASE + offset) = value;
}

void goldfish_fb_init(void) {
    volatile uint32_t* fb = (volatile uint32_t*)FRAMEBUFFER_ADDR;
    for (uint32_t i = 0; i < FB_WIDTH * FB_HEIGHT; i++) {
        fb[i] = 0x00440000;
    }

    clcd_write(CLCD_TIM0, 0x3F1F3F9C);
    clcd_write(CLCD_TIM1, 0x090B61DF);
    clcd_write(CLCD_TIM2, 0x067F1800);
    clcd_write(CLCD_TIM3, 0);
    clcd_write(CLCD_UBAS, FRAMEBUFFER_ADDR);
    clcd_write(CLCD_CNTL, 0x082B);
}

void goldfish_fb_clear(uint32_t color) {
    for (uint32_t i = 0; i < FB_WIDTH * FB_HEIGHT; i++) {
        framebuffer[i] = color;
    }
}

void goldfish_fb_putpixel(int x, int y, uint32_t color) {
    if (x >= 0 && x < (int)FB_WIDTH && y >= 0 && y < (int)FB_HEIGHT) {
        framebuffer[y * FB_WIDTH + x] = color;
    }
}

uint32_t* goldfish_fb_get_buffer(void) {
    return framebuffer;
}

uint32_t goldfish_fb_get_width(void) {
    return FB_WIDTH;
}

uint32_t goldfish_fb_get_height(void) {
    return FB_HEIGHT;
}

#endif
