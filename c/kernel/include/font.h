#ifndef FONT_H
#define FONT_H

#include "types.h"

#define FONT_WIDTH 8
#define FONT_HEIGHT 12

extern const uint8_t font_8x12[][12];

void draw_char(uint32_t* fb, int x, int y, char c, uint32_t color, int fb_width, int fb_height);
void draw_string(uint32_t* fb, int x, int y, const char* str, uint32_t color, int fb_width, int fb_height);

#endif