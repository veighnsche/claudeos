/*
 * ClaudeOS File Manager
 * GUI app for browsing and managing files
 */

#include "filemanager.h"
#include "fs.h"
#include "event.h"
#include "goldfish_fb.h"
#include "font.h"
#include "types.h"
#include "keyboard.h"
#include "virtio_input.h"

/* UI Configuration */
#define TITLE_BAR_HEIGHT    40
#define FILE_ROW_HEIGHT     50
#define FILE_ICON_SIZE      32
#define FILE_PADDING        10
#define MAX_VISIBLE_FILES   10

/* Colors - Blue theme */
#define COLOR_BG            0x001A1A2E  /* Dark blue background */
#define COLOR_TITLE_BG      0x0016213E  /* Darker blue title */
#define COLOR_TITLE_TEXT    0x0000D4FF  /* Cyan title */
#define COLOR_FILE_BG       0x00202040  /* File row background */
#define COLOR_FILE_BG_SEL   0x00304060  /* Selected file */
#define COLOR_FILE_TEXT     0x00FFFFFF  /* White text */
#define COLOR_FILE_SIZE     0x00888888  /* Gray size text */
#define COLOR_FOLDER        0x00FFD700  /* Gold for folders */
#define COLOR_FILE          0x0000D4FF  /* Cyan for files */
#define COLOR_EMPTY         0x00666666  /* Gray for empty */
#define COLOR_BTN           0x00303050  /* Button background */
#define COLOR_BTN_PRESS     0x00505080  /* Button pressed */
#define COLOR_ERROR         0x00FF4444  /* Red for errors */

/* State */
static int want_close = 0;
static int needs_redraw = 1;
static int back_btn_pressed = 0;

/* File list */
static fs_dirent_t files[32];
static int file_count = 0;
static int selected_file = -1;
static int scroll_offset = 0;

/* Touch state */
static int touch_active = 0;
static int touch_file_idx = -1;

/* Action buttons state */
static int add_btn_pressed = 0;
static int del_btn_pressed = 0;
static int edit_btn_pressed = 0;
static int save_btn_pressed = 0;
static int selected_for_action = -1;  /* File selected for delete */

/* Viewing/Editing file content */
static int viewing_file = 0;
static int editing_file = 0;
static char view_content[512];
static int view_content_len = 0;
static char view_filename[24];
static int edit_cursor = 0;

/* Screen dimensions */
static uint32_t screen_w = 0;
static uint32_t screen_h = 0;

/* Status message */
static char status_msg[64] = "";
static int status_is_error = 0;

/* Hardware keyboard state */
static int shift_held = 0;

/* Convert keycode to character */
static char keycode_to_char(uint16_t code) {
    static const char lower[] = {
        0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0, 0,
        'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 0, 0,
        'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\',
        'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '
    };
    static const char upper[] = {
        0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0, 0,
        'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 0, 0,
        'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|',
        'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '
    };
    if (code >= sizeof(lower)) return 0;
    return shift_held ? upper[code] : lower[code];
}

/* Forward declarations */
static void refresh_file_list(void);
static void view_file(int idx);

/* Draw back arrow */
static void draw_back_arrow(uint32_t* fb, int cx, int cy, uint32_t color) {
    int arrow_size = 8;
    for (int i = 0; i < arrow_size; i++) {
        int px = cx + i;
        int py_top = cy - i;
        int py_bot = cy + i;
        if (px >= 0 && py_top >= 0 && (uint32_t)px < screen_w && (uint32_t)py_top < screen_h)
            fb[py_top * screen_w + px] = color;
        if (px >= 0 && py_bot >= 0 && (uint32_t)px < screen_w && (uint32_t)py_bot < screen_h)
            fb[py_bot * screen_w + px] = color;
        if (i > 0 && px + 1 < (int)screen_w) {
            if (py_top >= 0 && (uint32_t)py_top < screen_h)
                fb[py_top * screen_w + px + 1] = color;
            if (py_bot >= 0 && (uint32_t)py_bot < screen_h)
                fb[py_bot * screen_w + px + 1] = color;
        }
    }
}

/* Draw a filled rectangle */
static void fill_rect(uint32_t* fb, int x, int y, int w, int h, uint32_t color) {
    for (int py = y; py < y + h && py < (int)screen_h; py++) {
        if (py < 0) continue;
        for (int px = x; px < x + w && px < (int)screen_w; px++) {
            if (px < 0) continue;
            fb[py * screen_w + px] = color;
        }
    }
}

/* Draw a file/folder icon */
static void draw_file_icon(uint32_t* fb, int x, int y, int is_folder, uint32_t color) {
    if (is_folder) {
        /* Folder icon - rectangle with tab */
        fill_rect(fb, x, y + 6, 24, 18, color);
        fill_rect(fb, x, y + 4, 10, 4, color);
    } else {
        /* File icon - rectangle with folded corner */
        fill_rect(fb, x + 2, y + 2, 18, 24, color);
        fill_rect(fb, x + 14, y + 2, 6, 6, COLOR_BG);
        for (int i = 0; i < 6; i++) {
            int py = y + 2 + i;
            int px = x + 14 + i;
            if (py >= 0 && px >= 0 && (uint32_t)py < screen_h && (uint32_t)px < screen_w) {
                fb[py * screen_w + px] = color;
            }
        }
    }
}

/* Draw title bar */
static void draw_title_bar(uint32_t* fb) {
    /* Background */
    fill_rect(fb, 0, 0, screen_w, TITLE_BAR_HEIGHT, COLOR_TITLE_BG);

    /* Back button */
    int btn_size = 32;
    int btn_x = 10;
    int btn_y = (TITLE_BAR_HEIGHT - btn_size) / 2;
    int btn_cx = btn_x + btn_size / 2;
    int btn_cy = btn_y + btn_size / 2;
    int btn_r = btn_size / 2;

    uint32_t btn_color = back_btn_pressed ? COLOR_BTN_PRESS : COLOR_BTN;
    uint32_t arrow_color = back_btn_pressed ? COLOR_TITLE_TEXT : 0x00FFFFFF;

    /* Draw circular button */
    for (int py = btn_y; py < btn_y + btn_size; py++) {
        for (int px = btn_x; px < btn_x + btn_size; px++) {
            int dx = px - btn_cx;
            int dy = py - btn_cy;
            if (dx * dx + dy * dy <= btn_r * btn_r) {
                fb[py * screen_w + px] = btn_color;
            }
        }
    }

    draw_back_arrow(fb, btn_cx - 3, btn_cy, arrow_color);

    /* Title */
    const char* title = editing_file ? "Edit" : (viewing_file ? view_filename : "Files");
    int title_len = 0;
    while (title[title_len]) title_len++;
    int title_x = (screen_w - title_len * FONT_WIDTH) / 2;
    draw_string(fb, title_x, (TITLE_BAR_HEIGHT - FONT_HEIGHT) / 2, title, COLOR_TITLE_TEXT, screen_w, screen_h);

    /* Edit button when viewing file */
    if (viewing_file && !editing_file) {
        int edit_x = screen_w - 50;
        uint32_t edit_color = edit_btn_pressed ? COLOR_BTN_PRESS : COLOR_FILE;
        for (int py = btn_y; py < btn_y + btn_size; py++) {
            for (int px = edit_x; px < edit_x + btn_size; px++) {
                int dx = px - (edit_x + btn_size/2);
                int dy = py - btn_cy;
                if (dx * dx + dy * dy <= btn_r * btn_r) {
                    fb[py * screen_w + px] = edit_color;
                }
            }
        }
        /* Draw pencil icon (simple line) */
        int ec = edit_x + btn_size/2;
        for (int i = -5; i <= 5; i++) {
            fb[(btn_cy + i) * screen_w + ec - i] = 0x00FFFFFF;
        }
    }

    /* Save button when editing */
    if (editing_file) {
        int save_x = screen_w - 50;
        uint32_t save_color = save_btn_pressed ? COLOR_BTN_PRESS : 0x0000AA00;
        for (int py = btn_y; py < btn_y + btn_size; py++) {
            for (int px = save_x; px < save_x + btn_size; px++) {
                int dx = px - (save_x + btn_size/2);
                int dy = py - btn_cy;
                if (dx * dx + dy * dy <= btn_r * btn_r) {
                    fb[py * screen_w + px] = save_color;
                }
            }
        }
        /* Draw checkmark */
        int sc = save_x + btn_size/2;
        for (int i = 0; i < 4; i++) {
            fb[(btn_cy + i) * screen_w + sc - 4 + i] = 0x00FFFFFF;
        }
        for (int i = 0; i < 6; i++) {
            fb[(btn_cy + 3 - i) * screen_w + sc + i] = 0x00FFFFFF;
        }
    }

    /* Action buttons on right side (only in file list view) */
    if (!viewing_file && !editing_file) {
        /* Add button (+) */
        int add_x = screen_w - 90;
        uint32_t add_color = add_btn_pressed ? COLOR_BTN_PRESS : COLOR_BTN;
        for (int py = btn_y; py < btn_y + btn_size; py++) {
            for (int px = add_x; px < add_x + btn_size; px++) {
                int dx = px - (add_x + btn_size/2);
                int dy = py - btn_cy;
                if (dx * dx + dy * dy <= btn_r * btn_r) {
                    fb[py * screen_w + px] = add_color;
                }
            }
        }
        /* Draw + symbol */
        int plus_cx = add_x + btn_size/2;
        for (int i = -6; i <= 6; i++) {
            if (btn_cy >= 0 && (uint32_t)btn_cy < screen_h) {
                fb[btn_cy * screen_w + plus_cx + i] = 0x00FFFFFF;
            }
            if (plus_cx >= 0 && (uint32_t)plus_cx < screen_w) {
                fb[(btn_cy + i) * screen_w + plus_cx] = 0x00FFFFFF;
            }
        }

        /* Delete button (X) - only show if file selected */
        if (selected_for_action >= 0) {
            int del_x = screen_w - 50;
            uint32_t del_color = del_btn_pressed ? COLOR_BTN_PRESS : COLOR_ERROR;
            for (int py = btn_y; py < btn_y + btn_size; py++) {
                for (int px = del_x; px < del_x + btn_size; px++) {
                    int dx = px - (del_x + btn_size/2);
                    int dy = py - btn_cy;
                    if (dx * dx + dy * dy <= btn_r * btn_r) {
                        fb[py * screen_w + px] = del_color;
                    }
                }
            }
            /* Draw X symbol */
            int x_cx = del_x + btn_size/2;
            for (int i = -5; i <= 5; i++) {
                fb[(btn_cy + i) * screen_w + x_cx + i] = 0x00FFFFFF;
                fb[(btn_cy + i) * screen_w + x_cx - i] = 0x00FFFFFF;
            }
        }
    }

    /* Bottom border */
    fill_rect(fb, 0, TITLE_BAR_HEIGHT - 1, screen_w, 1, 0x00333344);
}

/* Format file size */
static void format_size(uint32_t size, char* buf) {
    if (size < 1024) {
        int i = 0;
        if (size == 0) {
            buf[i++] = '0';
        } else {
            char tmp[10];
            int j = 0;
            while (size > 0) {
                tmp[j++] = '0' + (size % 10);
                size /= 10;
            }
            while (j > 0) buf[i++] = tmp[--j];
        }
        buf[i++] = ' ';
        buf[i++] = 'B';
        buf[i] = 0;
    } else {
        size /= 1024;
        int i = 0;
        char tmp[10];
        int j = 0;
        while (size > 0) {
            tmp[j++] = '0' + (size % 10);
            size /= 10;
        }
        while (j > 0) buf[i++] = tmp[--j];
        buf[i++] = ' ';
        buf[i++] = 'K';
        buf[i++] = 'B';
        buf[i] = 0;
    }
}

/* Draw file list */
static void draw_file_list(uint32_t* fb) {
    int y = TITLE_BAR_HEIGHT + 5;
    int visible_count = (screen_h - TITLE_BAR_HEIGHT - 60) / FILE_ROW_HEIGHT;

    if (file_count == 0) {
        /* Empty state */
        const char* msg = "Tap to load files";
        int msg_len = 0;
        while (msg[msg_len]) msg_len++;
        int msg_x = (screen_w - msg_len * FONT_WIDTH) / 2;
        draw_string(fb, msg_x, screen_h / 2 - FONT_HEIGHT, msg, COLOR_FILE, screen_w, screen_h);
        return;
    }

    for (int i = 0; i < visible_count && (i + scroll_offset) < file_count; i++) {
        int idx = i + scroll_offset;
        fs_dirent_t* f = &files[idx];

        /* Row background - highlight selected file */
        uint32_t bg_color = COLOR_FILE_BG;
        if (idx == selected_for_action) {
            bg_color = COLOR_FILE_BG_SEL;
        } else if (idx == touch_file_idx) {
            bg_color = COLOR_FILE_BG_SEL;
        }
        fill_rect(fb, FILE_PADDING, y, screen_w - FILE_PADDING * 2, FILE_ROW_HEIGHT - 4, bg_color);

        /* File icon */
        int is_folder = (f->flags & 0x01);
        uint32_t icon_color = is_folder ? COLOR_FOLDER : COLOR_FILE;
        draw_file_icon(fb, FILE_PADDING + 8, y + 8, is_folder, icon_color);

        /* File name */
        draw_string(fb, FILE_PADDING + 45, y + 8, f->name, COLOR_FILE_TEXT, screen_w, screen_h);

        /* File size */
        char size_buf[16];
        format_size(f->size, size_buf);
        draw_string(fb, FILE_PADDING + 45, y + 28, size_buf, COLOR_FILE_SIZE, screen_w, screen_h);

        y += FILE_ROW_HEIGHT;
    }

    /* Scroll indicator */
    if (file_count > visible_count) {
        int total_h = screen_h - TITLE_BAR_HEIGHT - 60;
        int thumb_h = (visible_count * total_h) / file_count;
        if (thumb_h < 20) thumb_h = 20;
        int thumb_y = TITLE_BAR_HEIGHT + 5 + (scroll_offset * (total_h - thumb_h)) / (file_count - visible_count);
        fill_rect(fb, screen_w - 6, thumb_y, 4, thumb_h, 0x00444466);
    }
}

/* Draw file viewer */
static void draw_file_viewer(uint32_t* fb) {
    int y = TITLE_BAR_HEIGHT + 10;
    int x = FILE_PADDING;
    int max_chars = (screen_w - FILE_PADDING * 2) / FONT_WIDTH;
    int line_start = 0;
    int cursor_x = -1, cursor_y = -1;

    /* Limit view area when keyboard is visible */
    int max_y = editing_file && keyboard_is_visible() ?
                (int)screen_h - keyboard_get_height() - 40 : (int)screen_h - 40;

    for (int i = 0; i <= view_content_len && y < max_y; i++) {
        /* Track cursor position */
        if (editing_file && i == edit_cursor) {
            cursor_x = x + (i - line_start) * FONT_WIDTH;
            cursor_y = y;
        }

        if (i == view_content_len || view_content[i] == '\n' || (i - line_start) >= max_chars) {
            /* Draw this line */
            char line[80];
            int len = i - line_start;
            if (len > 79) len = 79;
            for (int j = 0; j < len; j++) {
                char c = view_content[line_start + j];
                line[j] = (c >= 32 && c < 127) ? c : '.';
            }
            line[len] = 0;
            draw_string(fb, x, y, line, COLOR_FILE_TEXT, screen_w, screen_h);

            /* If cursor is at end of this line */
            if (editing_file && i == edit_cursor && cursor_x < 0) {
                cursor_x = x + len * FONT_WIDTH;
                cursor_y = y;
            }

            y += FONT_HEIGHT + 2;
            line_start = i + 1;
        }
    }

    if (view_content_len == 0) {
        if (editing_file) {
            cursor_x = x;
            cursor_y = y;
        } else {
            draw_string(fb, x, y, "(empty file)", COLOR_EMPTY, screen_w, screen_h);
        }
    }

    /* Draw cursor when editing */
    if (editing_file && cursor_x >= 0 && cursor_y >= 0) {
        fill_rect(fb, cursor_x, cursor_y, 2, FONT_HEIGHT, COLOR_TITLE_TEXT);
    }
}

/* Draw status bar */
static void draw_status_bar(uint32_t* fb) {
    int y = screen_h - 30;
    fill_rect(fb, 0, y, screen_w, 30, COLOR_TITLE_BG);

    if (status_msg[0]) {
        uint32_t color = status_is_error ? COLOR_ERROR : COLOR_FILE_SIZE;
        draw_string(fb, FILE_PADDING, y + 8, status_msg, color, screen_w, screen_h);
    } else if (!viewing_file && file_count > 0) {
        /* Use static string - local arrays cause issues */
        draw_string(fb, FILE_PADDING, y + 8, "Tap file to view", COLOR_FILE_SIZE, screen_w, screen_h);
    }
}

void filemanager_init(void) {
    screen_w = goldfish_fb_get_width();
    screen_h = goldfish_fb_get_height();
    if (screen_w == 0) screen_w = 360;
    if (screen_h == 0) screen_h = 640;

    want_close = 0;
    needs_redraw = 1;
    back_btn_pressed = 0;
    add_btn_pressed = 0;
    del_btn_pressed = 0;
    edit_btn_pressed = 0;
    save_btn_pressed = 0;
    selected_file = -1;
    selected_for_action = -1;
    scroll_offset = 0;
    viewing_file = 0;
    editing_file = 0;
    edit_cursor = 0;
    touch_active = 0;
    touch_file_idx = -1;
    status_msg[0] = 0;
    status_is_error = 0;

    /* Load files immediately */
    refresh_file_list();
}

static void refresh_file_list(void) {
    file_count = 0;
    if (fs_mounted()) {
        int count = fs_readdir("/", files, 32);
        if (count > 0 && count <= 32) {
            file_count = count;
        }
    }
    selected_file = -1;
    scroll_offset = 0;
}

static void view_file(int idx) {
    if (idx < 0 || idx >= file_count) return;

    fs_dirent_t* f = &files[idx];
    if (f->flags & 0x01) return;  /* Skip folders */

    int fd = fs_open(f->name, FS_O_READ);
    if (fd < 0) {
        status_msg[0] = 'E'; status_msg[1] = 'r'; status_msg[2] = 'r';
        status_msg[3] = 'o'; status_msg[4] = 'r'; status_msg[5] = 0;
        status_is_error = 1;
        return;
    }

    view_content_len = fs_read(fd, view_content, 511);
    if (view_content_len < 0) view_content_len = 0;
    view_content[view_content_len] = 0;
    fs_close(fd);

    /* Copy filename */
    int i;
    for (i = 0; f->name[i] && i < 23; i++) {
        view_filename[i] = f->name[i];
    }
    view_filename[i] = 0;

    viewing_file = 1;
    status_msg[0] = 0;
}

static void do_delete_file(int idx) {
    if (idx < 0 || idx >= file_count) return;

    if (fs_remove(files[idx].name) == 0) {
        status_msg[0] = 'D'; status_msg[1] = 'e'; status_msg[2] = 'l';
        status_msg[3] = 'e'; status_msg[4] = 't'; status_msg[5] = 'e';
        status_msg[6] = 'd'; status_msg[7] = 0;
        status_is_error = 0;
        selected_for_action = -1;
        refresh_file_list();
    } else {
        status_msg[0] = 'E'; status_msg[1] = 'r'; status_msg[2] = 'r';
        status_msg[3] = 'o'; status_msg[4] = 'r'; status_msg[5] = 0;
        status_is_error = 1;
    }
}

static int new_file_counter = 1;

static void save_file(void) {
    if (!viewing_file || !editing_file) return;

    int fd = fs_open(view_filename, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
    if (fd >= 0) {
        fs_write(fd, view_content, view_content_len);
        fs_close(fd);
        status_msg[0] = 'S'; status_msg[1] = 'a'; status_msg[2] = 'v';
        status_msg[3] = 'e'; status_msg[4] = 'd'; status_msg[5] = 0;
        status_is_error = 0;
        editing_file = 0;
        keyboard_hide();
    } else {
        status_msg[0] = 'E'; status_msg[1] = 'r'; status_msg[2] = 'r';
        status_msg[3] = 'o'; status_msg[4] = 'r'; status_msg[5] = 0;
        status_is_error = 1;
    }
}

static void do_create_file(void) {
    if (!fs_mounted()) {
        status_msg[0] = 'N'; status_msg[1] = 'o'; status_msg[2] = ' ';
        status_msg[3] = 'F'; status_msg[4] = 'S'; status_msg[5] = 0;
        status_is_error = 1;
        return;
    }

    /* Generate filename: new1.txt, new2.txt, etc */
    char fname[16];
    fname[0] = 'n'; fname[1] = 'e'; fname[2] = 'w';
    int n = new_file_counter++;
    int pos = 3;
    if (n >= 10) { fname[pos++] = '0' + (n / 10); n = n % 10; }
    fname[pos++] = '0' + n;
    fname[pos++] = '.'; fname[pos++] = 't'; fname[pos++] = 'x'; fname[pos++] = 't';
    fname[pos] = 0;

    int fd = fs_open(fname, FS_O_WRITE | FS_O_CREATE);
    if (fd >= 0) {
        fs_write(fd, "New file\n", 9);
        fs_close(fd);
        status_msg[0] = 'C'; status_msg[1] = 'r'; status_msg[2] = 'e';
        status_msg[3] = 'a'; status_msg[4] = 't'; status_msg[5] = 'e';
        status_msg[6] = 'd'; status_msg[7] = 0;
        status_is_error = 0;
        refresh_file_list();
    } else {
        status_msg[0] = 'E'; status_msg[1] = 'r'; status_msg[2] = 'r';
        status_msg[3] = 'o'; status_msg[4] = 'r'; status_msg[5] = 0;
        status_is_error = 1;
    }
}

int filemanager_update(void) {
    input_event_t ev;

    /* Handle keyboard input when editing */
    if (editing_file) {
        char c = keyboard_get_char();
        if (c) {
            if (c == '\b') {
                /* Backspace */
                if (edit_cursor > 0 && view_content_len > 0) {
                    edit_cursor--;
                    /* Shift content left */
                    for (int i = edit_cursor; i < view_content_len - 1; i++) {
                        view_content[i] = view_content[i + 1];
                    }
                    view_content_len--;
                    view_content[view_content_len] = 0;
                }
            } else if (view_content_len < 510) {
                /* Insert character at cursor */
                for (int i = view_content_len; i > edit_cursor; i--) {
                    view_content[i] = view_content[i - 1];
                }
                view_content[edit_cursor] = c;
                edit_cursor++;
                view_content_len++;
                view_content[view_content_len] = 0;
            }
            needs_redraw = 1;
        }
    }

    while (event_pop(&ev) == 0) {
        /* Handle hardware keyboard */
        if (ev.type == EVENT_KEY) {
            /* Track shift state */
            if (ev.code == KEY_LEFTSHIFT || ev.code == KEY_RIGHTSHIFT) {
                shift_held = (ev.subtype == KEY_PRESS);
                continue;
            }

            if (ev.subtype != KEY_PRESS) continue;

            if (editing_file) {
                /* Editing mode - handle text input */
                if (ev.code == KEY_ENTER) {
                    /* Insert newline */
                    if (view_content_len < 510) {
                        for (int i = view_content_len; i > edit_cursor; i--) {
                            view_content[i] = view_content[i - 1];
                        }
                        view_content[edit_cursor] = '\n';
                        edit_cursor++;
                        view_content_len++;
                        view_content[view_content_len] = 0;
                        needs_redraw = 1;
                    }
                } else if (ev.code == KEY_BACKSPACE) {
                    if (edit_cursor > 0 && view_content_len > 0) {
                        edit_cursor--;
                        for (int i = edit_cursor; i < view_content_len - 1; i++) {
                            view_content[i] = view_content[i + 1];
                        }
                        view_content_len--;
                        view_content[view_content_len] = 0;
                        needs_redraw = 1;
                    }
                } else if (ev.code == KEY_ESC) {
                    /* Cancel edit */
                    editing_file = 0;
                    keyboard_hide();
                    status_msg[0] = 0;
                    needs_redraw = 1;
                } else {
                    char c = keycode_to_char(ev.code);
                    if (c && view_content_len < 510) {
                        for (int i = view_content_len; i > edit_cursor; i--) {
                            view_content[i] = view_content[i - 1];
                        }
                        view_content[edit_cursor] = c;
                        edit_cursor++;
                        view_content_len++;
                        view_content[view_content_len] = 0;
                        needs_redraw = 1;
                    }
                }
            } else if (ev.code == KEY_ESC) {
                /* ESC to go back */
                if (viewing_file) {
                    viewing_file = 0;
                    status_msg[0] = 0;
                } else {
                    want_close = 1;
                }
                needs_redraw = 1;
            }
            continue;
        }

        if (ev.type == EVENT_TOUCH) {
            int sx = (ev.x * screen_w) / 32768;
            int sy = (ev.y * screen_h) / 32768;

            /* Let keyboard handle touch first if visible (pass raw coords) */
            if (editing_file && keyboard_is_visible()) {
                if (keyboard_handle_touch(ev.subtype, ev.x, ev.y)) {
                    needs_redraw = 1;
                    continue;
                }
            }

            if (ev.subtype == TOUCH_DOWN) {
                if (sy < TITLE_BAR_HEIGHT) {
                    if (sx < 50) {
                        back_btn_pressed = 1;
                    } else if (!viewing_file && sx >= (int)screen_w - 90 && sx < (int)screen_w - 58) {
                        add_btn_pressed = 1;
                    } else if (!viewing_file && selected_for_action >= 0 && sx >= (int)screen_w - 50) {
                        del_btn_pressed = 1;
                    } else if (viewing_file && !editing_file && sx >= (int)screen_w - 50) {
                        /* Edit button */
                        edit_btn_pressed = 1;
                    } else if (editing_file && sx >= (int)screen_w - 50) {
                        /* Save button */
                        save_btn_pressed = 1;
                    }
                } else if (!viewing_file && sy >= TITLE_BAR_HEIGHT + 5 && file_count > 0) {
                    /* Track which file was touched */
                    int row = (sy - TITLE_BAR_HEIGHT - 5) / FILE_ROW_HEIGHT;
                    touch_file_idx = row + scroll_offset;
                    if (touch_file_idx >= file_count) touch_file_idx = -1;
                }
                needs_redraw = 1;
            } else if (ev.subtype == TOUCH_UP) {
                if (back_btn_pressed && sy < TITLE_BAR_HEIGHT && sx < 50) {
                    if (editing_file) {
                        /* Cancel edit mode */
                        editing_file = 0;
                        keyboard_hide();
                        status_msg[0] = 0;
                    } else if (viewing_file) {
                        viewing_file = 0;
                        status_msg[0] = 0;
                    } else {
                        want_close = 1;
                    }
                } else if (edit_btn_pressed && sy < TITLE_BAR_HEIGHT && sx >= (int)screen_w - 50) {
                    /* Enter edit mode */
                    editing_file = 1;
                    edit_cursor = view_content_len;  /* Cursor at end */
                    keyboard_init(screen_w, screen_h);
                    keyboard_show();
                    status_msg[0] = 0;
                } else if (save_btn_pressed && sy < TITLE_BAR_HEIGHT && sx >= (int)screen_w - 50) {
                    /* Save file */
                    save_file();
                } else if (add_btn_pressed && sy < TITLE_BAR_HEIGHT) {
                    do_create_file();
                } else if (del_btn_pressed && sy < TITLE_BAR_HEIGHT && selected_for_action >= 0) {
                    do_delete_file(selected_for_action);
                } else if (!viewing_file && touch_file_idx >= 0) {
                    if (selected_for_action == touch_file_idx) {
                        /* Double tap - view file */
                        view_file(touch_file_idx);
                        selected_for_action = -1;
                    } else {
                        /* Single tap - select for action */
                        selected_for_action = touch_file_idx;
                    }
                } else if (!viewing_file && sy > TITLE_BAR_HEIGHT) {
                    /* Tap empty area - deselect */
                    selected_for_action = -1;
                }
                back_btn_pressed = 0;
                add_btn_pressed = 0;
                del_btn_pressed = 0;
                edit_btn_pressed = 0;
                save_btn_pressed = 0;
                touch_file_idx = -1;
                needs_redraw = 1;
            }
        }
    }

    return needs_redraw;
}

void filemanager_draw(void) {
    uint32_t* fb = goldfish_fb_get_buffer();

    goldfish_fb_clear(COLOR_BG);
    draw_title_bar(fb);

    if (viewing_file) {
        draw_file_viewer(fb);
    } else {
        draw_file_list(fb);
    }

    /* Draw keyboard when editing (before status bar) */
    if (editing_file && keyboard_is_visible()) {
        keyboard_draw(fb, screen_w, screen_h);
    }

    draw_status_bar(fb);
    goldfish_fb_flush();
    needs_redraw = 0;
}

int filemanager_should_close(void) {
    return want_close;
}

void filemanager_clear_close(void) {
    want_close = 0;
}
