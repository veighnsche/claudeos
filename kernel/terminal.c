/*
 * ClaudeOS Terminal
 * A simple command-line interface with basic built-in commands
 */

#include "terminal.h"
#include "event.h"
#include "virtio_input.h"
#include "goldfish_fb.h"
#include "font.h"
#include "memory.h"
#include "types.h"
#include "http.h"
#include "websocket.h"
#include "tcp.h"
#include "virtio_blk.h"
#include "fs.h"
#include "keyboard.h"

/* Terminal configuration */
#define MAX_CMD_LEN     80
#define MAX_ARGS        8
#define MAX_HISTORY     128  /* Lines of output history */
#define CHARS_PER_LINE  64

/* Title bar configuration */
#define TITLE_BAR_HEIGHT    40
#define BACK_BTN_WIDTH      60
#define TITLE_BAR_BG        0x001A1A1A  /* Dark gray */
#define TITLE_BAR_TEXT      0x0000FF00  /* Matrix green */
#define BACK_BTN_COLOR      0x00303030  /* Slightly lighter gray */

/* Matrix theme colors */
#define DEFAULT_BG      0x00000000  /* Pure black background */
#define DEFAULT_TEXT    0x0000FF00  /* Matrix green text */
#define DEFAULT_PROMPT  0x0000CC00  /* Slightly darker green prompt */
#define COLOR_TOUCH     0x0033FF33  /* Bright green for touch */

/* Current colors (can be changed) */
static uint32_t color_bg = DEFAULT_BG;
static uint32_t color_text = DEFAULT_TEXT;
static uint32_t color_prompt = DEFAULT_PROMPT;

/* Uptime counter (incremented in main loop) */
static uint64_t uptime_ticks = 0;

/* Command buffer */
static char cmd_buffer[MAX_CMD_LEN];
static int cmd_pos = 0;

/* Output history (ring buffer) */
static char history[MAX_HISTORY][CHARS_PER_LINE + 1];
static int history_head = 0;    /* Next write position */
static int history_count = 0;   /* Number of lines in history */

/* Scroll state */
static int scroll_offset = 0;   /* 0 = bottom (newest), positive = scroll up */
static int max_visible_lines = 0;

/* Shift key state */
static int shift_held = 0;

/* Touch scrolling state */
static int32_t touch_start_y = 0;
static int touch_scrolling = 0;

/* Touch indicator */
static int touch_active = 0;
static int32_t touch_x = 0, touch_y = 0;

/* Display state */
static int needs_redraw = 1;

/* Close flag - when set, shell wants to return to home */
static int want_close = 0;

/* Back button press state */
static int back_btn_pressed = 0;

/* Forward declarations */
static void shell_print(const char* str);
static void shell_println(const char* str);
static void execute_command(void);

/* Helper: string compare */
static int strcmp(const char* a, const char* b) {
    while (*a && *b && *a == *b) {
        a++; b++;
    }
    return *a - *b;
}

/* Current line buffer for building output */
static char line_buffer[CHARS_PER_LINE + 1];
static int line_pos = 0;

/* Add line to history */
static void history_add(const char* line) {
    int i;
    for (i = 0; i < CHARS_PER_LINE && line[i]; i++) {
        history[history_head][i] = line[i];
    }
    history[history_head][i] = 0;
    history_head = (history_head + 1) % MAX_HISTORY;
    if (history_count < MAX_HISTORY) history_count++;
}

/* Flush line buffer to history */
static void shell_flush(void) {
    if (line_pos > 0) {
        line_buffer[line_pos] = 0;
        history_add(line_buffer);
        line_pos = 0;
    }
    /* Auto-scroll to bottom when new content added */
    scroll_offset = 0;
    needs_redraw = 1;
}

/* Scroll helpers */
static void scroll_up(int lines) {
    int max_scroll = history_count > max_visible_lines ? history_count - max_visible_lines : 0;
    scroll_offset += lines;
    if (scroll_offset > max_scroll) scroll_offset = max_scroll;
    needs_redraw = 1;
}

static void scroll_down(int lines) {
    scroll_offset -= lines;
    if (scroll_offset < 0) scroll_offset = 0;
    needs_redraw = 1;
}

/* Print string (accumulates in line buffer) */
static void shell_print(const char* str) {
    while (*str && line_pos < CHARS_PER_LINE) {
        line_buffer[line_pos++] = *str++;
    }
    line_buffer[line_pos] = 0;
}

/* Print string and flush (newline) */
static void shell_println(const char* str) {
    shell_print(str);
    shell_flush();
}

/* Print number in decimal */
static void print_dec(uint64_t val) {
    char buf[24];
    int i = 22;
    buf[23] = 0;
    if (val == 0) {
        buf[i--] = '0';
    } else {
        while (val > 0) {
            buf[i--] = '0' + (val % 10);
            val /= 10;
        }
    }
    shell_print(&buf[i + 1]);
}

/* Convert keycode to character */
static char keycode_to_char(uint16_t code) {
    /* Lowercase mapping */
    static const char lower[] = {
        0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0, 0,
        'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,
        'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\',
        'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '
    };
    /* Uppercase/shifted */
    static const char upper[] = {
        0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0, 0,
        'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,
        'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|',
        'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '
    };

    if (code >= sizeof(lower)) return 0;
    return shift_held ? upper[code] : lower[code];
}

/* ==================== Built-in Commands ==================== */

static void cmd_help(int argc, char** argv) {
    (void)argc; (void)argv;
    shell_println("ClaudeOS Terminal Commands:");
    shell_println(" help    - This help");
    shell_println(" close   - Return to home");
    shell_println(" clear   - Clear screen");
    shell_println(" echo    - Echo text");
    shell_println(" cpu     - CPU info");
    shell_println(" mem     - Memory map");
    shell_println(" heap    - Heap stats");
    shell_println(" uptime  - Time since boot");
    shell_println(" curl    - HTTP request");
    shell_println(" ws      - WebSocket client");
    shell_println(" color   - Change colors");
    shell_println(" calc    - Calculator");
    shell_println(" touch   - Touch info/debug");
    shell_println("Filesystem:");
    shell_println(" disk    - Disk info");
    shell_println(" ls      - List files");
    shell_println(" cat     - Read file");
    shell_println(" write   - Write file");
    shell_println(" rm      - Delete file");
    shell_println(" format  - Format disk");
}

static void cmd_clear(int argc, char** argv) {
    (void)argc; (void)argv;
    history_count = 0;
    history_head = 0;
    needs_redraw = 1;
}

static void cmd_heap(int argc, char** argv) {
    (void)argc; (void)argv;
    shell_println("Heap Statistics:");
    shell_print("  Free: ");
    print_dec(heap_free_bytes());
    shell_println(" bytes");
    shell_print("  Used: ");
    print_dec(heap_used_bytes());
    shell_println(" bytes");
}

static void cmd_reboot(int argc, char** argv) {
    (void)argc; (void)argv;
    /* Reset shell state */
    cmd_pos = 0;
    cmd_buffer[0] = 0;
    history_count = 0;
    history_head = 0;
    line_pos = 0;
    needs_redraw = 1;
    /* Reinitialize terminal welcome message */
    shell_println("ClaudeOS Terminal v1.0");
    shell_println("Type 'help' for commands");
    shell_println("");
}

static void cmd_echo(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        shell_print(argv[i]);
        if (i < argc - 1) shell_print(" ");
    }
    if (argc <= 1) shell_println("");
}

/* Print hex number */
static void print_hex(uint64_t val) {
    char buf[19];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        int nibble = (val >> (60 - i * 4)) & 0xF;
        buf[2 + i] = "0123456789ABCDEF"[nibble];
    }
    buf[18] = 0;
    /* Skip leading zeros but keep at least one digit */
    char* p = buf + 2;
    while (*p == '0' && *(p+1) != 0) p++;
    shell_print("0x");
    shell_print(p);
}

static void cmd_cpu(int argc, char** argv) {
    (void)argc; (void)argv;
    uint64_t midr, mpidr, ctr;
    __asm__ volatile("mrs %0, midr_el1" : "=r"(midr));
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));

    shell_println("CPU Information:");
    shell_print("  MIDR_EL1:  ");
    print_hex(midr);
    shell_print("  MPIDR_EL1: ");
    print_hex(mpidr);
    shell_print("  Implementer: ");
    int impl = (midr >> 24) & 0xFF;
    if (impl == 0x41) shell_println("ARM");
    else if (impl == 0x51) shell_println("Qualcomm");
    else shell_println("Unknown");
}

static void cmd_mem(int argc, char** argv) {
    (void)argc; (void)argv;
    shell_println("Memory Map:");
    shell_println("  Kernel:  0x40200000");
    shell_println("  Heap:    0x40210000 - 0x41F00000");
    shell_println("  FB:      0x42000000");
    shell_println("  VirtIO:  0x46000000");
    shell_print("  Free:    ");
    print_dec(heap_free_bytes());
    shell_println(" bytes");
}

static void cmd_logo(int argc, char** argv) {
    (void)argc; (void)argv;
    shell_println("   ____ _                 _       ___  ____  ");
    shell_println("  / ___| | __ _ _   _  __| | ___ / _ \\/ ___| ");
    shell_println(" | |   | |/ _` | | | |/ _` |/ _ \\ | | \\___ \\ ");
    shell_println(" | |___| | (_| | |_| | (_| |  __/ |_| |___) |");
    shell_println("  \\____|_|\\__,_|\\__,_|\\__,_|\\___|\\___/|____/ ");
    shell_println("                                             ");
}

static void cmd_hex(int argc, char** argv) {
    if (argc < 2) {
        shell_println("Usage: hex <number>");
        return;
    }
    /* Parse decimal number */
    uint64_t val = 0;
    char* p = argv[1];
    while (*p >= '0' && *p <= '9') {
        val = val * 10 + (*p - '0');
        p++;
    }
    shell_print("  Dec: ");
    print_dec(val);
    shell_print("  Hex: ");
    print_hex(val);
    shell_flush();
}

static void cmd_peek(int argc, char** argv) {
    if (argc < 2) {
        shell_println("Usage: peek <hex_addr>");
        return;
    }
    /* Parse hex address (skip 0x if present) */
    char* p = argv[1];
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    uint64_t addr = 0;
    while (*p) {
        addr <<= 4;
        if (*p >= '0' && *p <= '9') addr |= *p - '0';
        else if (*p >= 'a' && *p <= 'f') addr |= *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') addr |= *p - 'A' + 10;
        else break;
        p++;
    }
    shell_print("  [");
    print_hex(addr);
    shell_print("] = ");
    volatile uint32_t* ptr = (volatile uint32_t*)addr;
    print_hex(*ptr);
    shell_flush();
}

static void cmd_poke(int argc, char** argv) {
    if (argc < 3) {
        shell_println("Usage: poke <hex_addr> <hex_val>");
        return;
    }
    /* Parse hex address */
    char* p = argv[1];
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    uint64_t addr = 0;
    while (*p) {
        addr <<= 4;
        if (*p >= '0' && *p <= '9') addr |= *p - '0';
        else if (*p >= 'a' && *p <= 'f') addr |= *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') addr |= *p - 'A' + 10;
        else break;
        p++;
    }
    /* Parse hex value */
    p = argv[2];
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    uint32_t val = 0;
    while (*p) {
        val <<= 4;
        if (*p >= '0' && *p <= '9') val |= *p - '0';
        else if (*p >= 'a' && *p <= 'f') val |= *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') val |= *p - 'A' + 10;
        else break;
        p++;
    }
    volatile uint32_t* ptr = (volatile uint32_t*)addr;
    *ptr = val;
    shell_print("  Wrote ");
    print_hex(val);
    shell_print(" to ");
    print_hex(addr);
    shell_flush();
}

/* ARM64 generic timer - read counter and frequency */
static inline uint64_t read_cntpct(void) {
    uint64_t val;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(val));
    return val;
}

static inline uint64_t read_cntfrq(void) {
    uint64_t val;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(val));
    return val;
}

static uint64_t boot_counter = 0;

static void cmd_uptime(int argc, char** argv) {
    (void)argc; (void)argv;

    /* Use ARM generic timer for accurate uptime */
    uint64_t now = read_cntpct();
    uint64_t freq = read_cntfrq();

    /* Calculate seconds since boot */
    uint64_t elapsed = now - boot_counter;
    uint64_t secs = (freq > 0) ? (elapsed / freq) : 0;
    uint64_t mins = secs / 60;
    uint64_t hours = mins / 60;

    shell_print("  Uptime: ");
    if (hours > 0) {
        print_dec(hours);
        shell_print("h ");
    }
    print_dec(mins % 60);
    shell_print("m ");
    print_dec(secs % 60);
    shell_println("s");
}

/* Parse hex color value */
static uint32_t parse_color(const char* s) {
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    uint32_t val = 0;
    while (*s) {
        val <<= 4;
        if (*s >= '0' && *s <= '9') val |= *s - '0';
        else if (*s >= 'a' && *s <= 'f') val |= *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') val |= *s - 'A' + 10;
        else break;
        s++;
    }
    return val;
}

static void cmd_color(int argc, char** argv) {
    if (argc < 2) {
        shell_println("Usage: color <preset>|<bg> [text]");
        shell_println("Presets: dark, light, matrix,");
        shell_println("         ocean, fire, cyber");
        return;
    }
    /* Check for presets */
    if (strcmp(argv[1], "dark") == 0) {
        color_bg = 0x00000044; color_text = 0x00FFFFFF; color_prompt = 0x0000FF00;
    } else if (strcmp(argv[1], "light") == 0) {
        color_bg = 0x00E0E0E0; color_text = 0x00000000; color_prompt = 0x00006600;
    } else if (strcmp(argv[1], "matrix") == 0) {
        color_bg = 0x00000000; color_text = 0x0000FF00; color_prompt = 0x0000AA00;
    } else if (strcmp(argv[1], "ocean") == 0) {
        color_bg = 0x00001133; color_text = 0x0066CCFF; color_prompt = 0x0000FFFF;
    } else if (strcmp(argv[1], "fire") == 0) {
        color_bg = 0x00220000; color_text = 0x00FF6600; color_prompt = 0x00FFFF00;
    } else if (strcmp(argv[1], "cyber") == 0) {
        color_bg = 0x00110022; color_text = 0x00FF00FF; color_prompt = 0x0000FFFF;
    } else {
        /* Custom hex colors */
        color_bg = parse_color(argv[1]);
        if (argc >= 3) color_text = parse_color(argv[2]);
        if (argc >= 4) color_prompt = parse_color(argv[3]);
    }
    needs_redraw = 1;
    shell_println("Colors updated!");
}

static void cmd_draw(int argc, char** argv) {
    (void)argc; (void)argv;
    uint32_t* fb = goldfish_fb_get_buffer();
    uint32_t w = goldfish_fb_get_width();
    uint32_t h = goldfish_fb_get_height();

    /* Draw colorful pattern */
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint8_t r = (x * 255) / w;
            uint8_t g = (y * 255) / h;
            uint8_t b = 128;
            fb[y * w + x] = (r << 16) | (g << 8) | b;
        }
    }
    /* Draw some shapes */
    /* White border */
    for (uint32_t x = 0; x < w; x++) {
        fb[x] = 0x00FFFFFF;
        fb[(h-1) * w + x] = 0x00FFFFFF;
    }
    for (uint32_t y = 0; y < h; y++) {
        fb[y * w] = 0x00FFFFFF;
        fb[y * w + w - 1] = 0x00FFFFFF;
    }
    /* Center circle */
    int cx = w / 2, cy = h / 2, r = 50;
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (dx*dx + dy*dy <= r*r) {
                int px = cx + dx, py = cy + dy;
                if (px >= 0 && px < (int)w && py >= 0 && py < (int)h) {
                    fb[py * w + px] = 0x00FFFF00;
                }
            }
        }
    }
    goldfish_fb_flush();
    shell_println("Graphics demo! Press key to return.");
}

static void cmd_touch(int argc, char** argv) {
    if (argc >= 2 && strcmp(argv[1], "debug") == 0) {
        virtio_input_set_debug(1);
        shell_println("Touch debug ON (see UART)");
        return;
    }
    if (argc >= 2 && strcmp(argv[1], "off") == 0) {
        virtio_input_set_debug(0);
        shell_println("Touch debug OFF");
        return;
    }
    /* Show current touch status */
    int32_t tx, ty;
    int is_down;
    virtio_input_get_touch(&tx, &ty, &is_down);
    shell_print("Touch: ");
    shell_print(is_down ? "DOWN" : "UP");
    shell_print(" x=");
    print_dec(tx);
    shell_print(" y=");
    print_dec(ty);
    shell_flush();
    shell_println("Use 'touch debug' to see events");
}

/* HTTP curl command - async/non-blocking */
static http_request_t http_req;
static int http_active = 0;

static void cmd_curl(int argc, char** argv) {
    if (argc < 2) {
        shell_println("Usage: curl <url>");
        shell_println("  curl http://example.com/");
        shell_println("  curl http://httpbin.org/ip");
        return;
    }

    /* Start new request */
    if (http_active) {
        shell_println("Request already in progress");
        return;
    }

    shell_print("Fetching ");
    shell_println(argv[1]);

    if (http_request_start(&http_req, HTTP_GET, argv[1], NULL, 0) == 0) {
        http_active = 1;
    } else {
        shell_println("Failed to start request");
    }
}

/* WebSocket command */
static websocket_t ws_conn;
static int ws_active = 0;

static void cmd_ws(int argc, char** argv) {
    if (argc < 2) {
        shell_println("Usage: ws <cmd> [args]");
        shell_println("  ws connect <url>");
        shell_println("  ws send <message>");
        shell_println("  ws ping");
        shell_println("  ws close");
        shell_println("  ws status");
        return;
    }

    if (strcmp(argv[1], "connect") == 0) {
        if (argc < 3) {
            shell_println("Usage: ws connect <url>");
            return;
        }
        if (ws_active) {
            shell_println("Already connected. Use 'ws close' first.");
            return;
        }
        shell_print("Connecting to ");
        shell_println(argv[2]);

        if (ws_connect(&ws_conn, argv[2]) == 0) {
            ws_active = 1;
            shell_println("Connection started...");
            shell_println("Use 'ws status' to check");
        } else {
            shell_println("Connect failed!");
        }
    }
    else if (strcmp(argv[1], "send") == 0) {
        if (!ws_active || ws_get_state(&ws_conn) != WS_STATE_OPEN) {
            shell_println("Not connected!");
            return;
        }
        if (argc < 3) {
            shell_println("Usage: ws send <message>");
            return;
        }
        /* Concatenate remaining args as message */
        char msg[128];
        int pos = 0;
        for (int i = 2; i < argc && pos < 120; i++) {
            char* p = argv[i];
            while (*p && pos < 120) msg[pos++] = *p++;
            if (i < argc - 1 && pos < 120) msg[pos++] = ' ';
        }
        msg[pos] = 0;

        if (ws_send_text(&ws_conn, msg) >= 0) {
            shell_print("Sent: ");
            shell_println(msg);
        } else {
            shell_println("Send failed!");
        }
    }
    else if (strcmp(argv[1], "ping") == 0) {
        if (!ws_active || ws_get_state(&ws_conn) != WS_STATE_OPEN) {
            shell_println("Not connected!");
            return;
        }
        ws_send_ping(&ws_conn);
        shell_println("Ping sent");
    }
    else if (strcmp(argv[1], "close") == 0) {
        if (ws_active) {
            ws_close(&ws_conn);
            ws_active = 0;
            shell_println("Connection closed");
        } else {
            shell_println("Not connected");
        }
    }
    else if (strcmp(argv[1], "status") == 0) {
        if (!ws_active) {
            shell_println("State: Not connected");
            return;
        }
        int state = ws_get_state(&ws_conn);
        shell_print("State: ");
        switch (state) {
            case WS_STATE_CLOSED: shell_println("Closed"); break;
            case WS_STATE_CONNECTING: shell_println("Connecting"); break;
            case WS_STATE_OPEN: shell_println("Open"); break;
            case WS_STATE_CLOSING: shell_println("Closing"); break;
            default: shell_println("Unknown"); break;
        }

        /* Check for messages */
        if (ws_message_ready(&ws_conn)) {
            char buf[256];
            int len = ws_get_message(&ws_conn, buf, sizeof(buf));
            shell_print("Received (");
            print_dec(len);
            shell_println(" bytes):");
            shell_println(buf);
        }
    }
    else if (strcmp(argv[1], "poll") == 0) {
        if (!ws_active) {
            shell_println("Not connected");
            return;
        }
        /* Poll WebSocket */
        ws_poll(&ws_conn);
        shell_println("Polled");

        if (ws_message_ready(&ws_conn)) {
            char buf[256];
            int len = ws_get_message(&ws_conn, buf, sizeof(buf));
            shell_print("Message (");
            print_dec(len);
            shell_println(" bytes):");
            shell_println(buf);
        }
    }
    else {
        shell_print("Unknown ws command: ");
        shell_println(argv[1]);
    }
}

static void cmd_calc(int argc, char** argv) {
    if (argc < 4) {
        shell_println("Usage: calc <n1> <op> <n2>");
        shell_println("  ops: + - * / %");
        return;
    }
    /* Parse first number */
    int64_t a = 0, b = 0, neg = 1;
    char* p = argv[1];
    if (*p == '-') { neg = -1; p++; }
    while (*p >= '0' && *p <= '9') a = a * 10 + (*p++ - '0');
    a *= neg;

    /* Parse second number */
    neg = 1;
    p = argv[3];
    if (*p == '-') { neg = -1; p++; }
    while (*p >= '0' && *p <= '9') b = b * 10 + (*p++ - '0');
    b *= neg;

    /* Perform operation */
    int64_t result = 0;
    char op = argv[2][0];
    switch (op) {
        case '+': result = a + b; break;
        case '-': result = a - b; break;
        case '*': result = a * b; break;
        case '/':
            if (b == 0) { shell_println("  Error: div by 0"); return; }
            result = a / b;
            break;
        case '%':
            if (b == 0) { shell_println("  Error: div by 0"); return; }
            result = a % b;
            break;
        default:
            shell_println("  Unknown operator");
            return;
    }
    shell_print("  = ");
    if (result < 0) {
        shell_print("-");
        result = -result;
    }
    print_dec((uint64_t)result);
    shell_flush();
}

static void cmd_close(int argc, char** argv) {
    (void)argc; (void)argv;
    shell_println("Returning to home...");
    want_close = 1;
}

/* ==================== Filesystem Commands ==================== */

static void cmd_disk(int argc, char** argv) {
    (void)argc; (void)argv;
    if (!blk_available()) {
        shell_println("No disk detected");
        return;
    }

    disk_info_t* info = blk_get_info();
    shell_println("Disk Information:");
    shell_print("  Capacity: ");
    print_dec(info->capacity);
    shell_println(" sectors");

    /* Calculate size in MB */
    uint64_t bytes = info->capacity * 512;
    uint64_t mb = bytes / (1024 * 1024);
    shell_print("  Size: ");
    print_dec(mb);
    shell_println(" MB");

    /* Filesystem status */
    if (fs_mounted()) {
        fs_stats_t stats;
        if (fs_stats(&stats) == 0) {
            shell_println("  Filesystem: TinyFS");
            shell_print("  Clusters: ");
            print_dec(stats.free_clusters);
            shell_print("/");
            print_dec(stats.total_clusters);
            shell_println(" free");
            shell_print("  Files: ");
            print_dec(stats.files_count);
            shell_println("");
        }
    } else {
        shell_println("  Filesystem: Not formatted");
        shell_println("  Use 'format' to create TinyFS");
    }
}

static void cmd_ls(int argc, char** argv) {
    (void)argc; (void)argv;
    if (!fs_mounted()) {
        shell_println("Filesystem not mounted");
        shell_println("Use 'format' to format disk");
        return;
    }

    fs_dirent_t entries[32];
    int count = fs_readdir("/", entries, 32);

    if (count < 0) {
        shell_println("Error reading directory");
        return;
    }

    if (count == 0) {
        shell_println("(empty)");
        return;
    }

    shell_println("Files:");
    for (int i = 0; i < count; i++) {
        shell_print("  ");
        shell_print(entries[i].name);
        shell_print("  ");
        print_dec(entries[i].size);
        shell_println(" bytes");
    }
    shell_print("Total: ");
    print_dec(count);
    shell_println(" file(s)");
}

static void cmd_cat(int argc, char** argv) {
    if (argc < 2) {
        shell_println("Usage: cat <filename>");
        return;
    }
    if (!fs_mounted()) {
        shell_println("Filesystem not mounted");
        return;
    }

    int fd = fs_open(argv[1], FS_O_READ);
    if (fd < 0) {
        shell_print("Cannot open: ");
        shell_println(argv[1]);
        return;
    }

    int size = fs_size(fd);
    shell_print("[");
    print_dec(size);
    shell_println(" bytes]");

    /* Read and display file contents */
    char buf[128];
    int total = 0;
    int len;
    while ((len = fs_read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[len] = 0;
        /* Print character by character, handling newlines */
        for (int i = 0; i < len; i++) {
            if (buf[i] == '\n') {
                shell_flush();
            } else if (buf[i] != '\r' && buf[i] >= 32) {
                if (line_pos < CHARS_PER_LINE - 1) {
                    line_buffer[line_pos++] = buf[i];
                }
            }
        }
        total += len;
        if (total > 1024) {
            shell_flush();
            shell_println("...(truncated)");
            break;
        }
    }
    if (line_pos > 0) shell_flush();

    fs_close(fd);
}

static void cmd_write(int argc, char** argv) {
    if (argc < 3) {
        shell_println("Usage: write <file> <text>");
        return;
    }
    if (!fs_mounted()) {
        shell_println("Filesystem not mounted");
        return;
    }

    /* Open/create file */
    int fd = fs_open(argv[1], FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
    if (fd < 0) {
        shell_print("Cannot create: ");
        shell_println(argv[1]);
        return;
    }

    /* Concatenate remaining args with spaces */
    char content[256];
    int pos = 0;
    for (int i = 2; i < argc && pos < 250; i++) {
        char* p = argv[i];
        while (*p && pos < 250) content[pos++] = *p++;
        if (i < argc - 1 && pos < 250) content[pos++] = ' ';
    }
    content[pos++] = '\n';
    content[pos] = 0;

    int written = fs_write(fd, content, pos);
    fs_close(fd);

    if (written > 0) {
        shell_print("Wrote ");
        print_dec(written);
        shell_print(" bytes to ");
        shell_println(argv[1]);
    } else {
        shell_println("Write failed!");
    }
}

static void cmd_rm(int argc, char** argv) {
    if (argc < 2) {
        shell_println("Usage: rm <filename>");
        return;
    }
    if (!fs_mounted()) {
        shell_println("Filesystem not mounted");
        return;
    }

    if (fs_remove(argv[1]) == 0) {
        shell_print("Deleted: ");
        shell_println(argv[1]);
    } else {
        shell_print("Cannot delete: ");
        shell_println(argv[1]);
    }
}

static void cmd_format(int argc, char** argv) {
    (void)argc; (void)argv;
    if (!blk_available()) {
        shell_println("No disk available");
        return;
    }

    shell_println("Formatting disk...");
    if (fs_format() == 0) {
        shell_println("Disk formatted successfully!");
        fs_stats_t stats;
        if (fs_stats(&stats) == 0) {
            shell_print("  ");
            print_dec(stats.total_clusters);
            shell_print(" clusters (");
            print_dec(stats.total_clusters * 2);
            shell_println(" KB)");
        }
    } else {
        shell_println("Format failed!");
    }
}

/* Command table */
struct command {
    const char* name;
    void (*func)(int argc, char** argv);
};

static struct command commands[] = {
    { "help",   cmd_help },
    { "close",  cmd_close },
    { "exit",   cmd_close },
    { "clear",  cmd_clear },
    { "heap",   cmd_heap },
    { "reboot", cmd_reboot },
    { "echo",   cmd_echo },
    { "cpu",    cmd_cpu },
    { "mem",    cmd_mem },
    { "logo",   cmd_logo },
    { "hex",    cmd_hex },
    { "peek",   cmd_peek },
    { "poke",   cmd_poke },
    { "uptime", cmd_uptime },
    { "color",  cmd_color },
    { "draw",   cmd_draw },
    { "calc",   cmd_calc },
    { "touch",  cmd_touch },
    { "curl",   cmd_curl },
    { "ws",     cmd_ws },
    /* Filesystem commands */
    { "disk",   cmd_disk },
    { "ls",     cmd_ls },
    { "cat",    cmd_cat },
    { "write",  cmd_write },
    { "rm",     cmd_rm },
    { "format", cmd_format },
    { NULL, NULL }
};

/* Parse and execute command */
static void execute_command(void) {
    if (cmd_pos == 0) return;

    /* Add command to history */
    char prompt_line[CHARS_PER_LINE + 1];
    prompt_line[0] = '>';
    prompt_line[1] = ' ';
    for (int i = 0; i < cmd_pos && i < CHARS_PER_LINE - 2; i++) {
        prompt_line[i + 2] = cmd_buffer[i];
    }
    prompt_line[cmd_pos + 2 < CHARS_PER_LINE ? cmd_pos + 2 : CHARS_PER_LINE] = 0;
    history_add(prompt_line);

    /* Tokenize */
    char* argv[MAX_ARGS];
    int argc = 0;
    char* p = cmd_buffer;

    while (*p && argc < MAX_ARGS) {
        /* Skip whitespace */
        while (*p == ' ') p++;
        if (*p == 0) break;

        argv[argc++] = p;

        /* Find end of token */
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
    }

    if (argc == 0) return;

    /* Look up command */
    for (int i = 0; commands[i].name; i++) {
        if (strcmp(argv[0], commands[i].name) == 0) {
            commands[i].func(argc, argv);
            return;
        }
    }

    /* Unknown command */
    shell_print("Unknown command: ");
    shell_println(argv[0]);
}

/* ==================== Shell Interface ==================== */

void terminal_init(void) {
    cmd_pos = 0;
    cmd_buffer[0] = 0;
    history_count = 0;
    history_head = 0;
    line_pos = 0;
    shift_held = 0;
    touch_active = 0;
    scroll_offset = 0;
    touch_scrolling = 0;
    needs_redraw = 1;
    uptime_ticks = 0;
    want_close = 0;
    back_btn_pressed = 0;

    /* Initialize boot timer */
    boot_counter = read_cntpct();

    /* Initialize soft keyboard */
    uint32_t w = goldfish_fb_get_width();
    uint32_t h = goldfish_fb_get_height();
    keyboard_init(w, h);

    shell_println("ClaudeOS Terminal v1.0");
    shell_println("Tap screen to show keyboard");
    shell_println("Type 'help' for commands");
    shell_println("");
}

/* Poll background network tasks - non-blocking */
static void poll_network_tasks(void) {
    /* Poll active HTTP request */
    if (http_active) {
        int state = http_request_poll(&http_req);
        if (state == HTTP_STATE_DONE) {
            /* Request complete - show response */
            shell_print("HTTP ");
            print_dec(http_req.response.status_code);
            shell_print(" (");
            print_dec(http_req.response.body_len);
            shell_println(" bytes)");

            /* Print body */
            if (http_req.response.body_len > 0) {
                char* p = http_req.response.body;
                while (*p && (p - http_req.response.body) < 500) {
                    if (*p == '\n') {
                        shell_flush();
                    } else if (*p != '\r') {
                        if (line_pos < CHARS_PER_LINE - 1) {
                            line_buffer[line_pos++] = *p;
                        }
                    }
                    p++;
                }
                if (line_pos > 0) shell_flush();
                if (http_req.response.body_len > 500) shell_println("...");
            }
            http_request_close(&http_req);
            http_active = 0;
            needs_redraw = 1;
        } else if (state == HTTP_STATE_ERROR) {
            shell_println("HTTP request failed");
            http_request_close(&http_req);
            http_active = 0;
            needs_redraw = 1;
        }
    }

    /* Poll active WebSocket */
    if (ws_active) {
        ws_poll(&ws_conn);
    }
}

void terminal_tick(void) {
    uptime_ticks++;
    /* Poll network tasks every tick (non-blocking) */
    poll_network_tasks();
}

int terminal_update(void) {
    input_event_t ev;

    /* Check for soft keyboard input */
    char kb_char = keyboard_get_char();
    if (kb_char) {
        if (kb_char == '\n') {
            cmd_buffer[cmd_pos] = 0;
            execute_command();
            cmd_pos = 0;
            cmd_buffer[0] = 0;
            scroll_offset = 0;
        } else if (kb_char == '\b') {
            if (cmd_pos > 0) {
                cmd_pos--;
                cmd_buffer[cmd_pos] = 0;
            }
        } else if (cmd_pos < MAX_CMD_LEN - 1) {
            cmd_buffer[cmd_pos++] = kb_char;
            cmd_buffer[cmd_pos] = 0;
        }
        needs_redraw = 1;
    }

    while (event_pop(&ev) == 0) {

        if (ev.type == EVENT_KEY) {
            /* Handle shift keys */
            if (ev.code == KEY_LEFTSHIFT || ev.code == KEY_RIGHTSHIFT) {
                shift_held = (ev.subtype == KEY_PRESS);
                continue;
            }

            if (ev.subtype != KEY_PRESS) continue;  /* Only process key presses */

            /* Arrow keys for scrolling */
            if (ev.code == KEY_UP) {
                scroll_up(1);
                continue;
            } else if (ev.code == KEY_DOWN) {
                scroll_down(1);
                continue;
            }

            if (ev.code == KEY_ENTER) {
                cmd_buffer[cmd_pos] = 0;
                execute_command();
                cmd_pos = 0;
                cmd_buffer[0] = 0;
                scroll_offset = 0;  /* Scroll to bottom on command */
                needs_redraw = 1;
            } else if (ev.code == KEY_BACKSPACE) {
                if (cmd_pos > 0) {
                    cmd_pos--;
                    cmd_buffer[cmd_pos] = 0;
                    needs_redraw = 1;
                }
            } else {
                char c = keycode_to_char(ev.code);
                if (c && cmd_pos < MAX_CMD_LEN - 1) {
                    cmd_buffer[cmd_pos++] = c;
                    cmd_buffer[cmd_pos] = 0;
                    needs_redraw = 1;
                }
            }
        } else if (ev.type == EVENT_TOUCH) {
            /* Let keyboard handle touch first */
            if (keyboard_handle_touch(ev.subtype, ev.x, ev.y)) {
                needs_redraw = 1;
                continue;
            }

            /* Handle touch events for scrolling and keyboard toggle */
            if (ev.subtype == TOUCH_DOWN) {
                /* Scale touch coords */
                uint32_t width = goldfish_fb_get_width();
                uint32_t height = goldfish_fb_get_height();
                int sx = (ev.x * width) / 32768;
                int sy = (ev.y * height) / 32768;

                touch_active = 1;
                touch_x = ev.x;
                touch_y = ev.y;
                touch_start_y = ev.y;
                touch_scrolling = 0;

                /* Check if back button is pressed */
                if (sy < TITLE_BAR_HEIGHT && sx < 50) {
                    back_btn_pressed = 1;
                } else {
                    back_btn_pressed = 0;
                }
                needs_redraw = 1;
            } else if (ev.subtype == TOUCH_MOVE) {
                touch_active = 1;
                touch_x = ev.x;
                touch_y = ev.y;

                /* Detect vertical swipe for scrolling */
                int32_t dy = touch_start_y - ev.y;
                int scroll_threshold = 1000;

                if (dy > scroll_threshold) {
                    scroll_down(1);
                    touch_start_y = ev.y;
                    touch_scrolling = 1;
                    back_btn_pressed = 0;  /* Cancel button press on scroll */
                } else if (dy < -scroll_threshold) {
                    scroll_up(1);
                    touch_start_y = ev.y;
                    touch_scrolling = 1;
                    back_btn_pressed = 0;  /* Cancel button press on scroll */
                }
                needs_redraw = 1;
            } else if (ev.subtype == TOUCH_UP) {
                /* Scale touch coords */
                uint32_t width = goldfish_fb_get_width();
                uint32_t height = goldfish_fb_get_height();
                int sx = (ev.x * width) / 32768;
                int sy = (ev.y * height) / 32768;

                if (!touch_scrolling) {
                    /* Check for back button release */
                    if (back_btn_pressed && sy < TITLE_BAR_HEIGHT && sx < 50) {
                        want_close = 1;
                    } else if (sy >= TITLE_BAR_HEIGHT) {
                        /* Toggle keyboard on tap (not in title bar) */
                        keyboard_toggle();
                    }
                }
                back_btn_pressed = 0;
                touch_active = 0;
                touch_scrolling = 0;
                needs_redraw = 1;
            } else if (ev.subtype == TOUCH_SCROLL_UP) {
                scroll_up(ev.y > 0 ? ev.y : 1);
            } else if (ev.subtype == TOUCH_SCROLL_DOWN) {
                scroll_down(ev.y > 0 ? ev.y : 1);
            }
        }
    }

    return needs_redraw;
}

/* Draw a left arrow icon */
static void draw_back_arrow(uint32_t* fb, int cx, int cy, uint32_t color, uint32_t width) {
    /* Draw a "<" arrow pointing left */
    int arrow_size = 8;
    for (int i = 0; i < arrow_size; i++) {
        /* Top half of arrow */
        int px = cx + i;
        int py = cy - i;
        if (px >= 0 && py >= 0 && (uint32_t)px < width)
            fb[py * width + px] = color;
        /* Bottom half of arrow */
        py = cy + i;
        if (px >= 0 && py >= 0 && (uint32_t)px < width)
            fb[py * width + px] = color;
        /* Make it thicker */
        if (i > 0) {
            fb[(cy - i) * width + px + 1] = color;
            fb[(cy + i) * width + px + 1] = color;
        }
    }
}

/* Draw title bar helper */
static void draw_title_bar(uint32_t* fb, uint32_t width, int btn_pressed) {
    /* Draw title bar background */
    for (uint32_t y = 0; y < TITLE_BAR_HEIGHT; y++) {
        for (uint32_t x = 0; x < width; x++) {
            fb[y * width + x] = TITLE_BAR_BG;
        }
    }

    /* Draw back button (circular) */
    int btn_size = 32;
    int btn_x = 10;
    int btn_y = (TITLE_BAR_HEIGHT - btn_size) / 2;
    int btn_cx = btn_x + btn_size / 2;
    int btn_cy = btn_y + btn_size / 2;
    int btn_r = btn_size / 2;

    /* Button color - lighter when pressed */
    uint32_t btn_color = btn_pressed ? 0x00505050 : BACK_BTN_COLOR;
    uint32_t arrow_color = btn_pressed ? 0x0000FF00 : 0x00FFFFFF;

    /* Draw circular button */
    for (int py = btn_y; py < btn_y + btn_size; py++) {
        for (int px = btn_x; px < btn_x + btn_size; px++) {
            int dx = px - btn_cx;
            int dy = py - btn_cy;
            if (dx * dx + dy * dy <= btn_r * btn_r) {
                fb[py * width + px] = btn_color;
            }
        }
    }

    /* Draw arrow in center of button */
    draw_back_arrow(fb, btn_cx - 3, btn_cy, arrow_color, width);

    /* Draw title centered */
    const char* title = "Terminal";
    int title_len = 8;
    int title_x = (width - title_len * FONT_WIDTH) / 2;
    draw_string(fb, title_x, (TITLE_BAR_HEIGHT - FONT_HEIGHT) / 2, title, TITLE_BAR_TEXT, width, TITLE_BAR_HEIGHT + 50);

    /* Draw bottom border */
    for (uint32_t x = 0; x < width; x++) {
        fb[(TITLE_BAR_HEIGHT - 1) * width + x] = 0x00333333;
    }
}

void terminal_draw(void) {
    uint32_t* fb = goldfish_fb_get_buffer();
    uint32_t width = goldfish_fb_get_width();
    uint32_t height = goldfish_fb_get_height();

    /* Clear screen */
    goldfish_fb_clear(color_bg);

    /* Draw title bar */
    draw_title_bar(fb, width, back_btn_pressed);

    /* Adjust available height for title bar and keyboard */
    int kb_h = keyboard_get_height();
    int available_height = height - kb_h - TITLE_BAR_HEIGHT;
    int content_start_y = TITLE_BAR_HEIGHT;

    /* Calculate visible lines */
    int line_height = FONT_HEIGHT + 2;
    max_visible_lines = (available_height - line_height - 20) / line_height;

    /* Calculate view window with scroll offset */
    int end_line = history_count - scroll_offset;
    int start_line = end_line - max_visible_lines;
    if (start_line < 0) start_line = 0;
    if (end_line < 0) end_line = 0;

    /* Draw history */
    int y = content_start_y + 10;
    for (int i = start_line; i < end_line; i++) {
        int idx = (history_head - history_count + i + MAX_HISTORY) % MAX_HISTORY;
        draw_string(fb, 10, y, history[idx], color_text, width, height);
        y += line_height;
    }

    /* Draw scroll indicator if scrolled */
    if (scroll_offset > 0) {
        char scroll_info[32];
        int pos = 0;
        scroll_info[pos++] = '[';
        scroll_info[pos++] = '+';
        int so = scroll_offset;
        if (so >= 10) scroll_info[pos++] = '0' + (so / 10) % 10;
        scroll_info[pos++] = '0' + so % 10;
        scroll_info[pos++] = ']';
        scroll_info[pos] = 0;
        draw_string(fb, width - 50, content_start_y + 10, scroll_info, 0x0033FF33, width, height);
    }

    /* Draw prompt and current command */
    char prompt[MAX_CMD_LEN + 4];
    prompt[0] = '>';
    prompt[1] = ' ';
    for (int i = 0; i <= cmd_pos; i++) {
        prompt[i + 2] = cmd_buffer[i];
    }
    /* Add cursor */
    prompt[cmd_pos + 2] = '_';
    prompt[cmd_pos + 3] = 0;

    int prompt_y = height - kb_h - line_height - 10;
    draw_string(fb, 10, prompt_y, prompt, color_prompt, width, height);

    /* Draw soft keyboard if visible */
    keyboard_draw(fb, width, height);

    /* Flush display */
    goldfish_fb_flush();
    needs_redraw = 0;
}

int terminal_should_close(void) {
    return want_close;
}

void terminal_clear_close(void) {
    want_close = 0;
}
