/*
 * TinyOS Event Queue
 * Lock-free SPSC ring buffer for input events
 */

#include "event.h"

/* Ring buffer */
static input_event_t event_queue[EVENT_QUEUE_SIZE];
static volatile uint32_t queue_head = 0;    /* Write index (producer/IRQ) */
static volatile uint32_t queue_tail = 0;    /* Read index (consumer/main) */

void event_queue_init(void) {
    queue_head = 0;
    queue_tail = 0;
}

int event_push(const input_event_t* event) {
    uint32_t next_head = (queue_head + 1) & EVENT_QUEUE_MASK;

    /* Check if queue is full */
    if (next_head == queue_tail) {
        return -1;  /* Queue full, drop event */
    }

    /* Copy each field (struct assignment caused hangs) */
    event_queue[queue_head].type = event->type;
    event_queue[queue_head].subtype = event->subtype;
    event_queue[queue_head].code = event->code;
    event_queue[queue_head].x = event->x;
    event_queue[queue_head].y = event->y;

    /* Memory barrier before updating head */
    __asm__ volatile("dmb sy" ::: "memory");

    queue_head = next_head;
    return 0;
}

int event_pop(input_event_t* event) {
    /* Check if queue is empty */
    if (queue_tail == queue_head) {
        return -1;  /* Queue empty */
    }

    /* Copy event from queue field by field (struct assignment caused hangs) */
    event->type = event_queue[queue_tail].type;
    event->subtype = event_queue[queue_tail].subtype;
    event->code = event_queue[queue_tail].code;
    event->x = event_queue[queue_tail].x;
    event->y = event_queue[queue_tail].y;

    /* Memory barrier before updating tail */
    __asm__ volatile("dmb sy" ::: "memory");

    queue_tail = (queue_tail + 1) & EVENT_QUEUE_MASK;
    return 0;
}

int event_pending(void) {
    return queue_head != queue_tail;
}

int event_count(void) {
    return (queue_head - queue_tail) & EVENT_QUEUE_MASK;
}

void event_push_key(uint16_t keycode, int pressed) {
    input_event_t ev;
    ev.type = EVENT_KEY;
    ev.subtype = pressed ? KEY_PRESS : KEY_RELEASE;
    ev.code = keycode;
    ev.x = 0;
    ev.y = 0;
    event_push(&ev);
}

void event_push_touch(uint16_t slot, int subtype, int32_t x, int32_t y) {
    input_event_t ev;
    ev.type = EVENT_TOUCH;
    ev.subtype = subtype;
    ev.code = slot;
    ev.x = x;
    ev.y = y;
    event_push(&ev);
}
