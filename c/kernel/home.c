/*
 * ClaudeOS Home Screen
 * Shows animated logo and macOS-style dock
 */

#include "home.h"
#include "event.h"
#include "font.h"
#include "goldfish_fb.h"
#include "image.h"
#include "images/background.h"

/* Background image */
static const image_t background_img = {.width = BACKGROUND_WIDTH,
                                       .height = BACKGROUND_HEIGHT,
                                       .data = background_data};

/* Colors - designed for dark purple/blue/cyan background */
#define HOME_TEXT 0x00FFFFFF     /* Bright white text */
#define HOME_TEXT_DIM 0x00CCCCDD /* Dimmed white */
#define ICON_BG 0x00302040       /* Dark purple icon bg */
#define ICON_BORDER 0x00A070B0   /* Purple/magenta border */
#define ICON_PRESSED 0x00503060  /* Pressed state */
#define DOCK_BG 0x00201828       /* Dark translucent dock */
#define DOCK_BORDER 0x00403050   /* Dock border */

/* Dock dimensions */
#define DOCK_HEIGHT 70
#define DOCK_MARGIN 20
#define DOCK_RADIUS 20
#define ICON_SIZE 50

/* Icon positions (calculated in init) */
static int terminal_icon_x = 0;
static int files_icon_x = 0;
static int icon_y = 0;
static int terminal_pressed = 0;
static int files_pressed = 0;
static int terminal_touch_active = 0;
static int files_touch_active = 0;
static int needs_redraw = 1;

/* Screen dimensions */
static uint32_t screen_w = 0;
static uint32_t screen_h = 0;

/* Animation state */
static uint32_t anim_frame = 0;

/* Internet connection status */
static int internet_connected = 0;
static int anim_tick = 0;

void home_init(void) {
  screen_w = goldfish_fb_get_width();
  screen_h = goldfish_fb_get_height();

  /* Fallback for safety */
  if (screen_w == 0)
    screen_w = 360;
  if (screen_h == 0)
    screen_h = 640;

  /* Bottom bar dimensions */
  int bar_h = 80;
  int bar_y = screen_h - bar_h;

  /* Position two icons with spacing */
  int icon_spacing = 30;
  int total_width = ICON_SIZE * 2 + icon_spacing;
  int start_x = (screen_w - total_width) / 2;

  terminal_icon_x = start_x;
  files_icon_x = start_x + ICON_SIZE + icon_spacing;
  icon_y = bar_y + (bar_h - ICON_SIZE) / 2;

  terminal_pressed = 0;
  files_pressed = 0;
  terminal_touch_active = 0;
  files_touch_active = 0;
  needs_redraw = 1;
}

/* Check if point is inside a circular icon at given position */
static int point_in_icon_at(int32_t x, int32_t y, int ix) {
  /* Scale touch coords from 0-32767 to screen coords */
  int sx = (x * screen_w) / 32768;
  int sy = (y * screen_h) / 32768;

  /* Check circular bounds */
  int cx = ix + ICON_SIZE / 2;
  int cy = icon_y + ICON_SIZE / 2;
  int dx = sx - cx;
  int dy = sy - cy;
  int r = ICON_SIZE / 2;

  return (dx * dx + dy * dy <= r * r);
}

int home_update(void) {
  input_event_t ev;

  while (event_pop(&ev) == 0) {
    if (ev.type == EVENT_TOUCH) {
      if (ev.subtype == TOUCH_DOWN) {
        if (point_in_icon_at(ev.x, ev.y, terminal_icon_x)) {
          terminal_touch_active = 1;
          needs_redraw = 1;
        } else if (point_in_icon_at(ev.x, ev.y, files_icon_x)) {
          files_touch_active = 1;
          needs_redraw = 1;
        }
      } else if (ev.subtype == TOUCH_UP) {
        if (terminal_touch_active &&
            point_in_icon_at(ev.x, ev.y, terminal_icon_x)) {
          terminal_pressed = 1;
        }
        if (files_touch_active && point_in_icon_at(ev.x, ev.y, files_icon_x)) {
          files_pressed = 1;
        }
        terminal_touch_active = 0;
        files_touch_active = 0;
        needs_redraw = 1;
      } else if (ev.subtype == TOUCH_MOVE) {
        if (terminal_touch_active &&
            !point_in_icon_at(ev.x, ev.y, terminal_icon_x)) {
          terminal_touch_active = 0;
          needs_redraw = 1;
        }
        if (files_touch_active && !point_in_icon_at(ev.x, ev.y, files_icon_x)) {
          files_touch_active = 0;
          needs_redraw = 1;
        }
      }
    } else if (ev.type == EVENT_KEY) {
      if (ev.subtype == KEY_PRESS) {
        if (ev.code == 28 || ev.code == 57) {
          terminal_pressed = 1;
        }
      }
    }
  }

  /* Animation - update every ~100 ticks */
  anim_tick++;
  if (anim_tick >= 100) {
    anim_tick = 0;
    anim_frame++;
    return 1;
  }

  return needs_redraw;
}

/* Blend two colors with alpha (0-255) */
static uint32_t blend_color(uint32_t bg, uint32_t fg, int alpha) {
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

/* Draw a filled rectangle */
static void draw_rect(uint32_t *fb, int x, int y, int w, int h,
                      uint32_t color) {
  for (int py = y; py < y + h && py < (int)screen_h; py++) {
    if (py < 0)
      continue;
    for (int px = x; px < x + w && px < (int)screen_w; px++) {
      if (px < 0)
        continue;
      fb[py * screen_w + px] = color;
    }
  }
}

/* Draw a filled rectangle with alpha transparency */
static void draw_rect_alpha(uint32_t *fb, int x, int y, int w, int h,
                            uint32_t color, int alpha) {
  for (int py = y; py < y + h && py < (int)screen_h; py++) {
    if (py < 0)
      continue;
    for (int px = x; px < x + w && px < (int)screen_w; px++) {
      if (px < 0)
        continue;
      uint32_t bg = fb[py * screen_w + px];
      fb[py * screen_w + px] = blend_color(bg, color, alpha);
    }
  }
}

/* Draw a filled circle */
static void draw_circle(uint32_t *fb, int cx, int cy, int r, uint32_t color) {
  for (int py = cy - r; py <= cy + r; py++) {
    if (py < 0 || py >= (int)screen_h)
      continue;
    for (int px = cx - r; px <= cx + r; px++) {
      if (px < 0 || px >= (int)screen_w)
        continue;
      int dx = px - cx;
      int dy = py - cy;
      if (dx * dx + dy * dy <= r * r) {
        fb[py * screen_w + px] = color;
      }
    }
  }
}

/* Draw a filled circle with alpha */
static void draw_circle_alpha(uint32_t *fb, int cx, int cy, int r,
                              uint32_t color, int alpha) {
  for (int py = cy - r; py <= cy + r; py++) {
    if (py < 0 || py >= (int)screen_h)
      continue;
    for (int px = cx - r; px <= cx + r; px++) {
      if (px < 0 || px >= (int)screen_w)
        continue;
      int dx = px - cx;
      int dy = py - cy;
      if (dx * dx + dy * dy <= r * r) {
        uint32_t bg = fb[py * screen_w + px];
        fb[py * screen_w + px] = blend_color(bg, color, alpha);
      }
    }
  }
}

/* Draw circle border */
static void draw_circle_ring(uint32_t *fb, int cx, int cy, int r, int thickness,
                             uint32_t color) {
  int r_outer = r;
  int r_inner = r - thickness;
  for (int py = cy - r_outer; py <= cy + r_outer; py++) {
    if (py < 0 || py >= (int)screen_h)
      continue;
    for (int px = cx - r_outer; px <= cx + r_outer; px++) {
      if (px < 0 || px >= (int)screen_w)
        continue;
      int dx = px - cx;
      int dy = py - cy;
      int d2 = dx * dx + dy * dy;
      if (d2 <= r_outer * r_outer && d2 >= r_inner * r_inner) {
        fb[py * screen_w + px] = color;
      }
    }
  }
}

/* Draw rounded rectangle for dock */
static void draw_rounded_rect(uint32_t *fb, int x, int y, int w, int h, int r,
                              uint32_t color) {
  /* Main body */
  draw_rect(fb, x + r, y, w - 2 * r, h, color);
  draw_rect(fb, x, y + r, w, h - 2 * r, color);

  /* Corners */
  draw_circle(fb, x + r, y + r, r, color);
  draw_circle(fb, x + w - r - 1, y + r, r, color);
  draw_circle(fb, x + r, y + h - r - 1, r, color);
  draw_circle(fb, x + w - r - 1, y + h - r - 1, r, color);
}

/* Draw rounded rectangle with alpha transparency (no overlap) */
static void draw_rounded_rect_alpha(uint32_t *fb, int x, int y, int w, int h,
                                    int r, uint32_t color, int alpha) {
  for (int py = y; py < y + h; py++) {
    if (py < 0 || py >= (int)screen_h)
      continue;
    for (int px = x; px < x + w; px++) {
      if (px < 0 || px >= (int)screen_w)
        continue;

      int inside = 0;

      /* Check if in main rectangle (excluding corners) */
      if ((px >= x + r && px < x + w - r) || (py >= y + r && py < y + h - r)) {
        inside = 1;
      } else {
        /* Check corner circles */
        int dx, dy, d2, r2 = r * r;

        /* Top-left corner */
        dx = px - (x + r);
        dy = py - (y + r);
        if (dx < 0 && dy < 0 && dx * dx + dy * dy <= r2)
          inside = 1;

        /* Top-right corner */
        dx = px - (x + w - r - 1);
        dy = py - (y + r);
        if (dx > 0 && dy < 0 && dx * dx + dy * dy <= r2)
          inside = 1;

        /* Bottom-left corner */
        dx = px - (x + r);
        dy = py - (y + h - r - 1);
        if (dx < 0 && dy > 0 && dx * dx + dy * dy <= r2)
          inside = 1;

        /* Bottom-right corner */
        dx = px - (x + w - r - 1);
        dy = py - (y + h - r - 1);
        if (dx > 0 && dy > 0 && dx * dx + dy * dy <= r2)
          inside = 1;
      }

      if (inside) {
        uint32_t bg = fb[py * screen_w + px];
        fb[py * screen_w + px] = blend_color(bg, color, alpha);
      }
    }
  }
}

/* Draw circular terminal icon (semi-transparent) */
static void draw_terminal_icon(uint32_t *fb, int x, int y, int size,
                               int pressed) {
  int cx = x + size / 2;
  int cy = y + size / 2;
  int r = size / 2;

  /* Circular background with alpha */
  int alpha = pressed ? 200 : 160;
  uint32_t bg = pressed ? 0x402060 : 0x201030;
  draw_circle_alpha(fb, cx, cy, r, bg, alpha);

  /* Border ring */
  draw_circle_ring(fb, cx, cy, r, 2, ICON_BORDER);

  /* Shiny highlight on top */
  draw_circle_ring(fb, cx, cy - 2, r - 4, 1, 0x00806090);

  /* Draw ">_" text inside icon */
  draw_string(fb, x + size / 2 - 12, y + size / 2 - 6, ">_", HOME_TEXT,
              screen_w, screen_h);
}

/* Draw circular files icon (semi-transparent) */
static void draw_files_icon(uint32_t *fb, int x, int y, int size, int pressed) {
  int cx = x + size / 2;
  int cy = y + size / 2;
  int r = size / 2;

  /* Circular background with alpha - blue tint */
  int alpha = pressed ? 200 : 160;
  uint32_t bg = pressed ? 0x203060 : 0x102040;
  draw_circle_alpha(fb, cx, cy, r, bg, alpha);

  /* Border ring */
  draw_circle_ring(fb, cx, cy, r, 2, 0x0060A0E0);

  /* Shiny highlight on top */
  draw_circle_ring(fb, cx, cy - 2, r - 4, 1, 0x00608090);

  /* Draw folder icon inside */
  int fx = cx - 10;
  int fy = cy - 6;
  /* Folder tab */
  for (int py = fy; py < fy + 4; py++) {
    for (int px = fx; px < fx + 8; px++) {
      if (px >= 0 && py >= 0 && (uint32_t)px < screen_w &&
          (uint32_t)py < screen_h)
        fb[py * screen_w + px] = 0x00FFD700;
    }
  }
  /* Folder body */
  for (int py = fy + 3; py < fy + 14; py++) {
    for (int px = fx; px < fx + 20; px++) {
      if (px >= 0 && py >= 0 && (uint32_t)px < screen_w &&
          (uint32_t)py < screen_h)
        fb[py * screen_w + px] = 0x00FFD700;
    }
  }
}

/* Draw a scaled character (4x scale for big logo) */
static void draw_char_4x(uint32_t *fb, int x, int y, char c, uint32_t color) {
  if (c < 32 || c > 126)
    return;
  const uint8_t *glyph = font_8x12[(unsigned char)c];

  for (int row = 0; row < FONT_HEIGHT; row++) {
    uint8_t bits = glyph[row];
    for (int col = 0; col < FONT_WIDTH; col++) {
      if (bits & (0x80 >> col)) {
        /* Draw 4x4 pixel block */
        int px = x + col * 4;
        int py = y + row * 4;
        for (int dy = 0; dy < 4; dy++) {
          for (int dx = 0; dx < 4; dx++) {
            if (px + dx >= 0 && px + dx < (int)screen_w && py + dy >= 0 &&
                py + dy < (int)screen_h) {
              fb[(py + dy) * screen_w + px + dx] = color;
            }
          }
        }
      }
    }
  }
}

/* Draw shiny animated ClaudeOS logo (4x scale) */
static void draw_logo_shiny(uint32_t *fb, int x, int y, const char *str,
                            int phase) {
  int i = 0;

  while (str[i]) {
    /* Shiny gradient effect - white/cyan highlight that sweeps across */
    int char_pos = (phase + i * 8) % 200;

    int r, g, b;
    if (char_pos < 40) {
      /* Bright white/cyan shine */
      int t = char_pos;
      r = 200 + t;
      g = 220 + t / 2;
      b = 255;
    } else if (char_pos < 80) {
      /* Fade to cyan */
      int t = char_pos - 40;
      r = 240 - t * 3;
      g = 240 - t;
      b = 255;
    } else if (char_pos < 120) {
      /* Cyan to purple */
      int t = char_pos - 80;
      r = 120 + t * 2;
      g = 200 - t * 2;
      b = 255;
    } else {
      /* Purple back to white-ish */
      int t = char_pos - 120;
      r = 200 + (t * 40) / 80;
      g = 120 + (t * 100) / 80;
      b = 255;
    }

    if (r > 255)
      r = 255;
    if (g > 255)
      g = 255;
    if (b > 255)
      b = 255;

    uint32_t color = (r << 16) | (g << 8) | b;

    /* Draw glow/shadow layers for shiny effect */
    uint32_t glow = 0x00400060; /* Purple glow */
    draw_char_4x(fb, x + i * FONT_WIDTH * 4 + 3, y + 3, str[i], glow);
    draw_char_4x(fb, x + i * FONT_WIDTH * 4 + 2, y + 2, str[i], 0x00000000);

    /* Main character */
    draw_char_4x(fb, x + i * FONT_WIDTH * 4, y, str[i], color);
    i++;
  }
}

void home_draw(void) {
  uint32_t *fb = goldfish_fb_get_buffer();

  if (screen_w == 0 || screen_h == 0) {
    screen_w = goldfish_fb_get_width();
    screen_h = goldfish_fb_get_height();
    if (screen_w == 0)
      screen_w = 360;
    if (screen_h == 0)
      screen_h = 640;
  }

  /* Solid dark background (temp: background image disabled) */
  goldfish_fb_clear(0x001A1A2E);

  /* Calculate logo dimensions (4x scale) */
  const char *logo = "ClaudeOS";
  int logo_len = 8;
  int logo_w = logo_len * FONT_WIDTH * 4; /* 4x width */
  int logo_h = FONT_HEIGHT * 4;
  int logo_x = (screen_w - logo_w) / 2;
  int logo_y =
      (screen_h - logo_h) / 2 - 80; /* Centered vertically, higher up */

  /* Dark transparent panel behind logo (50% opacity) */
  int panel_pad = 20;
  int panel_x = logo_x - panel_pad;
  int panel_y = logo_y - panel_pad;
  int panel_w = logo_w + panel_pad * 2;
  int panel_h = logo_h + FONT_HEIGHT + 30 + panel_pad * 2; /* Include tagline */
  draw_rounded_rect_alpha(fb, panel_x, panel_y, panel_w, panel_h, 15, 0x000000,
                          140);

  /* Big shiny logo */
  draw_logo_shiny(fb, logo_x, logo_y, logo, anim_frame * 3);

  /* Tagline under logo */
  const char *tagline = "AI-First OS";
  int tag_len = 11;
  int tag_x = (screen_w - tag_len * FONT_WIDTH) / 2;
  draw_string(fb, tag_x, logo_y + logo_h + 12, tagline, HOME_TEXT_DIM, screen_w,
              screen_h);

  /* Internet connection status */
  if (internet_connected) {
    const char *conn_msg = "Connected to Internet";
    int conn_len = 21;
    int conn_x = (screen_w - conn_len * FONT_WIDTH) / 2;
    draw_string(fb, conn_x, logo_y + logo_h + 32, conn_msg, 0x0000FF88,
                screen_w, screen_h);
  }

  /* Full-width bottom bar (transparent) */
  int bar_h = 80;
  int bar_y = screen_h - bar_h;
  draw_rect_alpha(fb, 0, bar_y, screen_w, bar_h, 0x000000, 140);

  /* Bar top border/highlight */
  draw_rect_alpha(fb, 0, bar_y, screen_w, 1, 0x808080, 80);

  /* Icon Y position in bottom bar */
  int icon_cy = bar_y + (bar_h - ICON_SIZE) / 2 - 8;

  /* Draw terminal icon */
  draw_terminal_icon(fb, terminal_icon_x, icon_cy, ICON_SIZE,
                     terminal_touch_active);
  int term_label_x = terminal_icon_x + (ICON_SIZE - 8 * FONT_WIDTH) / 2;
  draw_string(fb, term_label_x, icon_cy + ICON_SIZE + 2, "Terminal",
              HOME_TEXT_DIM, screen_w, screen_h);

  /* Draw files icon */
  draw_files_icon(fb, files_icon_x, icon_cy, ICON_SIZE, files_touch_active);
  int files_label_x = files_icon_x + (ICON_SIZE - 5 * FONT_WIDTH) / 2;
  draw_string(fb, files_label_x, icon_cy + ICON_SIZE + 2, "Files",
              HOME_TEXT_DIM, screen_w, screen_h);

  needs_redraw = 0;
}

int home_terminal_pressed(void) { return terminal_pressed; }

int home_files_pressed(void) { return files_pressed; }

void home_clear_pressed(void) {
  terminal_pressed = 0;
  files_pressed = 0;
}

void home_set_external_ip(const char *ip) {
  (void)ip; /* IP not displayed, just mark as connected */
  internet_connected = 1;
}
