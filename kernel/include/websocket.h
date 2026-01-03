/*
 * TinyOS WebSocket Client
 */
#ifndef WEBSOCKET_H
#define WEBSOCKET_H

#include "types.h"

/* WebSocket states */
#define WS_STATE_CLOSED      0
#define WS_STATE_CONNECTING  1
#define WS_STATE_OPEN        2
#define WS_STATE_CLOSING     3

/* WebSocket opcodes */
#define WS_OP_CONTINUATION 0x00
#define WS_OP_TEXT         0x01
#define WS_OP_BINARY       0x02
#define WS_OP_CLOSE        0x08
#define WS_OP_PING         0x09
#define WS_OP_PONG         0x0A

/* Buffer sizes */
#define WS_MAX_MESSAGE     2048
#define WS_MAX_HOST        64
#define WS_MAX_PATH        128

/* WebSocket connection */
typedef struct {
    int state;
    int tcp_conn;
    char host[WS_MAX_HOST];
    char path[WS_MAX_PATH];
    uint16_t port;

    /* Handshake state */
    int handshake_sent;
    int handshake_complete;
    char sec_key[32];       /* Base64 encoded key */

    /* Receive buffer */
    uint8_t rx_buffer[WS_MAX_MESSAGE];
    int rx_len;
    int rx_ready;
    uint8_t rx_opcode;

    /* Frame parsing state */
    int frame_state;
    int frame_len;
    int frame_mask;
    uint8_t frame_mask_key[4];

} websocket_t;

/* Initialize WebSocket system */
void ws_init(void);

/* Connect to WebSocket server */
int ws_connect(websocket_t* ws, const char* url);

/* Poll WebSocket (call repeatedly) */
int ws_poll(websocket_t* ws);

/* Send text message */
int ws_send_text(websocket_t* ws, const char* message);

/* Send binary message */
int ws_send_binary(websocket_t* ws, const uint8_t* data, int len);

/* Send ping */
int ws_send_ping(websocket_t* ws);

/* Check if message received */
int ws_message_ready(websocket_t* ws);

/* Get received message */
int ws_get_message(websocket_t* ws, char* buffer, int max_len);

/* Get message opcode */
int ws_get_opcode(websocket_t* ws);

/* Close connection */
void ws_close(websocket_t* ws);

/* Get state */
int ws_get_state(websocket_t* ws);

#endif
