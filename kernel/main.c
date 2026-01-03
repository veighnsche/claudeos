#include "cursor.h"
#include "filemanager.h"
#include "font.h"
#include "fs.h"
#include "gic.h"
#include "goldfish_fb.h"
#include "home.h"
#include "http.h"
#include "memory.h"
#include "net.h"
#include "tcp.h"
#include "terminal.h"
#include "virtio_blk.h"
#include "virtio_input.h"
#include "virtio_net.h"

/* UI State */
#define STATE_HOME 0
#define STATE_TERMINAL 1
#define STATE_FILES 2
static int ui_state = STATE_HOME;

/* Fake Linux version string for Android emulator compatibility */
const char linux_banner[] __attribute__((used, section(".rodata"))) =
    "Linux version 5.10.0-tinyos (tinyos@local) #1 SMP PREEMPT TinyOS";

/* UART for debug output */
#ifdef __aarch64__
/* ARM64 Android emulator uses PL011 at different address */
#define UART0_BASE 0x09000000
#else
/* VersatilePB UART */
#define UART0_BASE 0x101f1000
#endif
#define UART0_DR (*(volatile uint32_t *)(UART0_BASE))

static void uart_putc(char c) { UART0_DR = c; }

static void uart_puts(const char *s) {
  while (*s) {
    uart_putc(*s++);
  }
}

static void uart_hex(uint64_t val) {
  const char hex[] = "0123456789ABCDEF";
  uart_puts("0x");
  for (int i = 60; i >= 0; i -= 4) {
    uart_putc(hex[(val >> i) & 0xF]);
  }
}

static void test_memory_allocator(void) {
  uart_puts("Testing memory allocator...\r\n");

  /* Test 1: Basic allocation */
  uart_puts("  malloc(100): ");
  void *p1 = malloc(100);
  uart_hex((uint64_t)p1);
  uart_puts(p1 ? " OK\r\n" : " FAILED\r\n");

  /* Test 2: Write to allocated memory */
  if (p1) {
    memset(p1, 0xAB, 100);
    uart_puts("  memset: OK\r\n");
  }

  /* Test 3: Second allocation */
  uart_puts("  malloc(200): ");
  void *p2 = malloc(200);
  uart_hex((uint64_t)p2);
  uart_puts(p2 ? " OK\r\n" : " FAILED\r\n");

  /* Test 4: Free first block */
  uart_puts("  free(p1): ");
  free(p1);
  uart_puts("OK\r\n");

  /* Test 5: Reallocate - should reuse freed block */
  uart_puts("  malloc(50): ");
  void *p3 = malloc(50);
  uart_hex((uint64_t)p3);
  uart_puts(p3 ? " OK\r\n" : " FAILED\r\n");

  /* Test 6: calloc */
  uart_puts("  calloc(10, 10): ");
  void *p4 = calloc(10, 10);
  uart_hex((uint64_t)p4);
  uart_puts(p4 ? " OK\r\n" : " FAILED\r\n");

  /* Cleanup */
  free(p2);
  free(p3);
  free(p4);

  /* Report stats */
  uart_puts("  Heap free: ");
  uart_hex(heap_free_bytes());
  uart_puts(" bytes\r\n");

  uart_puts("Memory allocator test complete!\r\n");
}

void kernel_main(void) {
  uart_puts("\r\n*** TinyOS ***\r\n");

  /* Initialize GIC first (needed for interrupts) */
  gic_init();

  /* Initialize framebuffer FIRST - get GUI up immediately */
  goldfish_fb_init();

  /* Initialize input (needed for home screen) */
  virtio_input_init();

  /* Show home screen immediately - before network init */
  home_init();
  home_draw(); /* GUI visible now! */

  /* Initialize disk and filesystem (after display is up) */
  blk_init();
  fs_init();

  /* Enable interrupts */
  enable_interrupts();

  static uint32_t loop_count = 0;
  static int net_tried = 0;
  static int auto_curl_started = 0;
  static http_request_t auto_req;

  /* Main event loop */
  while (1) {
    loop_count++;

    /* Poll for input events */
    virtio_input_poll();

    /* Try network init ONCE after 10000 loops (GUI stable) */
    if (!net_tried && loop_count > 10000) {
      net_init();
      net_tried = 1;
    }

    /* Poll network only if init was attempted */
    if (net_tried) {
      net_poll();
    }

    /* Auto-fetch external IP after DHCP completes */
    net_config_t *nc = net_get_config();
    if (net_tried && nc->configured && !auto_curl_started) {
      if (http_request_start(&auto_req, HTTP_GET, "http://ifconfig.me/ip", NULL,
                             0) == 0) {
        auto_curl_started = 1;
      } else {
        auto_curl_started = 2; /* Mark as failed, don't retry */
      }
    }

    /* Poll external IP request */
    if (auto_curl_started == 1) {
      int state = http_request_poll(&auto_req);
      if (state == HTTP_STATE_DONE) {
        /* Store external IP for display in home screen */
        if (auto_req.response.body_len > 0) {
          home_set_external_ip(auto_req.response.body);
        }
        http_request_close(&auto_req);
        auto_curl_started = 2; /* Done */
      } else if (state == HTTP_STATE_ERROR) {
        http_request_close(&auto_req);
        auto_curl_started = 2;
      }
    }

    static int32_t last_cursor_x = -1;
    static int32_t last_cursor_y = -1;
    int32_t cur_x, cur_y;
    virtio_input_get_touch(&cur_x, &cur_y, NULL);
    int cursor_moved = (cur_x != last_cursor_x || cur_y != last_cursor_y);
    if (cursor_moved) {
      last_cursor_x = cur_x;
      last_cursor_y = cur_y;
    }

    uint32_t sw = goldfish_fb_get_width();
    uint32_t sh = goldfish_fb_get_height();
    uint32_t *fb = goldfish_fb_get_buffer();

    if (ui_state == STATE_HOME) {
      /* Home screen mode */
      if (home_update() || cursor_moved) {
        home_draw();
        cursor_draw(fb, sw, sh);
        goldfish_fb_flush();
      }

      /* Check if terminal icon was pressed */
      if (home_terminal_pressed()) {
        home_clear_pressed();
        ui_state = STATE_TERMINAL;
        terminal_init();
        terminal_draw();
        cursor_draw(fb, sw, sh);
        goldfish_fb_flush();
      }
      /* Check if files icon was pressed */
      else if (home_files_pressed()) {
        home_clear_pressed();
        ui_state = STATE_FILES;
        filemanager_init();
        filemanager_draw();
        cursor_draw(fb, sw, sh);
        goldfish_fb_flush();
      }
    } else if (ui_state == STATE_TERMINAL) {
      /* Terminal mode */
      if (terminal_update() || cursor_moved) {
        terminal_draw();
        cursor_draw(fb, sw, sh);
        goldfish_fb_flush();
      }

      if (terminal_should_close()) {
        terminal_clear_close();
        ui_state = STATE_HOME;
        home_init();
        home_draw();
        cursor_draw(fb, sw, sh);
        goldfish_fb_flush();
      }

      terminal_tick();
    } else if (ui_state == STATE_FILES) {
      /* File manager mode */
      if (filemanager_update() || cursor_moved) {
        filemanager_draw();
        cursor_draw(fb, sw, sh);
        goldfish_fb_flush();
      }

      if (filemanager_should_close()) {
        filemanager_clear_close();
        ui_state = STATE_HOME;
        home_init();
        home_draw();
        cursor_draw(fb, sw, sh);
        goldfish_fb_flush();
      }
    }

    /* Brief delay to avoid spinning too fast */
    for (volatile int i = 0; i < 10000; i++)
      ;
  }
}