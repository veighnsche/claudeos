/*
 * TinyOS TCP Stack
 */
#ifndef TCP_H
#define TCP_H

#include "types.h"
#include "net.h"

/* TCP header */
struct tcp_hdr {
    uint16_t src_port;
    uint16_t dest_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t data_off;     /* Upper 4 bits = header length in 32-bit words */
    uint8_t flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} __attribute__((packed));

/* TCP flags */
#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10
#define TCP_URG  0x20

/* TCP connection states */
#define TCP_CLOSED       0
#define TCP_SYN_SENT     1
#define TCP_ESTABLISHED  2
#define TCP_FIN_WAIT_1   3
#define TCP_FIN_WAIT_2   4
#define TCP_CLOSE_WAIT   5
#define TCP_LAST_ACK     6
#define TCP_TIME_WAIT    7

/* Maximum TCP connections */
#define MAX_TCP_CONNS    4

/* TCP receive buffer size */
#define TCP_RX_BUF_SIZE  4096

/* TCP connection */
typedef struct {
    int state;
    uint8_t remote_ip[4];
    uint16_t local_port;
    uint16_t remote_port;
    uint32_t seq_num;        /* Our sequence number */
    uint32_t ack_num;        /* What we expect from peer */
    uint32_t last_ack_sent;  /* Last ACK we sent */
    uint8_t rx_buffer[TCP_RX_BUF_SIZE];
    int rx_len;              /* Data in receive buffer */
    int rx_ready;            /* New data available */
    uint32_t timeout_tick;   /* For retransmission */
    int retries;
} tcp_conn_t;

/* Initialize TCP */
void tcp_init(void);

/* Create a new connection (returns connection index or -1) */
int tcp_connect(const uint8_t* ip, uint16_t port);

/* Send data on connection */
int tcp_send(int conn, const void* data, int len);

/* Receive data from connection (returns bytes read) */
int tcp_recv(int conn, void* buffer, int max_len);

/* Check if connection has data available */
int tcp_data_available(int conn);

/* Close connection */
void tcp_close(int conn);

/* Get connection state */
int tcp_get_state(int conn);

/* Poll for TCP events (called from net_poll) */
void tcp_poll(void);

/* Handle incoming TCP packet */
void tcp_handle_packet(struct eth_hdr* eth, struct ip_hdr* ip,
                       struct tcp_hdr* tcp, int len);

#endif
