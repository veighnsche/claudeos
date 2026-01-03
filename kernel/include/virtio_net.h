/*
 * Virtio Network Device Driver
 */
#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

#include "types.h"

/* Virtio net header (prepended to every packet) */
struct virtio_net_hdr {
    uint8_t flags;
    uint8_t gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
};

#define VIRTIO_NET_HDR_SIZE 10

/* Network status */
typedef struct {
    int detected;           /* Device found */
    int available;          /* Driver fully initialized, can send/recv */
    int link_up;            /* Link status */
    uint8_t mac[6];         /* MAC address */
} net_status_t;

/* Initialize virtio-net driver */
void virtio_net_init(void);

/* Check if network is available */
int virtio_net_available(void);

/* Get network status */
net_status_t* virtio_net_get_status(void);

/* Send raw ethernet frame (without virtio header) */
int virtio_net_send(const void* data, uint32_t len);

/* Receive ethernet frame (without virtio header), returns length or 0 */
int virtio_net_recv(void* buffer, uint32_t max_len);

/* Poll for incoming packets */
void virtio_net_poll(void);

#endif
