/*
 * Virtio Input Driver
 * Handles keyboard and multi-touch input from Android emulator
 */

#include "types.h"
#include "virtio_input.h"
#include "event.h"
#include "gic.h"

/* Virtio MMIO scan range */
#define VIRTIO_MMIO_START       0x0a000000
#define VIRTIO_MMIO_SIZE        0x200
#define VIRTIO_MMIO_COUNT       32

/* Virtio MMIO registers */
#define VIRTIO_MMIO_MAGIC           0x000
#define VIRTIO_MMIO_VERSION         0x004
#define VIRTIO_MMIO_DEVICE_ID       0x008
#define VIRTIO_MMIO_VENDOR_ID       0x00c
#define VIRTIO_MMIO_DEV_FEAT        0x010
#define VIRTIO_MMIO_DEV_FEAT_SEL    0x014
#define VIRTIO_MMIO_DRV_FEAT        0x020
#define VIRTIO_MMIO_DRV_FEAT_SEL    0x024
#define VIRTIO_MMIO_GUEST_PAGE_SIZE 0x028
#define VIRTIO_MMIO_QUEUE_SEL       0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX   0x034
#define VIRTIO_MMIO_QUEUE_NUM       0x038
#define VIRTIO_MMIO_QUEUE_ALIGN     0x03c
#define VIRTIO_MMIO_QUEUE_PFN       0x040
#define VIRTIO_MMIO_QUEUE_READY     0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY    0x050
#define VIRTIO_MMIO_INT_STATUS      0x060
#define VIRTIO_MMIO_INT_ACK         0x064
#define VIRTIO_MMIO_STATUS          0x070
#define VIRTIO_MMIO_QUEUE_DESC_LOW  0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH 0x084
#define VIRTIO_MMIO_QUEUE_AVAIL_LOW 0x090
#define VIRTIO_MMIO_QUEUE_AVAIL_HIGH 0x094
#define VIRTIO_MMIO_QUEUE_USED_LOW  0x0a0
#define VIRTIO_MMIO_QUEUE_USED_HIGH 0x0a4

/* Virtqueue descriptor flags */
#define VIRTQ_DESC_F_NEXT       1
#define VIRTQ_DESC_F_WRITE      2

/* Maximum input devices */
#define MAX_INPUT_DEVICES   4

/* Memory for virtqueues (in RAM region) */
#define INPUT_VIRTQUEUE_BASE    0x46200000
#define INPUT_BUFFER_BASE       0x46300000

/* Virtqueue structures */
struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed, aligned(16)));

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[64];
} __attribute__((packed, aligned(2)));

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[64];
} __attribute__((packed, aligned(4)));

/* Input device state */
struct input_device {
    uint64_t base;          /* MMIO base address */
    int version;            /* Virtio version (1 or 2) */
    int irq;                /* IRQ number */
    int is_keyboard;        /* 1 = keyboard, 0 = touch */
    int active;             /* Device is active */

    /* Virtqueue pointers */
    struct virtq_desc* desc;
    struct virtq_avail* avail;
    struct virtq_used* used;
    uint16_t queue_size;
    uint16_t last_used;

    /* Event buffers */
    struct virtio_input_event* events;
};

static struct input_device input_devices[MAX_INPUT_DEVICES];
static int num_input_devices = 0;
static int keyboard_available = 0;
static int touch_available = 0;

/* Current touch state - initialize to center of typical screen */
static int32_t touch_x = 540;
static int32_t touch_y = 1200;
static int touch_slot = 0;
static int touch_tracking_id = -1;

static inline void mmio_write(uint64_t base, uint32_t offset, uint32_t value) {
    *(volatile uint32_t*)(base + offset) = value;
    __asm__ volatile("dmb sy" ::: "memory");
}

static inline uint32_t mmio_read(uint64_t base, uint32_t offset) {
    __asm__ volatile("dmb sy" ::: "memory");
    return *(volatile uint32_t*)(base + offset);
}

/* Debug UART output */
#define UART0_BASE 0x09000000
static void debug_putc(char c) {
    *(volatile uint32_t*)UART0_BASE = c;
}

static void debug_puts(const char* s) {
    while (*s) debug_putc(*s++);
}

static void debug_hex16(uint16_t val) {
    const char hex[] = "0123456789ABCDEF";
    debug_putc(hex[(val >> 12) & 0xF]);
    debug_putc(hex[(val >> 8) & 0xF]);
    debug_putc(hex[(val >> 4) & 0xF]);
    debug_putc(hex[val & 0xF]);
}

static void debug_hex32(uint32_t val) {
    const char hex[] = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4) {
        debug_putc(hex[(val >> i) & 0xF]);
    }
}

static void debug_dec(uint32_t val) {
    char buf[12];
    int i = 10;
    buf[11] = 0;
    if (val == 0) {
        debug_putc('0');
        return;
    }
    while (val > 0) {
        buf[i--] = '0' + (val % 10);
        val /= 10;
    }
    debug_puts(&buf[i + 1]);
}

/* Touch debug mode */
static int touch_debug = 0;

void virtio_input_set_debug(int enable) {
    touch_debug = enable;
}

/* Forward declarations */
static void goldfish_events_poll(void);

/* Initialize virtqueue for an input device */
static void init_virtqueue(struct input_device* dev, int dev_idx) {
    uint64_t queue_base = INPUT_VIRTQUEUE_BASE + dev_idx * 0x10000;
    uint64_t event_base = INPUT_BUFFER_BASE + dev_idx * 0x10000;

    /* Select eventq (queue 0) */
    mmio_write(dev->base, VIRTIO_MMIO_QUEUE_SEL, 0);

    /* Get max queue size */
    dev->queue_size = mmio_read(dev->base, VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (dev->queue_size > 64) dev->queue_size = 64;
    if (dev->queue_size == 0) dev->queue_size = 16;

    /* Set queue size */
    mmio_write(dev->base, VIRTIO_MMIO_QUEUE_NUM, dev->queue_size);

    /* Calculate queue layout */
    uint32_t desc_size = dev->queue_size * 16;
    uint32_t avail_size = 6 + 2 * dev->queue_size;
    uint32_t used_offset = (desc_size + avail_size + 4095) & ~4095;

    dev->desc = (struct virtq_desc*)(queue_base);
    dev->avail = (struct virtq_avail*)(queue_base + desc_size);
    dev->used = (struct virtq_used*)(queue_base + used_offset);
    dev->events = (struct virtio_input_event*)event_base;
    dev->last_used = 0;

    /* Clear queue memory */
    uint8_t* ptr = (uint8_t*)queue_base;
    for (uint32_t i = 0; i < used_offset + 4096; i++) {
        ptr[i] = 0;
    }

    /* Initialize descriptors with event buffers */
    for (int i = 0; i < dev->queue_size; i++) {
        dev->desc[i].addr = (uint64_t)&dev->events[i];
        dev->desc[i].len = sizeof(struct virtio_input_event);
        dev->desc[i].flags = VIRTQ_DESC_F_WRITE;
        dev->desc[i].next = 0;

        /* Add to available ring */
        dev->avail->ring[i] = i;
    }
    dev->avail->idx = dev->queue_size;

    /* Set up queue based on virtio version */
    if (dev->version == 1) {
        mmio_write(dev->base, VIRTIO_MMIO_QUEUE_ALIGN, 4096);
        mmio_write(dev->base, VIRTIO_MMIO_QUEUE_PFN, (uint32_t)(queue_base >> 12));
    } else {
        mmio_write(dev->base, VIRTIO_MMIO_QUEUE_DESC_LOW, (uint32_t)(uint64_t)dev->desc);
        mmio_write(dev->base, VIRTIO_MMIO_QUEUE_DESC_HIGH, (uint32_t)((uint64_t)dev->desc >> 32));
        mmio_write(dev->base, VIRTIO_MMIO_QUEUE_AVAIL_LOW, (uint32_t)(uint64_t)dev->avail);
        mmio_write(dev->base, VIRTIO_MMIO_QUEUE_AVAIL_HIGH, (uint32_t)((uint64_t)dev->avail >> 32));
        mmio_write(dev->base, VIRTIO_MMIO_QUEUE_USED_LOW, (uint32_t)(uint64_t)dev->used);
        mmio_write(dev->base, VIRTIO_MMIO_QUEUE_USED_HIGH, (uint32_t)((uint64_t)dev->used >> 32));
        mmio_write(dev->base, VIRTIO_MMIO_QUEUE_READY, 1);
    }
}

/* Track if we got position data since last sync */
static int got_touch_data = 0;
static int touch_is_down = 0;

/* Button codes */
#define BTN_LEFT        0x110
#define BTN_RIGHT       0x111
#define BTN_MIDDLE      0x112
#define BTN_TOOL_FINGER 0x145
#define BTN_TOUCH       0x14a
#define BTN_TOOL_PEN    0x140

/* Process a single input event */
static void process_input_event(struct input_device* dev, struct virtio_input_event* ev) {
    (void)dev;

    /* Debug output for all events */
    if (touch_debug) {
        debug_puts("EV: t=");
        debug_hex16(ev->type);
        debug_puts(" c=");
        debug_hex16(ev->code);
        debug_puts(" v=");
        debug_hex32(ev->value);
        debug_puts("\r\n");
    }

    /* Handle keyboard events (EV_KEY with code < 0x100) */
    if (ev->type == EV_KEY && ev->code < 0x100) {
        event_push_key(ev->code, ev->value);
        return;
    }

    /* Handle mouse/touch buttons */
    if (ev->type == EV_KEY) {
        if (ev->code == BTN_LEFT || ev->code == BTN_TOUCH ||
            ev->code == BTN_TOOL_FINGER || ev->code == BTN_TOOL_PEN) {
            if (ev->value) {
                /* Button pressed - immediately push TOUCH_DOWN */
                touch_is_down = 1;
                touch_tracking_id = 0;
                event_push_touch(touch_slot, TOUCH_DOWN, touch_x, touch_y);
                if (touch_debug) {
                    debug_puts("BTN DOWN x=");
                    debug_hex32(touch_x);
                    debug_puts(" y=");
                    debug_hex32(touch_y);
                    debug_puts("\r\n");
                }
            } else {
                /* Button released */
                event_push_touch(touch_slot, TOUCH_UP, touch_x, touch_y);
                touch_is_down = 0;
                touch_tracking_id = -1;
                if (touch_debug) debug_puts("BTN UP\r\n");
            }
            return;
        }
    }

    /* Handle relative mouse movement (EV_REL) */
    if (ev->type == EV_REL) {
        if (ev->code == 0x00) {  /* REL_X */
            touch_x += (int32_t)ev->value;
            /* Clamp to screen bounds (assume 1080x2400) */
            if (touch_x < 0) touch_x = 0;
            if (touch_x > 1080) touch_x = 1080;
            got_touch_data = 1;
        } else if (ev->code == 0x01) {  /* REL_Y */
            touch_y += (int32_t)ev->value;
            if (touch_y < 0) touch_y = 0;
            if (touch_y > 2400) touch_y = 2400;
            got_touch_data = 1;
        } else if (ev->code == 0x08) {  /* REL_WHEEL - mouse scroll wheel */
            /* Positive = scroll up (older), negative = scroll down (newer) */
            int32_t scroll = (int32_t)ev->value;
            if (scroll > 0) {
                event_push_touch(0, TOUCH_SCROLL_UP, 0, scroll);
            } else if (scroll < 0) {
                event_push_touch(0, TOUCH_SCROLL_DOWN, 0, -scroll);
            }
            if (touch_debug) {
                debug_puts("WHEEL: ");
                debug_hex32(ev->value);
                debug_puts("\r\n");
            }
            return;
        }
        /* Don't return - let it fall through to sync handling */
    }

    /* Handle absolute touch/tablet events */
    if (ev->type == EV_ABS) {
        if (ev->code == ABS_X || ev->code == ABS_MT_POSITION_X) {
            touch_x = ev->value;
            got_touch_data = 1;
        } else if (ev->code == ABS_Y || ev->code == ABS_MT_POSITION_Y) {
            touch_y = ev->value;
            got_touch_data = 1;
        } else if (ev->code == ABS_MT_SLOT) {
            touch_slot = ev->value;
        } else if (ev->code == ABS_MT_TRACKING_ID) {
            if ((int32_t)ev->value == -1) {
                event_push_touch(touch_slot, TOUCH_UP, touch_x, touch_y);
                touch_tracking_id = -1;
                touch_is_down = 0;
                if (touch_debug) debug_puts("MT LIFT\r\n");
            } else {
                touch_tracking_id = ev->value;
                touch_is_down = 1;
                if (touch_debug) debug_puts("MT TOUCH\r\n");
            }
        }
        return;
    }

    /* Handle sync events */
    if (ev->type == EV_SYN && ev->code == 0) {
        /* SYN_REPORT - report accumulated touch data */
        if (got_touch_data && touch_is_down) {
            /* Use TOUCH_MOVE for position updates while touching */
            event_push_touch(touch_slot, TOUCH_MOVE, touch_x, touch_y);
            if (touch_debug) {
                debug_puts("MOVE: x=");
                debug_hex32(touch_x);
                debug_puts(" y=");
                debug_hex32(touch_y);
                debug_puts("\r\n");
            }
        }
        got_touch_data = 0;
    }
}

/* Virtio-input config registers (at offset 0x100) */
#define VIRTIO_INPUT_CFG_BASE       0x100

/* Config select values */
#define VIRTIO_INPUT_CFG_ID_NAME    0x01
#define VIRTIO_INPUT_CFG_EV_BITS    0x11

/* Query input device capabilities using 32-bit aligned access
 * Config layout: select(8) | subsel(8) | size(8) | reserved(8) */
static int query_ev_bits(uint64_t base, uint8_t ev_type) {
    volatile uint32_t* cfg = (volatile uint32_t*)(base + VIRTIO_INPUT_CFG_BASE);

    /* Write select=0x11 (EV_BITS) and subsel=ev_type in one 32-bit write */
    /* Little endian: byte0=select, byte1=subsel */
    cfg[0] = VIRTIO_INPUT_CFG_EV_BITS | (ev_type << 8);

    /* Memory barrier and delay */
    __asm__ volatile("dmb sy" ::: "memory");
    for (volatile int i = 0; i < 1000; i++);

    /* Read back - size is in byte 2 of first word */
    uint32_t val = cfg[0];
    int size = (val >> 16) & 0xFF;

    return size;
}

/* Initialize a single input device */
static int init_input_device(uint64_t base, int dev_idx) {
    debug_puts("  init dev ");
    debug_dec(dev_idx);
    debug_puts("...\r\n");

    struct input_device* dev = &input_devices[dev_idx];
    dev->base = base;
    dev->active = 0;

    /* Get version and reset */
    dev->version = mmio_read(base, VIRTIO_MMIO_VERSION);
    debug_puts("    v");
    debug_dec(dev->version);

    mmio_write(base, VIRTIO_MMIO_STATUS, 0);
    for (volatile int i = 0; i < 10000; i++);

    /* Legacy v1: set guest page size */
    if (dev->version == 1) {
        mmio_write(base, VIRTIO_MMIO_GUEST_PAGE_SIZE, 4096);
    }

    /* Acknowledge */
    mmio_write(base, VIRTIO_MMIO_STATUS, 1);
    debug_puts(" ack");

    /* Driver loaded */
    mmio_write(base, VIRTIO_MMIO_STATUS, 1 | 2);
    debug_puts(" drv");

    /* Accept features (none required for input) */
    mmio_write(base, VIRTIO_MMIO_DEV_FEAT_SEL, 0);
    mmio_write(base, VIRTIO_MMIO_DRV_FEAT_SEL, 0);
    mmio_write(base, VIRTIO_MMIO_DRV_FEAT, 0);
    debug_puts(" feat");

    /* Initialize virtqueue */
    debug_puts(" vq...");
    init_virtqueue(dev, dev_idx);
    debug_puts("ok");

    /* Notify device that buffers are available */
    mmio_write(base, VIRTIO_MMIO_QUEUE_NOTIFY, 0);
    debug_puts(" notified");

    /* Features OK (v2) and Driver OK */
    if (dev->version == 1) {
        mmio_write(base, VIRTIO_MMIO_STATUS, 1 | 2 | 4);
    } else {
        mmio_write(base, VIRTIO_MMIO_STATUS, 1 | 2 | 8);
        mmio_write(base, VIRTIO_MMIO_STATUS, 1 | 2 | 8 | 4);
    }

    /* Notify again after device is ready */
    mmio_write(base, VIRTIO_MMIO_QUEUE_NOTIFY, 0);
    debug_puts(" ready\r\n");

    /* Query device capabilities */
    debug_puts("    cfg: ");
    int has_keys = query_ev_bits(base, EV_KEY);
    debug_puts("key=");
    debug_dec(has_keys);
    int has_rel = query_ev_bits(base, EV_REL);
    debug_puts(" rel=");
    debug_dec(has_rel);
    int has_abs = query_ev_bits(base, EV_ABS);
    debug_puts(" abs=");
    debug_dec(has_abs);
    debug_puts("\r\n");

    /* Determine device type based on capabilities */
    if (has_abs > 0) {
        dev->is_keyboard = 0;
        touch_available = 1;
        debug_puts("    -> TOUCH\r\n");
    } else if (has_rel > 0) {
        dev->is_keyboard = 0;
        touch_available = 1;
        debug_puts("    -> MOUSE\r\n");
    } else {
        dev->is_keyboard = 1;
        keyboard_available = 1;
        debug_puts("    -> keyboard\r\n");
    }

    dev->irq = 32 + dev_idx + 16;
    dev->active = 1;

    return 0;
}

/* Debug: periodically show queue status */
static int poll_count = 0;

/* Poll all input devices for events (called from main loop) */
void virtio_input_poll(void) {
    poll_count++;

    /* Every ~5 seconds, show queue status for debugging */
    if (touch_debug && (poll_count % 50000) == 0) {
        for (int i = 0; i < num_input_devices; i++) {
            struct input_device* dev = &input_devices[i];
            if (!dev->active) continue;
            debug_puts("Q");
            debug_putc('0' + i);
            debug_puts(": avail=");
            debug_dec(dev->avail->idx);
            debug_puts(" used=");
            debug_dec(dev->used->idx);
            debug_puts(" last=");
            debug_dec(dev->last_used);
            debug_puts("\r\n");
        }
    }

    /* Poll virtio-input devices */
    for (int i = 0; i < num_input_devices; i++) {
        struct input_device* dev = &input_devices[i];
        if (!dev->active) continue;

        /* Check for used buffers */
        while (dev->last_used != dev->used->idx) {
            __asm__ volatile("dmb sy" ::: "memory");

            uint32_t idx = dev->last_used % dev->queue_size;
            uint32_t desc_idx = dev->used->ring[idx].id;

            /* Debug: show which device */
            if (touch_debug) {
                debug_puts("D");
                debug_putc('0' + i);
                debug_puts(" ");
            }

            /* Process the event */
            process_input_event(dev, &dev->events[desc_idx]);

            /* Return buffer to available ring */
            uint16_t avail_idx = dev->avail->idx % dev->queue_size;
            dev->avail->ring[avail_idx] = desc_idx;
            __asm__ volatile("dmb sy" ::: "memory");
            dev->avail->idx++;

            dev->last_used++;
        }

        /* Notify device we've refilled buffers */
        mmio_write(dev->base, VIRTIO_MMIO_QUEUE_NOTIFY, 0);

        /* Acknowledge any pending interrupts */
        uint32_t status = mmio_read(dev->base, VIRTIO_MMIO_INT_STATUS);
        if (status) {
            mmio_write(dev->base, VIRTIO_MMIO_INT_ACK, status);
        }
    }

    /* Also poll Goldfish events device */
    goldfish_events_poll();
}

/* Goldfish Events device (Android emulator touch/keyboard) */
#define GOLDFISH_EVENTS_BASE    0x09040000
#define GFEVENTS_READ           0x00
#define GFEVENTS_SET_PAGE       0x00
#define GFEVENTS_LEN            0x04
#define GFEVENTS_DATA           0x08
#define GFEVENTS_PAGE_NAME      0x00
#define GFEVENTS_PAGE_EVBITS    0x10
#define GFEVENTS_PAGE_ABSDATA   0x20

static volatile int goldfish_events_active = 0;

static void goldfish_events_poll(void) {
    if (!goldfish_events_active) return;

    volatile uint32_t* events = (volatile uint32_t*)GOLDFISH_EVENTS_BASE;

    /* Read up to 16 events per poll to avoid infinite loop */
    for (int count = 0; count < 16; count++) {
        uint32_t type = events[0];  /* Read type register */
        uint32_t code = events[1];  /* Read code register */
        uint32_t value = events[2]; /* Read value register */

        /* Check for no event (all zeros or type=0xFFFFFFFF means no device) */
        if ((type == 0 && code == 0 && value == 0) || type == 0xFFFFFFFF) {
            break;
        }

        if (touch_debug) {
            debug_puts("GF: t=");
            debug_hex16(type & 0xFFFF);
            debug_puts(" c=");
            debug_hex16(code & 0xFFFF);
            debug_puts(" v=");
            debug_hex32(value);
            debug_puts("\r\n");
        }

        /* Process the event using same logic as virtio */
        struct virtio_input_event ev;
        ev.type = type & 0xFFFF;
        ev.code = code & 0xFFFF;
        ev.value = value;
        process_input_event(NULL, &ev);
    }
}

static int goldfish_events_init(void) {
    /* Skip goldfish for now - it's not used in modern emulator */
    debug_puts("Goldfish events: skipped (using virtio-input)\r\n");
    goldfish_events_active = 0;
    return 0;
}

void virtio_input_init(void) {
    /* Initialize event queue */
    event_queue_init();

    debug_puts("Scanning for virtio devices...\r\n");

    /* Scan for virtio-input devices */
    for (int i = 0; i < VIRTIO_MMIO_COUNT && num_input_devices < MAX_INPUT_DEVICES; i++) {
        uint64_t base = VIRTIO_MMIO_START + (i * VIRTIO_MMIO_SIZE);

        uint32_t magic = mmio_read(base, VIRTIO_MMIO_MAGIC);
        uint32_t device_id = mmio_read(base, VIRTIO_MMIO_DEVICE_ID);

        /* Show all devices found */
        if (magic == 0x74726976 && device_id != 0) {
            debug_puts("  Slot ");
            debug_dec(i);
            debug_puts(": virtio ID ");
            debug_dec(device_id);
            if (device_id == 1) debug_puts(" (net)");
            else if (device_id == 2) debug_puts(" (blk)");
            else if (device_id == 3) debug_puts(" (con)");
            else if (device_id == 16) debug_puts(" (gpu)");
            else if (device_id == 18) debug_puts(" (input)");
            debug_puts("\r\n");
        }

        /* Check for virtio-input device (ID 18) */
        if (magic == 0x74726976 && device_id == VIRTIO_DEVICE_INPUT) {
            if (init_input_device(base, num_input_devices) == 0) {
                num_input_devices++;
            }
        }
    }

    debug_puts("VirtIO: ");
    debug_dec(num_input_devices);
    debug_puts(" input device(s)\r\n");

    /* Also try Goldfish events (Android emulator legacy input) */
    goldfish_events_init();

    debug_puts("Input init complete\r\n");
}

int virtio_input_keyboard_available(void) {
    return keyboard_available;
}

int virtio_input_touch_available(void) {
    return touch_available;
}

int virtio_input_pending(void) {
    return event_pending();
}

void virtio_input_get_touch(int32_t* x, int32_t* y, int* is_down) {
    if (x) *x = touch_x;
    if (y) *y = touch_y;
    if (is_down) *is_down = touch_is_down;
}
