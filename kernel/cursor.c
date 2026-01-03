#include "cursor.h"
#include "virtio_input.h"

void cursor_draw(uint32_t *fb, uint32_t screen_w, uint32_t screen_h) {
  int32_t cursor_px, cursor_py;
  virtio_input_get_touch(&cursor_px, &cursor_py, (void *)0);

  /* cursor_px/py are in pixel coordinates */
  if (cursor_px >= 0 && cursor_px < (int32_t)screen_w && cursor_py >= 0 &&
      cursor_py < (int32_t)screen_h) {
    /* Draw simple arrow cursor - white with black outline */
    int cx = cursor_px;
    int cy = cursor_py;

    /* Cursor shape: small triangle pointer */
    for (int dy = 0; dy < 12; dy++) {
      int width = (dy < 8) ? (dy / 2 + 1) : (12 - dy);
      for (int dx = 0; dx < width; dx++) {
        int px = cx + dx;
        int py = cy + dy;
        if (px >= 0 && px < (int)screen_w && py >= 0 && py < (int)screen_h) {
          /* White fill */
          fb[py * screen_w + px] = 0x00FFFFFF;
        }
      }
    }

    /* Black outline on right edge */
    for (int dy = 0; dy < 12; dy++) {
      int width = (dy < 8) ? (dy / 2 + 1) : (12 - dy);
      int px = cx + width;
      int py = cy + dy;
      if (px >= 0 && px < (int)screen_w && py >= 0 && py < (int)screen_h) {
        fb[py * screen_w + px] = 0x00000000;
      }
    }
  }
}
