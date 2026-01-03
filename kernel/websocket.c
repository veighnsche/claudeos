/*
 * TinyOS WebSocket Client Implementation
 */

#include "websocket.h"
#include "tcp.h"
#include "http.h"
#include "net.h"
#include "memory.h"

/* Debug output */
#define WS_UART ((volatile uint32_t*)0x09000000)
static void ws_log(const char* msg) {
    while (*msg) *WS_UART = *msg++;
}

/* Simple string utilities */
static int str_len(const char* s) {
    int len = 0;
    while (s[len]) len++;
    return len;
}

static void str_cpy(char* dst, const char* src, int max) {
    int i;
    for (i = 0; i < max - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

static int str_ncmp(const char* a, const char* b, int n) {
    while (n > 0 && *a && *b && *a == *b) { a++; b++; n--; }
    if (n == 0) return 0;
    return *a - *b;
}

/* Simple random number for key generation */
static uint32_t ws_rand_seed = 0x12345678;
static uint32_t ws_rand(void) {
    ws_rand_seed = ws_rand_seed * 1103515245 + 12345;
    return ws_rand_seed;
}

/* Base64 encoding table */
static const char base64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Base64 encode */
static void base64_encode(const uint8_t* input, int len, char* output) {
    int i, j;
    for (i = 0, j = 0; i < len; i += 3) {
        uint32_t triple = (input[i] << 16);
        if (i + 1 < len) triple |= (input[i + 1] << 8);
        if (i + 2 < len) triple |= input[i + 2];

        output[j++] = base64_table[(triple >> 18) & 0x3F];
        output[j++] = base64_table[(triple >> 12) & 0x3F];
        output[j++] = (i + 1 < len) ? base64_table[(triple >> 6) & 0x3F] : '=';
        output[j++] = (i + 2 < len) ? base64_table[triple & 0x3F] : '=';
    }
    output[j] = 0;
}

/* Generate WebSocket key (16 random bytes, base64 encoded) */
static void generate_ws_key(char* key_out) {
    uint8_t random_bytes[16];
    for (int i = 0; i < 16; i += 4) {
        uint32_t r = ws_rand();
        random_bytes[i] = r & 0xFF;
        random_bytes[i + 1] = (r >> 8) & 0xFF;
        random_bytes[i + 2] = (r >> 16) & 0xFF;
        random_bytes[i + 3] = (r >> 24) & 0xFF;
    }
    base64_encode(random_bytes, 16, key_out);
}

void ws_init(void) {
    /* Seed random with some entropy */
    ws_rand_seed = 0x12345678;  /* In real impl, use timer/etc */
    ws_log("WS: Init\r\n");
}

/* Parse WebSocket URL (ws:// or wss://) */
static int parse_ws_url(const char* url, websocket_t* ws) {
    const char* p = url;
    int is_secure = 0;

    if (str_ncmp(p, "wss://", 6) == 0) {
        is_secure = 1;
        p += 6;
    } else if (str_ncmp(p, "ws://", 5) == 0) {
        p += 5;
    } else {
        return -1;
    }

    /* Parse host */
    int host_len = 0;
    while (*p && *p != ':' && *p != '/' && host_len < WS_MAX_HOST - 1) {
        ws->host[host_len++] = *p++;
    }
    ws->host[host_len] = 0;

    /* Parse port */
    ws->port = is_secure ? 443 : 80;
    if (*p == ':') {
        p++;
        ws->port = 0;
        while (*p >= '0' && *p <= '9') {
            ws->port = ws->port * 10 + (*p - '0');
            p++;
        }
    }

    /* Parse path */
    if (*p == '/') {
        str_cpy(ws->path, p, WS_MAX_PATH);
    } else {
        str_cpy(ws->path, "/", WS_MAX_PATH);
    }

    if (is_secure) {
        ws_log("WS: WSS not supported\r\n");
        return -1;
    }

    return 0;
}

int ws_connect(websocket_t* ws, const char* url) {
    memset(ws, 0, sizeof(websocket_t));

    if (parse_ws_url(url, ws) != 0) {
        return -1;
    }

    /* Resolve host */
    uint8_t ip[4];
    if (http_resolve_host(ws->host, ip) != 0) {
        ws_log("WS: DNS fail\r\n");
        return -1;
    }

    ws_log("WS: Connect to ");
    ws_log(ws->host);
    ws_log("\r\n");

    /* Generate WebSocket key */
    generate_ws_key(ws->sec_key);

    /* Start TCP connection */
    ws->tcp_conn = tcp_connect(ip, ws->port);
    if (ws->tcp_conn < 0) {
        ws_log("WS: TCP fail\r\n");
        return -1;
    }

    ws->state = WS_STATE_CONNECTING;
    ws->handshake_sent = 0;
    ws->handshake_complete = 0;

    return 0;
}

/* Build WebSocket upgrade request */
static int build_upgrade_request(websocket_t* ws, char* buf, int buf_size) {
    int pos = 0;

    /* GET /path HTTP/1.1 */
    const char* s = "GET ";
    while (*s && pos < buf_size - 1) buf[pos++] = *s++;
    s = ws->path;
    while (*s && pos < buf_size - 1) buf[pos++] = *s++;
    s = " HTTP/1.1\r\n";
    while (*s && pos < buf_size - 1) buf[pos++] = *s++;

    /* Host header */
    s = "Host: ";
    while (*s && pos < buf_size - 1) buf[pos++] = *s++;
    s = ws->host;
    while (*s && pos < buf_size - 1) buf[pos++] = *s++;
    s = "\r\n";
    while (*s && pos < buf_size - 1) buf[pos++] = *s++;

    /* Upgrade headers */
    s = "Upgrade: websocket\r\n";
    while (*s && pos < buf_size - 1) buf[pos++] = *s++;

    s = "Connection: Upgrade\r\n";
    while (*s && pos < buf_size - 1) buf[pos++] = *s++;

    /* WebSocket key */
    s = "Sec-WebSocket-Key: ";
    while (*s && pos < buf_size - 1) buf[pos++] = *s++;
    s = ws->sec_key;
    while (*s && pos < buf_size - 1) buf[pos++] = *s++;
    s = "\r\n";
    while (*s && pos < buf_size - 1) buf[pos++] = *s++;

    /* WebSocket version */
    s = "Sec-WebSocket-Version: 13\r\n";
    while (*s && pos < buf_size - 1) buf[pos++] = *s++;

    /* End of headers */
    s = "\r\n";
    while (*s && pos < buf_size - 1) buf[pos++] = *s++;

    buf[pos] = 0;
    return pos;
}

/* Check for upgrade response (HTTP 101) */
static int check_upgrade_response(const char* data, int len) {
    /* Look for "101 Switching Protocols" */
    if (len < 20) return 0;

    /* Simple check for HTTP/1.1 101 */
    if (str_ncmp(data, "HTTP/1.1 101", 12) == 0 ||
        str_ncmp(data, "HTTP/1.0 101", 12) == 0) {
        /* Check for end of headers */
        for (int i = 0; i < len - 3; i++) {
            if (data[i] == '\r' && data[i+1] == '\n' &&
                data[i+2] == '\r' && data[i+3] == '\n') {
                return i + 4;  /* Return offset to frame data */
            }
        }
    }
    return 0;
}

/* Send WebSocket frame */
static int send_frame(websocket_t* ws, uint8_t opcode, const uint8_t* data, int len) {
    uint8_t frame[WS_MAX_MESSAGE + 14];  /* Max frame overhead */
    int pos = 0;

    /* First byte: FIN + opcode */
    frame[pos++] = 0x80 | (opcode & 0x0F);

    /* Second byte: MASK + length */
    /* Client MUST mask frames */
    if (len < 126) {
        frame[pos++] = 0x80 | len;
    } else if (len < 65536) {
        frame[pos++] = 0x80 | 126;
        frame[pos++] = (len >> 8) & 0xFF;
        frame[pos++] = len & 0xFF;
    } else {
        /* 8-byte length - not supported for simplicity */
        return -1;
    }

    /* Masking key */
    uint32_t mask = ws_rand();
    frame[pos++] = (mask >> 24) & 0xFF;
    frame[pos++] = (mask >> 16) & 0xFF;
    frame[pos++] = (mask >> 8) & 0xFF;
    frame[pos++] = mask & 0xFF;

    /* Masked payload */
    uint8_t* mask_bytes = &frame[pos - 4];
    for (int i = 0; i < len; i++) {
        frame[pos++] = data[i] ^ mask_bytes[i % 4];
    }

    return tcp_send(ws->tcp_conn, frame, pos);
}

/* Parse incoming frame */
static int parse_frame(websocket_t* ws, const uint8_t* data, int len) {
    if (len < 2) return 0;  /* Need more data */

    int pos = 0;
    uint8_t byte1 = data[pos++];
    uint8_t byte2 = data[pos++];

    int fin = (byte1 >> 7) & 1;
    int opcode = byte1 & 0x0F;
    int mask = (byte2 >> 7) & 1;
    int payload_len = byte2 & 0x7F;

    (void)fin;  /* We don't handle fragmentation */

    /* Extended length */
    if (payload_len == 126) {
        if (len < 4) return 0;
        payload_len = (data[pos] << 8) | data[pos + 1];
        pos += 2;
    } else if (payload_len == 127) {
        /* 8-byte length - just read lower 32 bits */
        if (len < 10) return 0;
        pos += 4;  /* Skip high 32 bits */
        payload_len = (data[pos] << 24) | (data[pos+1] << 16) |
                      (data[pos+2] << 8) | data[pos+3];
        pos += 4;
    }

    /* Masking key (server shouldn't mask, but handle it) */
    uint8_t mask_key[4] = {0};
    if (mask) {
        if (pos + 4 > len) return 0;
        for (int i = 0; i < 4; i++) mask_key[i] = data[pos++];
    }

    /* Check we have full payload */
    if (pos + payload_len > len) return 0;

    /* Copy and unmask payload */
    ws->rx_opcode = opcode;
    ws->rx_len = 0;

    for (int i = 0; i < payload_len && ws->rx_len < WS_MAX_MESSAGE - 1; i++) {
        uint8_t b = data[pos + i];
        if (mask) b ^= mask_key[i % 4];
        ws->rx_buffer[ws->rx_len++] = b;
    }
    ws->rx_buffer[ws->rx_len] = 0;

    /* Handle control frames */
    if (opcode == WS_OP_PING) {
        /* Send pong with same payload */
        send_frame(ws, WS_OP_PONG, ws->rx_buffer, ws->rx_len);
        ws->rx_ready = 0;
    } else if (opcode == WS_OP_CLOSE) {
        ws_log("WS: Close frame\r\n");
        /* Send close response */
        send_frame(ws, WS_OP_CLOSE, NULL, 0);
        ws->state = WS_STATE_CLOSED;
        ws->rx_ready = 0;
    } else if (opcode == WS_OP_PONG) {
        /* Ignore pong */
        ws->rx_ready = 0;
    } else {
        /* Text or binary message */
        ws->rx_ready = 1;
    }

    return pos + payload_len;  /* Bytes consumed */
}

int ws_poll(websocket_t* ws) {
    if (ws->state == WS_STATE_CLOSED) return WS_STATE_CLOSED;

    int tcp_state = tcp_get_state(ws->tcp_conn);

    if (ws->state == WS_STATE_CONNECTING) {
        if (tcp_state == TCP_ESTABLISHED && !ws->handshake_sent) {
            /* Send upgrade request */
            char request[512];
            int req_len = build_upgrade_request(ws, request, sizeof(request));
            tcp_send(ws->tcp_conn, request, req_len);
            ws->handshake_sent = 1;
            ws_log("WS: Upgrade sent\r\n");
        }

        if (tcp_state == TCP_CLOSED) {
            ws_log("WS: Connect failed\r\n");
            ws->state = WS_STATE_CLOSED;
            return WS_STATE_CLOSED;
        }

        /* Check for upgrade response */
        if (ws->handshake_sent && tcp_data_available(ws->tcp_conn)) {
            char buf[512];
            int len = tcp_recv(ws->tcp_conn, buf, sizeof(buf) - 1);
            buf[len] = 0;

            int frame_start = check_upgrade_response(buf, len);
            if (frame_start > 0) {
                ws_log("WS: Upgraded!\r\n");
                ws->state = WS_STATE_OPEN;
                ws->handshake_complete = 1;

                /* Process any remaining data as frame */
                if (frame_start < len) {
                    parse_frame(ws, (uint8_t*)(buf + frame_start), len - frame_start);
                }
            }
        }
    } else if (ws->state == WS_STATE_OPEN) {
        if (tcp_state == TCP_CLOSED) {
            ws_log("WS: Disconnected\r\n");
            ws->state = WS_STATE_CLOSED;
            return WS_STATE_CLOSED;
        }

        /* Receive frames */
        if (tcp_data_available(ws->tcp_conn)) {
            uint8_t buf[1024];
            int len = tcp_recv(ws->tcp_conn, buf, sizeof(buf));
            if (len > 0) {
                parse_frame(ws, buf, len);
            }
        }
    }

    return ws->state;
}

int ws_send_text(websocket_t* ws, const char* message) {
    if (ws->state != WS_STATE_OPEN) return -1;
    return send_frame(ws, WS_OP_TEXT, (const uint8_t*)message, str_len(message));
}

int ws_send_binary(websocket_t* ws, const uint8_t* data, int len) {
    if (ws->state != WS_STATE_OPEN) return -1;
    return send_frame(ws, WS_OP_BINARY, data, len);
}

int ws_send_ping(websocket_t* ws) {
    if (ws->state != WS_STATE_OPEN) return -1;
    return send_frame(ws, WS_OP_PING, NULL, 0);
}

int ws_message_ready(websocket_t* ws) {
    return ws->rx_ready;
}

int ws_get_message(websocket_t* ws, char* buffer, int max_len) {
    if (!ws->rx_ready) return 0;

    int to_copy = ws->rx_len;
    if (to_copy > max_len - 1) to_copy = max_len - 1;

    memcpy(buffer, ws->rx_buffer, to_copy);
    buffer[to_copy] = 0;

    ws->rx_ready = 0;
    ws->rx_len = 0;

    return to_copy;
}

int ws_get_opcode(websocket_t* ws) {
    return ws->rx_opcode;
}

void ws_close(websocket_t* ws) {
    if (ws->state == WS_STATE_OPEN) {
        send_frame(ws, WS_OP_CLOSE, NULL, 0);
        ws->state = WS_STATE_CLOSING;
    }
    tcp_close(ws->tcp_conn);
    ws->state = WS_STATE_CLOSED;
}

int ws_get_state(websocket_t* ws) {
    return ws->state;
}
