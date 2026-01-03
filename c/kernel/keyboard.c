/*
 * ClaudeOS Soft Keyboard Implementation
 */

#include "keyboard.h"
#include "font.h"
#include "event.h"

/* Keyboard dimensions */
#define KEY_ROWS        4
#define KEY_COLS        10
#define KEY_HEIGHT      45
#define KEY_SPACING     4
#define KB_PADDING      8

/* Colors */
#define KB_BG           0x202030
#define KEY_BG          0x404050
#define KEY_BG_PRESS    0x606080
#define KEY_BORDER      0x505060
#define KEY_TEXT        0xFFFFFF
#define KEY_SPECIAL_BG  0x353545

/* Keyboard state */
static int kb_visible = 0;
static int kb_shift = 0;
static char last_char = 0;
static int pressed_row = -1;
static int pressed_col = -1;
static int touch_active = 0;

/* Screen info */
static uint32_t scr_w = 0;
static uint32_t scr_h = 0;
static int kb_y = 0;
static int kb_height = 0;
static int key_width = 0;

/* Key layouts */
static const char* keys_lower[KEY_ROWS] = {
    "1234567890",
    "qwertyuiop",
    "asdfghjkl;",
    "zxcvbnm,./"
};

static const char* keys_upper[KEY_ROWS] = {
    "!@#$%^&*()",
    "QWERTYUIOP",
    "ASDFGHJKL:",
    "ZXCVBNM<>?"
};

/* Special keys info */
#define SPECIAL_NONE    0
#define SPECIAL_SHIFT   1
#define SPECIAL_BACK    2
#define SPECIAL_SPACE   3
#define SPECIAL_ENTER   4

void keyboard_init(uint32_t screen_width, uint32_t screen_height) {
    scr_w = screen_width;
    scr_h = screen_height;

    /* Calculate dimensions */
    kb_height = KEY_ROWS * KEY_HEIGHT + (KEY_ROWS + 1) * KEY_SPACING + KB_PADDING * 2 + KEY_HEIGHT + KEY_SPACING; /* Extra row for space/special */
    kb_y = scr_h - kb_height;
    key_width = (scr_w - KB_PADDING * 2 - (KEY_COLS + 1) * KEY_SPACING) / KEY_COLS;

    kb_visible = 0;
    kb_shift = 0;
    last_char = 0;
}

void keyboard_show(void) {
    kb_visible = 1;
}

void keyboard_hide(void) {
    kb_visible = 0;
}

int keyboard_is_visible(void) {
    return kb_visible;
}

void keyboard_toggle(void) {
    kb_visible = !kb_visible;
}

int keyboard_get_height(void) {
    return kb_visible ? kb_height : 0;
}

/* Get key at screen position */
static int get_key_at(int sx, int sy, int* out_row, int* out_col, int* out_special) {
    *out_special = SPECIAL_NONE;
    *out_row = -1;
    *out_col = -1;

    if (!kb_visible) return 0;
    if (sy < kb_y || sy >= (int)scr_h) return 0;

    int rel_y = sy - kb_y - KB_PADDING;
    int rel_x = sx - KB_PADDING;

    /* Check main key rows */
    for (int row = 0; row < KEY_ROWS; row++) {
        int row_y = row * (KEY_HEIGHT + KEY_SPACING);
        if (rel_y >= row_y && rel_y < row_y + KEY_HEIGHT) {
            /* In this row */
            for (int col = 0; col < KEY_COLS; col++) {
                int key_x = col * (key_width + KEY_SPACING);
                if (rel_x >= key_x && rel_x < key_x + key_width) {
                    *out_row = row;
                    *out_col = col;
                    return 1;
                }
            }
        }
    }

    /* Check special row (bottom) */
    int special_y = KEY_ROWS * (KEY_HEIGHT + KEY_SPACING);
    if (rel_y >= special_y && rel_y < special_y + KEY_HEIGHT) {
        int total_w = scr_w - KB_PADDING * 2;

        /* Calculate widths first */
        int shift_w = key_width + key_width / 2;
        int back_w = key_width + 10;
        int enter_w = key_width + key_width / 2;
        int space_w = total_w - shift_w - back_w - enter_w - 3 * KEY_SPACING;

        /* Shift key (left) */
        int shift_x = 0;
        if (rel_x >= shift_x && rel_x < shift_x + shift_w) {
            *out_special = SPECIAL_SHIFT;
            return 1;
        }

        /* Space bar (center) */
        int space_x = shift_x + shift_w + KEY_SPACING;
        if (rel_x >= space_x && rel_x < space_x + space_w) {
            *out_special = SPECIAL_SPACE;
            return 1;
        }

        /* Backspace */
        int back_x = space_x + space_w + KEY_SPACING;
        if (rel_x >= back_x && rel_x < back_x + back_w) {
            *out_special = SPECIAL_BACK;
            return 1;
        }

        /* Enter */
        int enter_x = back_x + back_w + KEY_SPACING;
        if (rel_x >= enter_x && rel_x < enter_x + enter_w) {
            *out_special = SPECIAL_ENTER;
            return 1;
        }
    }

    return 0;
}

int keyboard_handle_touch(int touch_type, int32_t x, int32_t y) {
    if (!kb_visible) return 0;

    /* Scale touch coords */
    int sx = (x * scr_w) / 32768;
    int sy = (y * scr_h) / 32768;

    /* Check if touch is in keyboard area */
    if (sy < kb_y) return 0;

    int row, col, special;

    if (touch_type == TOUCH_DOWN) {
        if (get_key_at(sx, sy, &row, &col, &special)) {
            pressed_row = row;
            pressed_col = col;
            touch_active = 1;

            if (special != SPECIAL_NONE) {
                pressed_row = -1;
                pressed_col = special + 100;  /* Encode special key */
            }
        }
        return 1;
    }
    else if (touch_type == TOUCH_UP) {
        if (touch_active) {
            if (get_key_at(sx, sy, &row, &col, &special)) {
                if (special == SPECIAL_SHIFT) {
                    kb_shift = !kb_shift;
                }
                else if (special == SPECIAL_SPACE) {
                    last_char = ' ';
                }
                else if (special == SPECIAL_BACK) {
                    last_char = '\b';
                }
                else if (special == SPECIAL_ENTER) {
                    last_char = '\n';
                }
                else if (row >= 0 && col >= 0) {
                    const char* layout = kb_shift ? keys_upper[row] : keys_lower[row];
                    last_char = layout[col];
                    /* Auto-unshift after typing */
                    if (kb_shift && last_char >= 'A' && last_char <= 'Z') {
                        kb_shift = 0;
                    }
                }
            }
            pressed_row = -1;
            pressed_col = -1;
            touch_active = 0;
        }
        return 1;
    }
    else if (touch_type == TOUCH_MOVE) {
        return touch_active ? 1 : (sy >= kb_y);
    }

    return 1;
}

char keyboard_get_char(void) {
    char c = last_char;
    last_char = 0;
    return c;
}

/* Blend color with alpha */
static uint32_t blend(uint32_t bg, uint32_t fg, int alpha) {
    int bg_r = (bg >> 16) & 0xFF;
    int bg_g = (bg >> 8) & 0xFF;
    int bg_b = bg & 0xFF;
    int fg_r = (fg >> 16) & 0xFF;
    int fg_g = (fg >> 8) & 0xFF;
    int fg_b = fg & 0xFF;
    int r = (fg_r * alpha + bg_r * (255 - alpha)) / 255;
    int g = (fg_g * alpha + bg_g * (255 - alpha)) / 255;
    int b = (fg_b * alpha + bg_b * (255 - alpha)) / 255;
    return (r << 16) | (g << 8) | b;
}

/* Draw filled rectangle with alpha */
static void draw_rect_a(uint32_t* fb, int x, int y, int w, int h, uint32_t color, int alpha) {
    for (int py = y; py < y + h && py < (int)scr_h; py++) {
        if (py < 0) continue;
        for (int px = x; px < x + w && px < (int)scr_w; px++) {
            if (px < 0) continue;
            if (alpha >= 255) {
                fb[py * scr_w + px] = color;
            } else {
                fb[py * scr_w + px] = blend(fb[py * scr_w + px], color, alpha);
            }
        }
    }
}

/* Draw key with rounded corners (simplified) */
static void draw_key(uint32_t* fb, int x, int y, int w, int h, uint32_t bg, int pressed) {
    uint32_t color = pressed ? KEY_BG_PRESS : bg;
    int r = 6;

    /* Main rectangle */
    draw_rect_a(fb, x + r, y, w - 2 * r, h, color, 220);
    draw_rect_a(fb, x, y + r, w, h - 2 * r, color, 220);

    /* Rounded corners (simplified as filled circles) */
    for (int py = 0; py < r; py++) {
        for (int px = 0; px < r; px++) {
            int dx = r - 1 - px;
            int dy = r - 1 - py;
            if (dx * dx + dy * dy <= r * r) {
                /* Top-left */
                if (x + px >= 0 && y + py >= 0)
                    fb[(y + py) * scr_w + x + px] = blend(fb[(y + py) * scr_w + x + px], color, 220);
                /* Top-right */
                if (x + w - 1 - px >= 0 && y + py >= 0)
                    fb[(y + py) * scr_w + x + w - 1 - px] = blend(fb[(y + py) * scr_w + x + w - 1 - px], color, 220);
                /* Bottom-left */
                if (x + px >= 0 && y + h - 1 - py >= 0)
                    fb[(y + h - 1 - py) * scr_w + x + px] = blend(fb[(y + h - 1 - py) * scr_w + x + px], color, 220);
                /* Bottom-right */
                if (x + w - 1 - px >= 0 && y + h - 1 - py >= 0)
                    fb[(y + h - 1 - py) * scr_w + x + w - 1 - px] = blend(fb[(y + h - 1 - py) * scr_w + x + w - 1 - px], color, 220);
            }
        }
    }
}

/* Draw centered text on key */
static void draw_key_label(uint32_t* fb, int x, int y, int w, int h, const char* label) {
    int len = 0;
    while (label[len]) len++;
    int tx = x + (w - len * FONT_WIDTH) / 2;
    int ty = y + (h - FONT_HEIGHT) / 2;
    draw_string(fb, tx, ty, label, KEY_TEXT, scr_w, scr_h);
}

void keyboard_draw(uint32_t* fb, uint32_t fb_width, uint32_t fb_height) {
    if (!kb_visible) return;

    (void)fb_width;
    (void)fb_height;

    /* Draw keyboard background */
    draw_rect_a(fb, 0, kb_y, scr_w, kb_height, KB_BG, 230);

    /* Top border */
    draw_rect_a(fb, 0, kb_y, scr_w, 1, 0x606070, 255);

    /* Draw main keys */
    const char** layout = kb_shift ? keys_upper : keys_lower;
    char label[2] = {0, 0};

    for (int row = 0; row < KEY_ROWS; row++) {
        for (int col = 0; col < KEY_COLS; col++) {
            int kx = KB_PADDING + col * (key_width + KEY_SPACING);
            int ky = kb_y + KB_PADDING + row * (KEY_HEIGHT + KEY_SPACING);

            int pressed = (pressed_row == row && pressed_col == col);
            draw_key(fb, kx, ky, key_width, KEY_HEIGHT, KEY_BG, pressed);

            label[0] = layout[row][col];
            draw_key_label(fb, kx, ky, key_width, KEY_HEIGHT, label);
        }
    }

    /* Draw special row */
    int special_y = kb_y + KB_PADDING + KEY_ROWS * (KEY_HEIGHT + KEY_SPACING);
    int total_w = scr_w - KB_PADDING * 2;
    int shift_w = key_width + key_width / 2;
    int back_w = key_width + 10;
    int enter_w = key_width + key_width / 2;
    int space_w = total_w - shift_w - back_w - enter_w - 3 * KEY_SPACING;

    /* Shift */
    int shift_x = KB_PADDING;
    draw_key(fb, shift_x, special_y, shift_w, KEY_HEIGHT, kb_shift ? KEY_BG_PRESS : KEY_SPECIAL_BG, pressed_col == SPECIAL_SHIFT + 100);
    draw_key_label(fb, shift_x, special_y, shift_w, KEY_HEIGHT, kb_shift ? "SHIFT" : "Shift");

    /* Space */
    int space_x = shift_x + shift_w + KEY_SPACING;
    draw_key(fb, space_x, special_y, space_w, KEY_HEIGHT, KEY_BG, pressed_col == SPECIAL_SPACE + 100);
    draw_key_label(fb, space_x, special_y, space_w, KEY_HEIGHT, "Space");

    /* Backspace */
    int back_x = space_x + space_w + KEY_SPACING;
    draw_key(fb, back_x, special_y, back_w, KEY_HEIGHT, KEY_SPECIAL_BG, pressed_col == SPECIAL_BACK + 100);
    draw_key_label(fb, back_x, special_y, back_w, KEY_HEIGHT, "Del");

    /* Enter */
    int enter_x = back_x + back_w + KEY_SPACING;
    draw_key(fb, enter_x, special_y, enter_w, KEY_HEIGHT, KEY_SPECIAL_BG, pressed_col == SPECIAL_ENTER + 100);
    draw_key_label(fb, enter_x, special_y, enter_w, KEY_HEIGHT, "Go");
}
