/*
 * TinyOS Event Queue
 * Lock-free single-producer single-consumer ring buffer
 */

#ifndef EVENT_H
#define EVENT_H

#include "types.h"

/* Event types */
#define EVENT_NONE      0
#define EVENT_KEY       1   /* Keyboard event */
#define EVENT_TOUCH     2   /* Touch event */

/* Event subtypes */
#define KEY_PRESS       1   /* Key pressed */
#define KEY_RELEASE     0   /* Key released */

#define TOUCH_DOWN      1   /* Touch started */
#define TOUCH_UP        0   /* Touch ended */
#define TOUCH_MOVE      2   /* Touch moved */
#define TOUCH_SCROLL_UP   3   /* Scroll up (mouse wheel or swipe) */
#define TOUCH_SCROLL_DOWN 4   /* Scroll down (mouse wheel or swipe) */

/* Input event structure */
typedef struct {
    uint8_t type;       /* EVENT_KEY or EVENT_TOUCH */
    uint8_t subtype;    /* KEY_PRESS/RELEASE or TOUCH_DOWN/UP/MOVE */
    uint16_t code;      /* Key code or touch slot ID */
    int32_t x;          /* Touch X coordinate (0 for keyboard) */
    int32_t y;          /* Touch Y coordinate (0 for keyboard) */
} input_event_t;

/* Event queue size (must be power of 2) */
#define EVENT_QUEUE_SIZE    256
#define EVENT_QUEUE_MASK    (EVENT_QUEUE_SIZE - 1)

/* Initialize the event queue */
void event_queue_init(void);

/* Push an event to the queue (called from IRQ context)
 * Returns 0 on success, -1 if queue is full */
int event_push(const input_event_t* event);

/* Pop an event from the queue
 * Returns 0 on success, -1 if queue is empty */
int event_pop(input_event_t* event);

/* Check if queue has pending events */
int event_pending(void);

/* Get number of pending events */
int event_count(void);

/* Helper: push a keyboard event */
void event_push_key(uint16_t keycode, int pressed);

/* Helper: push a touch event */
void event_push_touch(uint16_t slot, int subtype, int32_t x, int32_t y);

#endif /* EVENT_H */
