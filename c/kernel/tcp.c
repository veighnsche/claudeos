/*
 * TinyOS TCP Stack Implementation
 */

#include "tcp.h"
#include "net.h"
#include "virtio_net.h"
#include "memory.h"

/* IP protocol number for TCP */
#define IP_PROTO_TCP 6

/* Packet buffer */
static uint8_t tcp_tx_buf[2048];

/* Connection pool */
static tcp_conn_t connections[MAX_TCP_CONNS];

/* Local port counter */
static uint16_t next_local_port = 49152;

/* Tick counter for timeouts */
static uint32_t tcp_ticks = 0;


/* Forward declarations */
static void send_tcp_packet(tcp_conn_t* conn, uint8_t flags,
                           const void* data, int data_len);
static uint16_t tcp_checksum(struct ip_hdr* ip, struct tcp_hdr* tcp, int tcp_len);

void tcp_init(void) {
    for (int i = 0; i < MAX_TCP_CONNS; i++) {
        connections[i].state = TCP_CLOSED;
        connections[i].rx_len = 0;
        connections[i].rx_ready = 0;
    }
}

/* Find a free connection slot */
static int find_free_conn(void) {
    for (int i = 0; i < MAX_TCP_CONNS; i++) {
        if (connections[i].state == TCP_CLOSED) {
            return i;
        }
    }
    return -1;
}

/* Find connection by remote IP/port */
static int find_conn(const uint8_t* ip, uint16_t local_port, uint16_t remote_port) {
    for (int i = 0; i < MAX_TCP_CONNS; i++) {
        if (connections[i].state != TCP_CLOSED &&
            connections[i].local_port == local_port &&
            connections[i].remote_port == remote_port &&
            ip[0] == connections[i].remote_ip[0] &&
            ip[1] == connections[i].remote_ip[1] &&
            ip[2] == connections[i].remote_ip[2] &&
            ip[3] == connections[i].remote_ip[3]) {
            return i;
        }
    }
    return -1;
}

/* Simple pseudo-random for initial sequence number */
static uint32_t get_initial_seq(void) {
    static uint32_t seed = 0x12345678;
    seed = seed * 1103515245 + 12345;
    return seed;
}

int tcp_connect(const uint8_t* ip, uint16_t port) {
    net_config_t* nc = net_get_config();
    if (!nc->configured) {
        return -1;
    }

    int idx = find_free_conn();
    if (idx < 0) {
        return -1;
    }

    tcp_conn_t* conn = &connections[idx];
    memset(conn, 0, sizeof(tcp_conn_t));

    conn->remote_ip[0] = ip[0];
    conn->remote_ip[1] = ip[1];
    conn->remote_ip[2] = ip[2];
    conn->remote_ip[3] = ip[3];
    conn->remote_port = port;
    conn->local_port = next_local_port++;
    if (next_local_port > 65000) next_local_port = 49152;

    conn->seq_num = get_initial_seq();
    conn->ack_num = 0;
    conn->state = TCP_SYN_SENT;
    conn->timeout_tick = tcp_ticks + 500;  /* 0.5 second timeout - faster retry */
    conn->retries = 0;
    conn->rx_len = 0;
    conn->rx_ready = 0;


    /* Send SYN */
    send_tcp_packet(conn, TCP_SYN, NULL, 0);

    return idx;
}

/* Calculate TCP checksum with pseudo-header */
static uint16_t tcp_checksum(struct ip_hdr* ip, struct tcp_hdr* tcp, int tcp_len) {
    uint32_t sum = 0;

    /* Pseudo-header */
    sum += (ip->src_ip[0] << 8) | ip->src_ip[1];
    sum += (ip->src_ip[2] << 8) | ip->src_ip[3];
    sum += (ip->dest_ip[0] << 8) | ip->dest_ip[1];
    sum += (ip->dest_ip[2] << 8) | ip->dest_ip[3];
    sum += IP_PROTO_TCP;
    sum += tcp_len;

    /* TCP header + data - read as bytes to handle alignment */
    uint8_t* ptr = (uint8_t*)tcp;
    int remaining = tcp_len;
    while (remaining > 1) {
        sum += (ptr[0] << 8) | ptr[1];
        ptr += 2;
        remaining -= 2;
    }
    if (remaining == 1) {
        sum += ptr[0] << 8;
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return ~sum;
}

/* Send a TCP packet */
static void send_tcp_packet(tcp_conn_t* conn, uint8_t flags,
                           const void* data, int data_len) {
    net_status_t* ns = virtio_net_get_status();
    net_config_t* nc = net_get_config();
    if (!ns->available || !nc->configured) return;

    memset(tcp_tx_buf, 0, sizeof(tcp_tx_buf));

    struct eth_hdr* eth = (struct eth_hdr*)tcp_tx_buf;
    struct ip_hdr* ip = (struct ip_hdr*)(tcp_tx_buf + ETH_HLEN);
    struct tcp_hdr* tcp = (struct tcp_hdr*)(tcp_tx_buf + ETH_HLEN + 20);
    uint8_t* payload = tcp_tx_buf + ETH_HLEN + 20 + 20;

    /* Get gateway MAC for routing */
    uint8_t dest_mac[6];
    const uint8_t* route_ip = nc->gateway;  /* Route through gateway */

    if (!net_arp_lookup(route_ip, dest_mac)) {
        /* Don't have MAC yet - send ARP and retry later */
        net_send_arp_request(route_ip);
        return;
    }
    /* Ethernet */
    memcpy(eth->dest, dest_mac, 6);
    memcpy(eth->src, ns->mac, 6);
    eth->ethertype = htons(ETH_P_IP);

    /* IP header */
    int total_len = 20 + 20 + data_len;  /* IP + TCP + data */
    ip->version_ihl = 0x45;
    ip->tos = 0;
    ip->total_len = htons(total_len);
    ip->id = htons(1000 + (conn - connections));
    ip->frag_off = 0;
    ip->ttl = 64;
    ip->protocol = IP_PROTO_TCP;
    ip->checksum = 0;
    memcpy(ip->src_ip, nc->ip, 4);
    memcpy(ip->dest_ip, conn->remote_ip, 4);

    /* IP checksum - read bytes to avoid alignment issues */
    uint8_t* ip_bytes = (uint8_t*)ip;
    uint32_t ip_sum = 0;
    for (int i = 0; i < 20; i += 2) {
        ip_sum += (ip_bytes[i] << 8) | ip_bytes[i+1];
    }
    while (ip_sum >> 16) ip_sum = (ip_sum & 0xFFFF) + (ip_sum >> 16);
    uint16_t ip_csum = ~ip_sum;
    ip_bytes[10] = (ip_csum >> 8) & 0xFF;
    ip_bytes[11] = ip_csum & 0xFF;

    /* TCP header - write byte by byte in network (big-endian) order */
    uint8_t* tcp_bytes = (uint8_t*)tcp;

    /* Source port - manual big endian (don't use htons then shift) */
    tcp_bytes[0] = (conn->local_port >> 8) & 0xFF;
    tcp_bytes[1] = conn->local_port & 0xFF;

    /* Dest port - manual big endian */
    tcp_bytes[2] = (conn->remote_port >> 8) & 0xFF;
    tcp_bytes[3] = conn->remote_port & 0xFF;

    /* Sequence number - manual big endian */
    tcp_bytes[4] = (conn->seq_num >> 24) & 0xFF;
    tcp_bytes[5] = (conn->seq_num >> 16) & 0xFF;
    tcp_bytes[6] = (conn->seq_num >> 8) & 0xFF;
    tcp_bytes[7] = conn->seq_num & 0xFF;

    /* Ack number - manual big endian */
    tcp_bytes[8] = (conn->ack_num >> 24) & 0xFF;
    tcp_bytes[9] = (conn->ack_num >> 16) & 0xFF;
    tcp_bytes[10] = (conn->ack_num >> 8) & 0xFF;
    tcp_bytes[11] = conn->ack_num & 0xFF;

    tcp_bytes[12] = 0x50;  /* data_off: 5 * 4 = 20 bytes header */
    tcp_bytes[13] = flags;

    /* Window size - manual big endian */
    tcp_bytes[14] = (TCP_RX_BUF_SIZE >> 8) & 0xFF;
    tcp_bytes[15] = TCP_RX_BUF_SIZE & 0xFF;

    tcp_bytes[16] = 0;  /* checksum placeholder */
    tcp_bytes[17] = 0;
    tcp_bytes[18] = 0;  /* urgent */
    tcp_bytes[19] = 0;

    /* Copy payload */
    if (data && data_len > 0) {
        memcpy(payload, data, data_len);
    }

    /* TCP checksum - calculate and write byte by byte */
    uint16_t tcp_csum = tcp_checksum(ip, (struct tcp_hdr*)tcp_bytes, 20 + data_len);
    tcp_bytes[16] = (tcp_csum >> 8) & 0xFF;
    tcp_bytes[17] = tcp_csum & 0xFF;

    /* Send packet */
    virtio_net_send(tcp_tx_buf, ETH_HLEN + total_len);

    /* Update sequence number for data sent */
    if (flags & TCP_SYN) conn->seq_num++;
    if (flags & TCP_FIN) conn->seq_num++;
    if (data_len > 0) conn->seq_num += data_len;
}

int tcp_send(int idx, const void* data, int len) {
    if (idx < 0 || idx >= MAX_TCP_CONNS) return -1;

    tcp_conn_t* conn = &connections[idx];
    if (conn->state != TCP_ESTABLISHED) {
        return -1;
    }

    /* Send in chunks if needed */
    int sent = 0;
    const uint8_t* ptr = (const uint8_t*)data;

    while (sent < len) {
        int chunk = len - sent;
        if (chunk > 1400) chunk = 1400;  /* MSS */

        send_tcp_packet(conn, TCP_ACK | TCP_PSH, ptr + sent, chunk);
        sent += chunk;
    }

    return sent;
}

int tcp_recv(int idx, void* buffer, int max_len) {
    if (idx < 0 || idx >= MAX_TCP_CONNS) return -1;

    tcp_conn_t* conn = &connections[idx];
    if (conn->rx_len == 0) return 0;

    int to_copy = conn->rx_len;
    if (to_copy > max_len) to_copy = max_len;

    memcpy(buffer, conn->rx_buffer, to_copy);

    /* Shift remaining data */
    if (to_copy < conn->rx_len) {
        memmove(conn->rx_buffer, conn->rx_buffer + to_copy, conn->rx_len - to_copy);
    }
    conn->rx_len -= to_copy;
    conn->rx_ready = (conn->rx_len > 0);

    return to_copy;
}

int tcp_data_available(int idx) {
    if (idx < 0 || idx >= MAX_TCP_CONNS) return 0;
    return connections[idx].rx_ready;
}

void tcp_close(int idx) {
    if (idx < 0 || idx >= MAX_TCP_CONNS) return;

    tcp_conn_t* conn = &connections[idx];
    if (conn->state == TCP_ESTABLISHED) {
        send_tcp_packet(conn, TCP_FIN | TCP_ACK, NULL, 0);
        conn->state = TCP_FIN_WAIT_1;
        conn->timeout_tick = tcp_ticks + 5000;
    } else {
        conn->state = TCP_CLOSED;
    }
}

int tcp_get_state(int idx) {
    if (idx < 0 || idx >= MAX_TCP_CONNS) return TCP_CLOSED;
    return connections[idx].state;
}

void tcp_poll(void) {
    tcp_ticks++;

    /* Check for timeouts */
    for (int i = 0; i < MAX_TCP_CONNS; i++) {
        tcp_conn_t* conn = &connections[i];
        if (conn->state == TCP_CLOSED) continue;

        if (tcp_ticks > conn->timeout_tick) {
            if (conn->state == TCP_SYN_SENT) {
                /* Retry SYN */
                conn->retries++;
                if (conn->retries > 5) {
                    conn->state = TCP_CLOSED;
                } else {
                    /* Reset seq_num for retry - SYN should use same seq */
                    conn->seq_num--;
                    send_tcp_packet(conn, TCP_SYN, NULL, 0);
                    conn->timeout_tick = tcp_ticks + 500;
                }
            } else if (conn->state == TCP_FIN_WAIT_1 ||
                       conn->state == TCP_FIN_WAIT_2 ||
                       conn->state == TCP_TIME_WAIT) {
                conn->state = TCP_CLOSED;
            }
        }
    }
}

void tcp_handle_packet(struct eth_hdr* eth, struct ip_hdr* ip,
                       struct tcp_hdr* tcp, int len) {
    (void)eth;

    /* Read TCP header byte by byte to avoid alignment issues */
    uint8_t* tcp_bytes = (uint8_t*)tcp;
    uint16_t src_port = (tcp_bytes[0] << 8) | tcp_bytes[1];
    uint16_t dest_port = (tcp_bytes[2] << 8) | tcp_bytes[3];
    uint32_t seq = ((uint32_t)tcp_bytes[4] << 24) | ((uint32_t)tcp_bytes[5] << 16) |
                   ((uint32_t)tcp_bytes[6] << 8) | tcp_bytes[7];
    uint32_t ack = ((uint32_t)tcp_bytes[8] << 24) | ((uint32_t)tcp_bytes[9] << 16) |
                   ((uint32_t)tcp_bytes[10] << 8) | tcp_bytes[11];
    uint8_t flags = tcp_bytes[13];

    /* Find matching connection */
    int idx = find_conn(ip->src_ip, dest_port, src_port);
    if (idx < 0) {
        /* No connection - ignore (could send RST in full impl) */
        return;
    }

    tcp_conn_t* conn = &connections[idx];

    /* Calculate header and data length */
    int tcp_hdr_len = (tcp->data_off >> 4) * 4;
    int data_len = len - tcp_hdr_len;
    uint8_t* data = (uint8_t*)tcp + tcp_hdr_len;

    /* Handle RST */
    if (flags & TCP_RST) {
        conn->state = TCP_CLOSED;
        return;
    }

    /* State machine */
    switch (conn->state) {
        case TCP_SYN_SENT:
            if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
                /* Got SYN-ACK, send ACK */
                conn->ack_num = seq + 1;
                if (ack == conn->seq_num) {
                    conn->state = TCP_ESTABLISHED;
                    /* Send ACK */
                    send_tcp_packet(conn, TCP_ACK, NULL, 0);
                    conn->last_ack_sent = conn->ack_num;
                }
            }
            break;

        case TCP_ESTABLISHED:
            /* Update ack number for received data */
            if (data_len > 0) {
                /* Copy data to receive buffer */
                int space = TCP_RX_BUF_SIZE - conn->rx_len;
                int to_copy = data_len;
                if (to_copy > space) to_copy = space;

                if (to_copy > 0) {
                    memcpy(conn->rx_buffer + conn->rx_len, data, to_copy);
                    conn->rx_len += to_copy;
                    conn->rx_ready = 1;
                }

                conn->ack_num = seq + data_len;
                /* Send ACK */
                send_tcp_packet(conn, TCP_ACK, NULL, 0);
                conn->last_ack_sent = conn->ack_num;
            }

            /* Handle FIN */
            if (flags & TCP_FIN) {
                conn->ack_num = seq + 1;
                send_tcp_packet(conn, TCP_ACK, NULL, 0);
                conn->state = TCP_CLOSE_WAIT;
                /* Send our FIN */
                send_tcp_packet(conn, TCP_FIN | TCP_ACK, NULL, 0);
                conn->state = TCP_LAST_ACK;
            }
            break;

        case TCP_FIN_WAIT_1:
            if (flags & TCP_ACK) {
                conn->state = TCP_FIN_WAIT_2;
            }
            if (flags & TCP_FIN) {
                conn->ack_num = seq + 1;
                send_tcp_packet(conn, TCP_ACK, NULL, 0);
                conn->state = TCP_TIME_WAIT;
                conn->timeout_tick = tcp_ticks + 2000;
            }
            break;

        case TCP_FIN_WAIT_2:
            if (flags & TCP_FIN) {
                conn->ack_num = seq + 1;
                send_tcp_packet(conn, TCP_ACK, NULL, 0);
                conn->state = TCP_TIME_WAIT;
                conn->timeout_tick = tcp_ticks + 2000;
            }
            break;

        case TCP_LAST_ACK:
            if (flags & TCP_ACK) {
                conn->state = TCP_CLOSED;
            }
            break;

        default:
            break;
    }
}
