/*
 * Virtio Input driver definitions
 * Based on Linux virtio-input specification
 */

#ifndef VIRTIO_INPUT_H
#define VIRTIO_INPUT_H

#include "types.h"

/* Virtio device ID for input */
#define VIRTIO_DEVICE_INPUT     18

/* Linux input event types */
#define EV_SYN      0x00    /* Synchronization event */
#define EV_KEY      0x01    /* Key/button event */
#define EV_REL      0x02    /* Relative axis (mouse) */
#define EV_ABS      0x03    /* Absolute axis (touch) */

/* Key codes (subset) */
#define KEY_RESERVED    0
#define KEY_ESC         1
#define KEY_1           2
#define KEY_2           3
#define KEY_3           4
#define KEY_4           5
#define KEY_5           6
#define KEY_6           7
#define KEY_7           8
#define KEY_8           9
#define KEY_9           10
#define KEY_0           11
#define KEY_MINUS       12
#define KEY_EQUAL       13
#define KEY_BACKSPACE   14
#define KEY_TAB         15
#define KEY_Q           16
#define KEY_W           17
#define KEY_E           18
#define KEY_R           19
#define KEY_T           20
#define KEY_Y           21
#define KEY_U           22
#define KEY_I           23
#define KEY_O           24
#define KEY_P           25
#define KEY_LEFTBRACE   26
#define KEY_RIGHTBRACE  27
#define KEY_ENTER       28
#define KEY_LEFTCTRL    29
#define KEY_A           30
#define KEY_S           31
#define KEY_D           32
#define KEY_F           33
#define KEY_G           34
#define KEY_H           35
#define KEY_J           36
#define KEY_K           37
#define KEY_L           38
#define KEY_SEMICOLON   39
#define KEY_APOSTROPHE  40
#define KEY_GRAVE       41
#define KEY_LEFTSHIFT   42
#define KEY_BACKSLASH   43
#define KEY_Z           44
#define KEY_X           45
#define KEY_C           46
#define KEY_V           47
#define KEY_B           48
#define KEY_N           49
#define KEY_M           50
#define KEY_COMMA       51
#define KEY_DOT         52
#define KEY_SLASH       53
#define KEY_RIGHTSHIFT  54
#define KEY_KPASTERISK  55
#define KEY_LEFTALT     56
#define KEY_SPACE       57
#define KEY_CAPSLOCK    58
#define KEY_UP          103
#define KEY_LEFT        105
#define KEY_RIGHT       106
#define KEY_DOWN        108

/* Absolute axis codes (for touch) */
#define ABS_X               0x00
#define ABS_Y               0x01
#define ABS_MT_SLOT         0x2f
#define ABS_MT_TOUCH_MAJOR  0x30
#define ABS_MT_POSITION_X   0x35
#define ABS_MT_POSITION_Y   0x36
#define ABS_MT_TRACKING_ID  0x39

/* Virtio input event structure (matches Linux) */
struct virtio_input_event {
    uint16_t type;      /* EV_KEY, EV_ABS, etc. */
    uint16_t code;      /* Key code or axis code */
    uint32_t value;     /* 1=press, 0=release, or axis value */
} __attribute__((packed));

/* Initialize virtio-input driver */
void virtio_input_init(void);

/* Check if keyboard is available */
int virtio_input_keyboard_available(void);

/* Check if touch is available */
int virtio_input_touch_available(void);

/* Get number of pending events */
int virtio_input_pending(void);

/* Poll for input events (call from main loop) */
void virtio_input_poll(void);

/* Enable/disable debug output for touch events */
void virtio_input_set_debug(int enable);

/* Get current touch position (for global access) */
void virtio_input_get_touch(int32_t* x, int32_t* y, int* is_down);

#endif /* VIRTIO_INPUT_H */
