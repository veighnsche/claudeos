/*
 * TinyOS Network Stack
 */
#ifndef NET_H
#define NET_H

#include "types.h"

/* Ethernet header */
#define ETH_ALEN 6
#define ETH_HLEN 14

#define ETH_P_IP   0x0800
#define ETH_P_ARP  0x0806

struct eth_hdr {
    uint8_t dest[ETH_ALEN];
    uint8_t src[ETH_ALEN];
    uint16_t ethertype;
} __attribute__((packed));

/* ARP header */
#define ARP_REQUEST 1
#define ARP_REPLY   2

struct arp_hdr {
    uint16_t hw_type;       /* Hardware type (Ethernet = 1) */
    uint16_t proto_type;    /* Protocol type (IP = 0x0800) */
    uint8_t hw_len;         /* Hardware address length (6) */
    uint8_t proto_len;      /* Protocol address length (4) */
    uint16_t opcode;        /* ARP_REQUEST or ARP_REPLY */
    uint8_t sender_mac[6];
    uint8_t sender_ip[4];
    uint8_t target_mac[6];
    uint8_t target_ip[4];
} __attribute__((packed));

/* IP header */
#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

struct ip_hdr {
    uint8_t version_ihl;    /* Version (4 bits) + IHL (4 bits) */
    uint8_t tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t frag_off;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t checksum;
    uint8_t src_ip[4];
    uint8_t dest_ip[4];
} __attribute__((packed));

/* ICMP header */
#define ICMP_ECHO_REPLY   0
#define ICMP_ECHO_REQUEST 8

struct icmp_hdr {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} __attribute__((packed));

/* UDP header */
struct udp_hdr {
    uint16_t src_port;
    uint16_t dest_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed));

/* DHCP message */
#define DHCP_BOOTREQUEST 1
#define DHCP_BOOTREPLY   2
#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5

struct dhcp_msg {
    uint8_t op;
    uint8_t htype;
    uint8_t hlen;
    uint8_t hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint8_t ciaddr[4];
    uint8_t yiaddr[4];
    uint8_t siaddr[4];
    uint8_t giaddr[4];
    uint8_t chaddr[16];
    uint8_t sname[64];
    uint8_t file[128];
    uint8_t options[312];
} __attribute__((packed));

/* Network configuration */
typedef struct {
    uint8_t ip[4];
    uint8_t subnet[4];
    uint8_t gateway[4];
    uint8_t dns[4];
    int configured;
    int dhcp_state;
} net_config_t;

/* Ping status */
typedef struct {
    int sent;
    int received;
    int last_rtt_ms;
    uint32_t last_ping_time;
} ping_status_t;

/* Initialize network stack */
void net_init(void);

/* Process incoming packets */
void net_poll(void);

/* Get network configuration */
net_config_t* net_get_config(void);

/* Get ping status */
ping_status_t* net_get_ping_status(void);

/* Send a ping to gateway */
void net_ping_gateway(void);

/* Format IP address to string */
void net_ip_to_str(const uint8_t* ip, char* buf);

/* Format MAC address to string */
void net_mac_to_str(const uint8_t* mac, char* buf);

/* ARP functions for TCP */
int net_arp_lookup(const uint8_t* ip, uint8_t* mac_out);
void net_send_arp_request(const uint8_t* target_ip);

/* DNS resolution */
#define DNS_STATE_IDLE     0
#define DNS_STATE_PENDING  1
#define DNS_STATE_DONE     2
#define DNS_STATE_ERROR    3

typedef struct {
    int state;
    uint8_t result_ip[4];
    uint16_t query_id;
    uint32_t timeout_tick;
    uint32_t retry_tick;
    char hostname[64];
} dns_query_t;

void dns_resolve_start(dns_query_t* query, const char* hostname);
int dns_resolve_poll(dns_query_t* query);
void net_send_udp(const uint8_t* dest_ip, uint16_t src_port, uint16_t dest_port,
                  const void* data, int len);

/* Byte order conversion */
static inline uint16_t htons(uint16_t x) {
    return ((x & 0xFF) << 8) | ((x >> 8) & 0xFF);
}

static inline uint16_t ntohs(uint16_t x) {
    return htons(x);
}

static inline uint32_t htonl(uint32_t x) {
    return ((x & 0xFF) << 24) | ((x & 0xFF00) << 8) |
           ((x >> 8) & 0xFF00) | ((x >> 24) & 0xFF);
}

static inline uint32_t ntohl(uint32_t x) {
    return htonl(x);
}

#endif
