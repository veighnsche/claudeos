#ifndef GOLDFISH_FB_H
#define GOLDFISH_FB_H

#include "types.h"

#ifdef __aarch64__
/* ARM64 Android Emulator (Ranchu) uses virtio-gpu */
/* For now, use a simple framebuffer at fixed address */
#define FRAMEBUFFER_ADDR    0x50000000
#define FB_WIDTH            720
#define FB_HEIGHT           1280
#define FB_BPP              32

/* Virtio-GPU base (simplified - real init is complex) */
#define VIRTIO_GPU_BASE     0x0a003000

#else
/* ARM32 VersatilePB uses PL110 CLCD */
#define PL110_BASE          0x10120000
#define FRAMEBUFFER_ADDR    0x01000000
#define FB_WIDTH            640
#define FB_HEIGHT           480
#define FB_BPP              32

/* PL110 Register offsets */
#define CLCD_TIM0           0x00
#define CLCD_TIM1           0x04
#define CLCD_TIM2           0x08
#define CLCD_TIM3           0x0C
#define CLCD_UBAS           0x10
#define CLCD_LBAS           0x14
#define CLCD_CNTL           0x18
#define CLCD_IENB           0x1C

/* Control register bits */
#define CLCD_CNTL_LCDEN     (1 << 0)
#define CLCD_CNTL_LCDBPP16  (4 << 1)
#define CLCD_CNTL_LCDTFT    (1 << 5)
#define CLCD_CNTL_BGR       (1 << 8)
#define CLCD_CNTL_LCDPWR    (1 << 11)
#endif

void goldfish_fb_init(void);
void goldfish_fb_clear(uint32_t color);
void goldfish_fb_putpixel(int x, int y, uint32_t color);
void goldfish_fb_flush(void);
uint32_t* goldfish_fb_get_buffer(void);
uint32_t goldfish_fb_get_width(void);
uint32_t goldfish_fb_get_height(void);

#endif