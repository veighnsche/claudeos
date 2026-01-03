/*
 * ClaudeOS Soft Keyboard
 */

#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "types.h"

/* Initialize keyboard */
void keyboard_init(uint32_t screen_width, uint32_t screen_height);

/* Show/hide keyboard */
void keyboard_show(void);
void keyboard_hide(void);
int keyboard_is_visible(void);

/* Toggle keyboard visibility */
void keyboard_toggle(void);

/* Handle touch events - returns 1 if keyboard consumed the touch */
int keyboard_handle_touch(int touch_type, int32_t x, int32_t y);

/* Get last pressed character (0 if none) */
char keyboard_get_char(void);

/* Draw keyboard to framebuffer */
void keyboard_draw(uint32_t* fb, uint32_t fb_width, uint32_t fb_height);

/* Get keyboard height (for adjusting terminal size) */
int keyboard_get_height(void);

#endif /* KEYBOARD_H */
