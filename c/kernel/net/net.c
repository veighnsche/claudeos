/*
 * TinyOS Network Stack
 * Implements Ethernet, ARP, IPv4, ICMP, UDP, DHCP
 */

#include "net.h"
#include "virtio_net.h"
#include "memory.h"
#include "tcp.h"

/* Packet buffers */
static uint8_t rx_buf[2048];
static uint8_t tx_buf[2048];

/* Network configuration */
static net_config_t config = {
    .ip = {0, 0, 0, 0},
    .subnet = {255, 255, 255, 0},
    .gateway = {0, 0, 0, 0},
    .dns = {0, 0, 0, 0},
    .configured = 0,
    .dhcp_state = 0
};

/* ARP cache */
#define ARP_CACHE_SIZE 8
static struct {
    uint8_t ip[4];
    uint8_t mac[6];
    int valid;
} arp_cache[ARP_CACHE_SIZE];

/* Ping tracking */
static ping_status_t ping_status = {0};
static uint16_t ping_seq = 0;
static uint32_t ping_sent_time = 0;

/* Simple tick counter (incremented by net_poll) */
static uint32_t tick_counter = 0;

/* DHCP state */
#define DHCP_IDLE       0
#define DHCP_DISCOVERING 1
#define DHCP_REQUESTING  2
#define DHCP_CONFIGURED  3

static uint32_t dhcp_xid = 0x12345678;

/* Broadcast addresses */
static const uint8_t broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const uint8_t broadcast_ip[4] = {255, 255, 255, 255};

/* Forward declarations */
static void send_dhcp_request(const uint8_t* server_ip);
static void send_dhcp_discover(void);
static void handle_dns_response(uint8_t* data, int len);

/* IP checksum calculation */
static uint16_t ip_checksum(const void* data, int len) {
    const uint16_t* ptr = (const uint16_t*)data;
    uint32_t sum = 0;

    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }

    if (len == 1) {
        sum += *(const uint8_t*)ptr;
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return ~sum;
}

/* Compare IP addresses */
static int ip_match(const uint8_t* a, const uint8_t* b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}


/* Copy addresses */
static void ip_copy(uint8_t* dst, const uint8_t* src) {
    dst[0] = src[0]; dst[1] = src[1];
    dst[2] = src[2]; dst[3] = src[3];
}

static void mac_copy(uint8_t* dst, const uint8_t* src) {
    for (int i = 0; i < 6; i++) dst[i] = src[i];
}

/* ARP cache lookup - exported for TCP */
int net_arp_lookup(const uint8_t* ip, uint8_t* mac_out) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && ip_match(arp_cache[i].ip, ip)) {
            mac_copy(mac_out, arp_cache[i].mac);
            return 1;
        }
    }
    return 0;
}

/* ARP cache add */
static void arp_add(const uint8_t* ip, const uint8_t* mac) {
    /* Check if already exists */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && ip_match(arp_cache[i].ip, ip)) {
            mac_copy(arp_cache[i].mac, mac);
            return;
        }
    }

    /* Find empty slot */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_cache[i].valid) {
            ip_copy(arp_cache[i].ip, ip);
            mac_copy(arp_cache[i].mac, mac);
            arp_cache[i].valid = 1;
            return;
        }
    }

    /* Replace first entry if full */
    ip_copy(arp_cache[0].ip, ip);
    mac_copy(arp_cache[0].mac, mac);
}

/* Send ARP request - exported for TCP */
void net_send_arp_request(const uint8_t* target_ip) {
    net_status_t* ns = virtio_net_get_status();
    if (!ns->available) return;

    struct eth_hdr* eth = (struct eth_hdr*)tx_buf;
    struct arp_hdr* arp = (struct arp_hdr*)(tx_buf + ETH_HLEN);

    /* Ethernet header */
    mac_copy(eth->dest, broadcast_mac);
    mac_copy(eth->src, ns->mac);
    eth->ethertype = htons(ETH_P_ARP);

    /* ARP header */
    arp->hw_type = htons(1);        /* Ethernet */
    arp->proto_type = htons(0x0800); /* IPv4 */
    arp->hw_len = 6;
    arp->proto_len = 4;
    arp->opcode = htons(ARP_REQUEST);
    mac_copy(arp->sender_mac, ns->mac);
    ip_copy(arp->sender_ip, config.ip);
    memset(arp->target_mac, 0, 6);
    ip_copy(arp->target_ip, target_ip);

    virtio_net_send(tx_buf, ETH_HLEN + sizeof(struct arp_hdr));
}

/* Send ARP reply */
static void send_arp_reply(const uint8_t* target_mac, const uint8_t* target_ip) {
    net_status_t* ns = virtio_net_get_status();
    if (!ns->available) return;

    struct eth_hdr* eth = (struct eth_hdr*)tx_buf;
    struct arp_hdr* arp = (struct arp_hdr*)(tx_buf + ETH_HLEN);

    /* Ethernet header */
    mac_copy(eth->dest, target_mac);
    mac_copy(eth->src, ns->mac);
    eth->ethertype = htons(ETH_P_ARP);

    /* ARP header */
    arp->hw_type = htons(1);
    arp->proto_type = htons(0x0800);
    arp->hw_len = 6;
    arp->proto_len = 4;
    arp->opcode = htons(ARP_REPLY);
    mac_copy(arp->sender_mac, ns->mac);
    ip_copy(arp->sender_ip, config.ip);
    mac_copy(arp->target_mac, target_mac);
    ip_copy(arp->target_ip, target_ip);

    virtio_net_send(tx_buf, ETH_HLEN + sizeof(struct arp_hdr));
}

/* Handle ARP packet */
static void handle_arp(struct arp_hdr* arp) {
    uint16_t opcode = ntohs(arp->opcode);

    /* Always learn from ARP packets */
    arp_add(arp->sender_ip, arp->sender_mac);

    if (opcode == ARP_REQUEST) {
        /* Is it asking for our IP? */
        if (config.configured && ip_match(arp->target_ip, config.ip)) {
            send_arp_reply(arp->sender_mac, arp->sender_ip);
        }
    }
}

/* Send ICMP echo reply */
static void send_icmp_reply(const uint8_t* dest_mac, const uint8_t* dest_ip,
                            uint16_t id, uint16_t seq, const uint8_t* data, int data_len) {
    net_status_t* ns = virtio_net_get_status();
    if (!ns->available) return;

    struct eth_hdr* eth = (struct eth_hdr*)tx_buf;
    struct ip_hdr* ip = (struct ip_hdr*)(tx_buf + ETH_HLEN);
    struct icmp_hdr* icmp = (struct icmp_hdr*)(tx_buf + ETH_HLEN + 20);
    uint8_t* payload = tx_buf + ETH_HLEN + 20 + 8;

    /* Ethernet */
    mac_copy(eth->dest, dest_mac);
    mac_copy(eth->src, ns->mac);
    eth->ethertype = htons(ETH_P_IP);

    /* IP */
    ip->version_ihl = 0x45;
    ip->tos = 0;
    ip->total_len = htons(20 + 8 + data_len);
    ip->id = htons(1234);
    ip->frag_off = 0;
    ip->ttl = 64;
    ip->protocol = IP_PROTO_ICMP;
    ip->checksum = 0;
    ip_copy(ip->src_ip, config.ip);
    ip_copy(ip->dest_ip, dest_ip);
    ip->checksum = ip_checksum(ip, 20);

    /* ICMP */
    icmp->type = ICMP_ECHO_REPLY;
    icmp->code = 0;
    icmp->checksum = 0;
    icmp->id = id;
    icmp->seq = seq;

    /* Copy payload */
    memcpy(payload, data, data_len);

    /* ICMP checksum */
    icmp->checksum = ip_checksum(icmp, 8 + data_len);

    virtio_net_send(tx_buf, ETH_HLEN + 20 + 8 + data_len);
}

/* Send ICMP echo request (ping) */
static void send_icmp_request(const uint8_t* dest_ip) {
    net_status_t* ns = virtio_net_get_status();
    if (!ns->available || !config.configured) return;

    /* Need MAC address - use gateway for non-local IPs */
    uint8_t dest_mac[6];
    const uint8_t* target_ip = dest_ip;

    /* For simplicity, always go through gateway if configured */
    if (config.gateway[0] != 0) {
        target_ip = config.gateway;
    }

    if (!net_arp_lookup(target_ip, dest_mac)) {
        net_send_arp_request(target_ip);
        return;
    }

    struct eth_hdr* eth = (struct eth_hdr*)tx_buf;
    struct ip_hdr* ip = (struct ip_hdr*)(tx_buf + ETH_HLEN);
    struct icmp_hdr* icmp = (struct icmp_hdr*)(tx_buf + ETH_HLEN + 20);

    /* Ethernet */
    mac_copy(eth->dest, dest_mac);
    mac_copy(eth->src, ns->mac);
    eth->ethertype = htons(ETH_P_IP);

    /* IP */
    ip->version_ihl = 0x45;
    ip->tos = 0;
    ip->total_len = htons(20 + 8 + 8);  /* 8 bytes of ping data */
    ip->id = htons(1234);
    ip->frag_off = 0;
    ip->ttl = 64;
    ip->protocol = IP_PROTO_ICMP;
    ip->checksum = 0;
    ip_copy(ip->src_ip, config.ip);
    ip_copy(ip->dest_ip, dest_ip);
    ip->checksum = ip_checksum(ip, 20);

    /* ICMP */
    icmp->type = ICMP_ECHO_REQUEST;
    icmp->code = 0;
    icmp->checksum = 0;
    icmp->id = htons(0x1234);
    icmp->seq = htons(ping_seq++);

    /* Simple payload */
    uint8_t* payload = tx_buf + ETH_HLEN + 20 + 8;
    memset(payload, 'T', 8);

    icmp->checksum = ip_checksum(icmp, 8 + 8);

    virtio_net_send(tx_buf, ETH_HLEN + 20 + 8 + 8);

    ping_status.sent++;
    ping_sent_time = tick_counter;
}

/* Handle ICMP packet */
static void handle_icmp(struct eth_hdr* eth, struct ip_hdr* ip, struct icmp_hdr* icmp, int len) {
    (void)len;  /* Suppress unused warning */
    if (icmp->type == ICMP_ECHO_REQUEST) {
        /* Respond to ping */
        int data_len = ntohs(ip->total_len) - 20 - 8;
        if (data_len < 0) data_len = 0;
        if (data_len > 1400) data_len = 1400;

        send_icmp_reply(eth->src, ip->src_ip, icmp->id, icmp->seq,
                        (uint8_t*)icmp + 8, data_len);
    } else if (icmp->type == ICMP_ECHO_REPLY) {
        /* Got ping response */
        ping_status.received++;
        ping_status.last_rtt_ms = (tick_counter - ping_sent_time) / 10;  /* Rough estimate */
    }
}

/* Handle UDP packet */
static void handle_udp(struct eth_hdr* eth, struct ip_hdr* ip, struct udp_hdr* udp, int len) {
    (void)eth; (void)ip;
    uint16_t dest_port = ntohs(udp->dest_port);
    uint16_t src_port = ntohs(udp->src_port);

    /* DNS response? */
    if (src_port == 53) {
        uint8_t* dns_data = (uint8_t*)udp + 8;
        int dns_len = len - 8;
        handle_dns_response(dns_data, dns_len);
        return;
    }

    /* DHCP response? */
    if (dest_port == DHCP_CLIENT_PORT && src_port == DHCP_SERVER_PORT) {
        struct dhcp_msg* dhcp = (struct dhcp_msg*)((uint8_t*)udp + 8);

        /* Read xid byte by byte to avoid alignment issues */
        uint8_t* xid_bytes = (uint8_t*)&dhcp->xid;
        uint32_t xid = ((uint32_t)xid_bytes[0] << 24) | ((uint32_t)xid_bytes[1] << 16) |
                       ((uint32_t)xid_bytes[2] << 8) | xid_bytes[3];

        if (xid != dhcp_xid) return;

        /* Parse DHCP options with safety limit */
        uint8_t* opts = dhcp->options;
        uint8_t* opts_end = (uint8_t*)udp + len;
        int msg_type = 0;
        uint8_t server_ip[4] = {0};
        int max_opts = 50;

        /* Skip magic cookie */
        if (opts + 4 <= opts_end &&
            opts[0] == 99 && opts[1] == 130 && opts[2] == 83 && opts[3] == 99) {
            opts += 4;
        }

        while (opts < opts_end && *opts != 255 && max_opts-- > 0) {
            uint8_t opt = *opts++;
            if (opt == 0) continue;
            if (opts >= opts_end) break;

            uint8_t opt_len = *opts++;
            if (opts + opt_len > opts_end) break;

            switch (opt) {
                case 53: if (opt_len >= 1) msg_type = opts[0]; break;
                case 1:  if (opt_len == 4) ip_copy(config.subnet, opts); break;
                case 3:  if (opt_len >= 4) ip_copy(config.gateway, opts); break;
                case 6:  if (opt_len >= 4) ip_copy(config.dns, opts); break;
                case 54: if (opt_len == 4) ip_copy(server_ip, opts); break;
            }
            opts += opt_len;
        }

        if (msg_type == DHCP_OFFER && config.dhcp_state == DHCP_DISCOVERING) {
            ip_copy(config.ip, dhcp->yiaddr);
            config.dhcp_state = DHCP_REQUESTING;
            send_dhcp_request(server_ip);
        } else if (msg_type == DHCP_ACK && config.dhcp_state == DHCP_REQUESTING) {
            ip_copy(config.ip, dhcp->yiaddr);
            config.configured = 1;
            config.dhcp_state = DHCP_CONFIGURED;
            /* Log IP address to UART */
            #define LOG_UART ((volatile uint32_t*)0x09000000)
            const char* m = "DHCP: Got IP ";
            while (*m) *LOG_UART = *m++;
            char ipbuf[16];
            net_ip_to_str(config.ip, ipbuf);
            char* p = ipbuf;
            while (*p) *LOG_UART = *p++;
            *LOG_UART = '\r'; *LOG_UART = '\n';
        }
    }
}

/* Handle IP packet */
static void handle_ip(struct eth_hdr* eth, struct ip_hdr* ip, int len) {
    (void)len;
    if ((ip->version_ihl >> 4) != 4) return;

    int ip_hdr_len = (ip->version_ihl & 0x0F) * 4;
    if (ip_hdr_len < 20) return;

    /* Check if it's for us (or broadcast) */
    if (!ip_match(ip->dest_ip, config.ip) &&
        !ip_match(ip->dest_ip, broadcast_ip)) {
        return;
    }

    uint8_t* payload = (uint8_t*)ip + ip_hdr_len;
    int payload_len = ntohs(ip->total_len) - ip_hdr_len;

    switch (ip->protocol) {
        case IP_PROTO_ICMP:
            if (payload_len >= 8) {
                handle_icmp(eth, ip, (struct icmp_hdr*)payload, payload_len);
            }
            break;
        case IP_PROTO_TCP:
            if (payload_len >= 20) {
                tcp_handle_packet(eth, ip, (struct tcp_hdr*)payload, payload_len);
            }
            break;
        case IP_PROTO_UDP:
            if (payload_len >= 8) {
                handle_udp(eth, ip, (struct udp_hdr*)payload, payload_len);
            }
            break;
    }
}

/* Send DHCP Discover */
static void send_dhcp_discover(void) {
    net_status_t* ns = virtio_net_get_status();
    if (!ns->available) return;

    struct eth_hdr* eth = (struct eth_hdr*)tx_buf;
    struct ip_hdr* ip = (struct ip_hdr*)(tx_buf + ETH_HLEN);
    struct udp_hdr* udp = (struct udp_hdr*)(tx_buf + ETH_HLEN + 20);
    struct dhcp_msg* dhcp = (struct dhcp_msg*)(tx_buf + ETH_HLEN + 20 + 8);

    memset(tx_buf, 0, sizeof(tx_buf));

    /* Ethernet - broadcast */
    mac_copy(eth->dest, broadcast_mac);
    mac_copy(eth->src, ns->mac);
    eth->ethertype = htons(ETH_P_IP);

    /* IP */
    ip->version_ihl = 0x45;
    ip->tos = 0;
    ip->total_len = htons(20 + 8 + sizeof(struct dhcp_msg));
    ip->id = htons(1);
    ip->frag_off = 0;
    ip->ttl = 64;
    ip->protocol = IP_PROTO_UDP;
    ip->checksum = 0;
    ip->src_ip[0] = 0; ip->src_ip[1] = 0; ip->src_ip[2] = 0; ip->src_ip[3] = 0;
    ip->dest_ip[0] = 255; ip->dest_ip[1] = 255; ip->dest_ip[2] = 255; ip->dest_ip[3] = 255;
    ip->checksum = ip_checksum(ip, 20);

    /* UDP */
    udp->src_port = htons(DHCP_CLIENT_PORT);
    udp->dest_port = htons(DHCP_SERVER_PORT);
    udp->length = htons(8 + sizeof(struct dhcp_msg));
    udp->checksum = 0;

    /* DHCP - write byte by byte to avoid alignment issues */
    dhcp->op = DHCP_BOOTREQUEST;
    dhcp->htype = 1;
    dhcp->hlen = 6;
    dhcp->hops = 0;
    /* Write xid byte by byte - no conversion, just copy bytes */
    uint8_t* xid_ptr = (uint8_t*)&dhcp->xid;
    xid_ptr[0] = (dhcp_xid >> 24) & 0xFF;
    xid_ptr[1] = (dhcp_xid >> 16) & 0xFF;
    xid_ptr[2] = (dhcp_xid >> 8) & 0xFF;
    xid_ptr[3] = dhcp_xid & 0xFF;
    dhcp->secs = 0;
    dhcp->flags = htons(0x8000);
    mac_copy(dhcp->chaddr, ns->mac);

    /* DHCP Options */
    uint8_t* opts = dhcp->options;
    *opts++ = 99; *opts++ = 130; *opts++ = 83; *opts++ = 99;  /* Magic cookie */
    *opts++ = 53; *opts++ = 1; *opts++ = DHCP_DISCOVER;       /* Message type */
    *opts++ = 55; *opts++ = 3; *opts++ = 1; *opts++ = 3; *opts++ = 6;  /* Parameter request */
    *opts++ = 255;  /* End */

    virtio_net_send(tx_buf, ETH_HLEN + 20 + 8 + sizeof(struct dhcp_msg));
    config.dhcp_state = DHCP_DISCOVERING;
}

/* Send DHCP Request */
static void send_dhcp_request(const uint8_t* server_ip) {
    net_status_t* ns = virtio_net_get_status();
    if (!ns->available) return;

    struct eth_hdr* eth = (struct eth_hdr*)tx_buf;
    struct ip_hdr* ip = (struct ip_hdr*)(tx_buf + ETH_HLEN);
    struct udp_hdr* udp = (struct udp_hdr*)(tx_buf + ETH_HLEN + 20);
    struct dhcp_msg* dhcp = (struct dhcp_msg*)(tx_buf + ETH_HLEN + 20 + 8);

    memset(tx_buf, 0, sizeof(tx_buf));

    /* Ethernet - broadcast */
    mac_copy(eth->dest, broadcast_mac);
    mac_copy(eth->src, ns->mac);
    eth->ethertype = htons(ETH_P_IP);

    /* IP */
    ip->version_ihl = 0x45;
    ip->tos = 0;
    ip->total_len = htons(20 + 8 + sizeof(struct dhcp_msg));
    ip->id = htons(2);
    ip->frag_off = 0;
    ip->ttl = 64;
    ip->protocol = IP_PROTO_UDP;
    ip->checksum = 0;
    memset(ip->src_ip, 0, 4);
    memset(ip->dest_ip, 255, 4);
    ip->checksum = ip_checksum(ip, 20);

    /* UDP */
    udp->src_port = htons(DHCP_CLIENT_PORT);
    udp->dest_port = htons(DHCP_SERVER_PORT);
    udp->length = htons(8 + sizeof(struct dhcp_msg));
    udp->checksum = 0;

    /* DHCP */
    dhcp->op = DHCP_BOOTREQUEST;
    dhcp->htype = 1;
    dhcp->hlen = 6;
    dhcp->hops = 0;
    /* Write xid byte by byte - same as discover */
    uint8_t* xid_ptr = (uint8_t*)&dhcp->xid;
    xid_ptr[0] = (dhcp_xid >> 24) & 0xFF;
    xid_ptr[1] = (dhcp_xid >> 16) & 0xFF;
    xid_ptr[2] = (dhcp_xid >> 8) & 0xFF;
    xid_ptr[3] = dhcp_xid & 0xFF;
    dhcp->secs = 0;
    dhcp->flags = htons(0x8000);
    mac_copy(dhcp->chaddr, ns->mac);

    /* DHCP Options */
    uint8_t* opts = dhcp->options;
    *opts++ = 99; *opts++ = 130; *opts++ = 83; *opts++ = 99;  /* Magic cookie */
    *opts++ = 53; *opts++ = 1; *opts++ = DHCP_REQUEST;        /* Message type */
    *opts++ = 50; *opts++ = 4;                                 /* Requested IP */
    ip_copy(opts, config.ip); opts += 4;
    *opts++ = 54; *opts++ = 4;                                 /* Server ID */
    ip_copy(opts, server_ip); opts += 4;
    *opts++ = 255;  /* End */

    virtio_net_send(tx_buf, ETH_HLEN + 20 + 8 + sizeof(struct dhcp_msg));
}

/* Process received packet */
static void process_packet(uint8_t* pkt, int len) {
    if (len < ETH_HLEN) return;

    struct eth_hdr* eth = (struct eth_hdr*)pkt;
    uint16_t ethertype = ntohs(eth->ethertype);

    switch (ethertype) {
        case ETH_P_ARP:
            if ((uint32_t)len >= ETH_HLEN + sizeof(struct arp_hdr)) {
                handle_arp((struct arp_hdr*)(pkt + ETH_HLEN));
            }
            break;
        case ETH_P_IP:
            if (len >= ETH_HLEN + 20) {
                handle_ip(eth, (struct ip_hdr*)(pkt + ETH_HLEN), len - ETH_HLEN);
            }
            break;
    }
}

void net_init(void) {
    /* Clear state */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        arp_cache[i].valid = 0;
    }
    ping_status.sent = 0;
    ping_status.received = 0;

    /* Initialize driver */
    virtio_net_init();

    /* Initialize TCP stack */
    tcp_init();
}

void net_poll(void) {
    tick_counter++;

    if (!virtio_net_available()) return;

    virtio_net_poll();

    int len = virtio_net_recv(rx_buf, sizeof(rx_buf));
    if (len > 0) {
        process_packet(rx_buf, len);
    }

    /* Poll TCP for timeouts/retransmissions */
    tcp_poll();

    /* Start DHCP if not configured - retry every 30000 ticks */
    if (!config.configured && config.dhcp_state != DHCP_CONFIGURED) {
        if (config.dhcp_state == DHCP_IDLE || (tick_counter % 30000) == 0) {
            send_dhcp_discover();
        }
    }
}

net_config_t* net_get_config(void) {
    return &config;
}

ping_status_t* net_get_ping_status(void) {
    return &ping_status;
}

void net_ping_gateway(void) {
    if (config.configured && config.gateway[0] != 0) {
        send_icmp_request(config.gateway);
    }
}

void net_ip_to_str(const uint8_t* ip, char* buf) {
    /* Simple decimal formatting */
    char* p = buf;
    for (int i = 0; i < 4; i++) {
        uint8_t val = ip[i];
        if (val >= 100) { *p++ = '0' + val / 100; val %= 100; }
        if (val >= 10 || ip[i] >= 100) { *p++ = '0' + val / 10; val %= 10; }
        *p++ = '0' + val;
        if (i < 3) *p++ = '.';
    }
    *p = '\0';
}

void net_mac_to_str(const uint8_t* mac, char* buf) {
    const char hex[] = "0123456789ABCDEF";
    char* p = buf;
    for (int i = 0; i < 6; i++) {
        *p++ = hex[mac[i] >> 4];
        *p++ = hex[mac[i] & 0x0F];
        if (i < 5) *p++ = ':';
    }
    *p = '\0';
}

/* ============ DNS Resolution ============ */

#define DNS_PORT 53

/* Active DNS query tracking */
static dns_query_t* active_dns_query = NULL;
static uint16_t dns_query_id_counter = 1;

/* Send generic UDP packet */
void net_send_udp(const uint8_t* dest_ip, uint16_t src_port, uint16_t dest_port,
                  const void* data, int len) {
    net_status_t* ns = virtio_net_get_status();
    if (!ns->available || !config.configured) return;

    memset(tx_buf, 0, sizeof(tx_buf));

    struct eth_hdr* eth = (struct eth_hdr*)tx_buf;
    struct ip_hdr* ip = (struct ip_hdr*)(tx_buf + ETH_HLEN);
    struct udp_hdr* udp = (struct udp_hdr*)(tx_buf + ETH_HLEN + 20);
    uint8_t* payload = tx_buf + ETH_HLEN + 20 + 8;

    /* Get gateway MAC */
    uint8_t dest_mac[6];
    if (!net_arp_lookup(config.gateway, dest_mac)) {
        net_send_arp_request(config.gateway);
        return;
    }

    /* Ethernet */
    mac_copy(eth->dest, dest_mac);
    mac_copy(eth->src, ns->mac);
    eth->ethertype = htons(ETH_P_IP);

    /* IP */
    int total_len = 20 + 8 + len;
    ip->version_ihl = 0x45;
    ip->tos = 0;
    ip->total_len = htons(total_len);
    ip->id = htons(dns_query_id_counter);
    ip->frag_off = 0;
    ip->ttl = 64;
    ip->protocol = IP_PROTO_UDP;
    ip->checksum = 0;
    memcpy(ip->src_ip, config.ip, 4);
    memcpy(ip->dest_ip, dest_ip, 4);
    ip->checksum = ip_checksum(ip, 20);

    /* UDP */
    udp->src_port = htons(src_port);
    udp->dest_port = htons(dest_port);
    udp->length = htons(8 + len);
    udp->checksum = 0;  /* UDP checksum optional */

    /* Payload */
    memcpy(payload, data, len);

    virtio_net_send(tx_buf, ETH_HLEN + total_len);
}

/* Build DNS query packet */
static int build_dns_query(uint8_t* buf, uint16_t id, const char* hostname) {
    uint8_t* p = buf;

    /* DNS Header */
    *p++ = (id >> 8) & 0xFF;  /* ID high */
    *p++ = id & 0xFF;          /* ID low */
    *p++ = 0x01;               /* Flags: RD (recursion desired) */
    *p++ = 0x00;
    *p++ = 0x00; *p++ = 0x01;  /* QDCOUNT = 1 */
    *p++ = 0x00; *p++ = 0x00;  /* ANCOUNT = 0 */
    *p++ = 0x00; *p++ = 0x00;  /* NSCOUNT = 0 */
    *p++ = 0x00; *p++ = 0x00;  /* ARCOUNT = 0 */

    /* Question: hostname in label format */
    const char* h = hostname;
    while (*h) {
        /* Find next dot or end */
        const char* start = h;
        int len = 0;
        while (*h && *h != '.') { h++; len++; }

        if (len > 0 && len < 64) {
            *p++ = len;
            for (int i = 0; i < len; i++) *p++ = start[i];
        }
        if (*h == '.') h++;
    }
    *p++ = 0;  /* End of name */

    /* QTYPE = A (1) */
    *p++ = 0x00; *p++ = 0x01;
    /* QCLASS = IN (1) */
    *p++ = 0x00; *p++ = 0x01;

    return p - buf;
}

/* Start DNS resolution */
void dns_resolve_start(dns_query_t* query, const char* hostname) {
    query->state = DNS_STATE_PENDING;
    query->query_id = dns_query_id_counter++;
    query->timeout_tick = tick_counter + 30000;  /* 30 sec timeout */
    query->retry_tick = tick_counter + 1000;     /* Retry every 1 sec */
    memset(query->result_ip, 0, 4);

    /* Store hostname for retries */
    int i;
    for (i = 0; i < 63 && hostname[i]; i++) {
        query->hostname[i] = hostname[i];
    }
    query->hostname[i] = 0;

    /* Build and send DNS query */
    uint8_t dns_buf[256];
    int len = build_dns_query(dns_buf, query->query_id, hostname);

    /* Use DNS server from DHCP or default to QEMU's 10.0.2.3 */
    uint8_t dns_server[4];
    if (config.dns[0] != 0) {
        memcpy(dns_server, config.dns, 4);
    } else {
        dns_server[0] = 10;
        dns_server[1] = 0;
        dns_server[2] = 2;
        dns_server[3] = 3;
    }

    net_send_udp(dns_server, 12345, DNS_PORT, dns_buf, len);
    active_dns_query = query;
}

/* Resend DNS query (called from poll if needed) */
static void dns_retry(dns_query_t* query, const char* hostname) {
    uint8_t dns_buf[256];
    int len = build_dns_query(dns_buf, query->query_id, hostname);

    uint8_t dns_server[4];
    if (config.dns[0] != 0) {
        memcpy(dns_server, config.dns, 4);
    } else {
        dns_server[0] = 10;
        dns_server[1] = 0;
        dns_server[2] = 2;
        dns_server[3] = 3;
    }

    net_send_udp(dns_server, 12345, DNS_PORT, dns_buf, len);
}

/* Handle DNS response in UDP handler */
static void handle_dns_response(uint8_t* data, int len) {
    if (len < 12) return;
    if (!active_dns_query || active_dns_query->state != DNS_STATE_PENDING) return;

    /* Parse DNS header */
    uint16_t id = (data[0] << 8) | data[1];
    if (id != active_dns_query->query_id) return;

    uint16_t flags = (data[2] << 8) | data[3];
    if ((flags & 0x8000) == 0) return;  /* Not a response */
    if ((flags & 0x000F) != 0) {        /* RCODE error */
        active_dns_query->state = DNS_STATE_ERROR;
        return;
    }

    uint16_t ancount = (data[6] << 8) | data[7];
    if (ancount == 0) {
        active_dns_query->state = DNS_STATE_ERROR;
        return;
    }

    /* Skip question section - find answer */
    uint8_t* p = data + 12;
    uint8_t* end = data + len;

    /* Skip QNAME */
    while (p < end && *p != 0) {
        if ((*p & 0xC0) == 0xC0) { p += 2; break; }
        p += *p + 1;
    }
    if (p < end && *p == 0) p++;
    p += 4;  /* Skip QTYPE and QCLASS */

    /* Parse first answer */
    if (p + 12 > end) return;

    /* Skip NAME (might be pointer) */
    if ((*p & 0xC0) == 0xC0) p += 2;
    else {
        while (p < end && *p != 0) p += *p + 1;
        if (p < end && *p == 0) p++;
    }

    if (p + 10 > end) return;

    uint16_t atype = (p[0] << 8) | p[1];
    uint16_t rdlen = (p[8] << 8) | p[9];
    p += 10;  /* Skip TYPE, CLASS, TTL, RDLEN headers */

    if (atype == 1 && rdlen == 4 && p + 4 <= end) {
        /* A record - IPv4 address */
        memcpy(active_dns_query->result_ip, p, 4);
        active_dns_query->state = DNS_STATE_DONE;
    } else {
        active_dns_query->state = DNS_STATE_ERROR;
    }
}

/* Poll DNS resolution status */
int dns_resolve_poll(dns_query_t* query) {
    if (query->state == DNS_STATE_PENDING) {
        if (tick_counter > query->timeout_tick) {
            query->state = DNS_STATE_ERROR;
        } else if (tick_counter > query->retry_tick) {
            /* Retry DNS query */
            dns_retry(query, query->hostname);
            query->retry_tick = tick_counter + 500;
        }
    }
    return query->state;
}
