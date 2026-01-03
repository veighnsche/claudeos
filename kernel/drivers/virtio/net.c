/*
 * Virtio Network Device Driver for TinyOS
 */

#include "virtio_net.h"
#include "memory.h"

/* Virtio MMIO registers */
#define VIRTIO_MAGIC         0x000
#define VIRTIO_VERSION       0x004
#define VIRTIO_DEVICE_ID     0x008
#define VIRTIO_VENDOR_ID     0x00C
#define VIRTIO_DEV_FEATURES  0x010
#define VIRTIO_DRV_FEATURES  0x020
#define VIRTIO_GUEST_PAGE_SZ 0x028
#define VIRTIO_QUEUE_SEL     0x030
#define VIRTIO_QUEUE_NUM_MAX 0x034
#define VIRTIO_QUEUE_NUM     0x038
#define VIRTIO_QUEUE_ALIGN   0x03C
#define VIRTIO_QUEUE_PFN     0x040
#define VIRTIO_QUEUE_NOTIFY  0x050
#define VIRTIO_INT_STATUS    0x060
#define VIRTIO_INT_ACK       0x064
#define VIRTIO_STATUS        0x070
#define VIRTIO_CONFIG        0x100

/* Status bits */
#define VIRTIO_STATUS_ACK        1
#define VIRTIO_STATUS_DRIVER     2
#define VIRTIO_STATUS_DRIVER_OK  4
#define VIRTIO_STATUS_FEATURES_OK 8

/* Virtio net features */
#define VIRTIO_NET_F_MAC    (1 << 5)
#define VIRTIO_NET_F_STATUS (1 << 16)

/* Virtqueue structures */
struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

#define VIRTQ_DESC_F_NEXT     1
#define VIRTQ_DESC_F_WRITE    2

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
};

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
};

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[];
};

/* Queue configuration */
#define QUEUE_SIZE 16
#define RX_QUEUE 0
#define TX_QUEUE 1

/* Packet buffer size */
#define PACKET_BUF_SIZE 2048

/* Driver state */
static volatile uint32_t* mmio_base = 0;
static net_status_t status = {0};

/* Memory will be allocated from heap */
static uint8_t* net_memory = 0;
#define NET_MEM_SIZE     (64 * 1024)  /* 64KB for all network buffers */

/* Virtqueue pointers */
static struct virtq_desc* rx_desc;
static struct virtq_avail* rx_avail;
static struct virtq_used* rx_used;
static struct virtq_desc* tx_desc;
static struct virtq_avail* tx_avail;
static struct virtq_used* tx_used;

/* Packet buffers */
static uint8_t* rx_buffers;  /* QUEUE_SIZE * PACKET_BUF_SIZE */
static uint8_t* tx_buffer;   /* Single TX buffer */

/* Tracking */
static uint16_t rx_last_used = 0;
static uint16_t tx_free_desc = 0;

/* MMIO helpers */
static inline uint32_t mmio_read(uint32_t offset) {
    return mmio_base[offset / 4];
}

static inline void mmio_write(uint32_t offset, uint32_t val) {
    mmio_base[offset / 4] = val;
}

/* Calculate virtqueue offsets per legacy spec */
#define VIRTQ_DESC_SIZE (QUEUE_SIZE * 16)
#define VIRTQ_AVAIL_SIZE (6 + QUEUE_SIZE * 2)
#define VIRTQ_AVAIL_OFFSET VIRTQ_DESC_SIZE
#define VIRTQ_USED_OFFSET ((VIRTQ_DESC_SIZE + VIRTQ_AVAIL_SIZE + 4095) & ~4095)
#define VIRTQ_TOTAL_SIZE (VIRTQ_USED_OFFSET + 6 + QUEUE_SIZE * 8)

/* Initialize a virtqueue - returns pointers via out params */
static void init_queue_at(int queue_num, uint8_t* base,
                          struct virtq_desc** desc_out,
                          struct virtq_avail** avail_out,
                          struct virtq_used** used_out) {
    mmio_write(VIRTIO_QUEUE_SEL, queue_num);

    uint32_t max_size = mmio_read(VIRTIO_QUEUE_NUM_MAX);
    if (max_size == 0 || max_size < QUEUE_SIZE) return;

    mmio_write(VIRTIO_QUEUE_NUM, QUEUE_SIZE);
    mmio_write(VIRTIO_QUEUE_ALIGN, 4096);

    /* Layout per virtio legacy spec - device computes offsets from PFN */
    *desc_out = (struct virtq_desc*)base;
    *avail_out = (struct virtq_avail*)(base + VIRTQ_AVAIL_OFFSET);
    *used_out = (struct virtq_used*)(base + VIRTQ_USED_OFFSET);

    /* Clear entire queue memory */
    memset(base, 0, VIRTQ_TOTAL_SIZE);

    /* Set queue PFN (page frame number) */
    mmio_write(VIRTIO_QUEUE_PFN, (uint64_t)base / 4096);
}

/* Setup RX buffers */
static void setup_rx_buffers(void) {
    for (int i = 0; i < QUEUE_SIZE; i++) {
        rx_desc[i].addr = (uint64_t)(rx_buffers + i * PACKET_BUF_SIZE);
        rx_desc[i].len = PACKET_BUF_SIZE;
        rx_desc[i].flags = VIRTQ_DESC_F_WRITE;
        rx_desc[i].next = 0;

        rx_avail->ring[i] = i;
    }
    rx_avail->idx = QUEUE_SIZE;

    /* Notify device */
    mmio_write(VIRTIO_QUEUE_NOTIFY, RX_QUEUE);
}

/* Debug UART */
#define NET_UART_BASE 0x09000000
static void net_puts(const char* s) {
    while (*s) *(volatile uint32_t*)NET_UART_BASE = *s++;
}

void virtio_net_init(void) {
    status.detected = 0;
    status.available = 0;
    status.link_up = 0;

    /* Scan for virtio-net device (ID 1) */
    for (int slot = 0; slot < 32; slot++) {
        uint64_t base = 0x0a000000 + slot * 0x200;
        volatile uint32_t* probe = (volatile uint32_t*)base;
        uint32_t magic = probe[VIRTIO_MAGIC / 4];
        uint32_t device_id = probe[VIRTIO_DEVICE_ID / 4];

        if (magic != 0x74726976 || device_id != 1) continue;

        net_puts("NET: Found\r\n");
        mmio_base = probe;

        /* Reset device */
        mmio_write(VIRTIO_STATUS, 0);
        for (volatile int i = 0; i < 10000; i++);

        /* Set guest page size for legacy v1 */
        uint32_t version = mmio_read(VIRTIO_VERSION);
        if (version == 1) {
            mmio_write(VIRTIO_GUEST_PAGE_SZ, 4096);
        }

        /* Acknowledge device */
        mmio_write(VIRTIO_STATUS, VIRTIO_STATUS_ACK);
        mmio_write(VIRTIO_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);

        /* Read features and negotiate MAC feature */
        uint32_t features = mmio_read(VIRTIO_DEV_FEATURES);
        uint32_t wanted = VIRTIO_NET_F_MAC;
        mmio_write(VIRTIO_DRV_FEATURES, features & wanted);

        /* Read MAC address from config space */
        volatile uint8_t* mac_cfg = (volatile uint8_t*)((uint64_t)mmio_base + VIRTIO_CONFIG);
        for (int i = 0; i < 6; i++) {
            status.mac[i] = mac_cfg[i];
        }

        status.detected = 1;
        /* Log MAC address */
        net_puts("MAC: ");
        const char hex[] = "0123456789ABCDEF";
        for (int i = 0; i < 6; i++) {
            char c1 = hex[status.mac[i] >> 4];
            char c2 = hex[status.mac[i] & 0x0F];
            *((volatile uint32_t*)0x09000000) = c1;
            *((volatile uint32_t*)0x09000000) = c2;
            if (i < 5) *((volatile uint32_t*)0x09000000) = ':';
        }
        net_puts("\r\n");

        /* Allocate memory for queues and buffers - page aligned */
        net_memory = (uint8_t*)0x47000000;

        /* Layout: RX queue (contiguous per spec), TX queue, then buffers */
        uint8_t* rx_queue_base = net_memory;
        uint8_t* tx_queue_base = net_memory + 0x2000;  /* 8KB offset - enough for RX queue */
        rx_buffers = net_memory + 0x4000;              /* 16KB offset */
        tx_buffer = rx_buffers + QUEUE_SIZE * PACKET_BUF_SIZE;

        /* Initialize RX queue with correct layout */
        init_queue_at(RX_QUEUE, rx_queue_base, &rx_desc, &rx_avail, &rx_used);
        setup_rx_buffers();

        /* Initialize TX queue with correct layout */
        init_queue_at(TX_QUEUE, tx_queue_base, &tx_desc, &tx_avail, &tx_used);

        /* Set driver OK */
        if (version == 1) {
            mmio_write(VIRTIO_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);
        } else {
            mmio_write(VIRTIO_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);
            mmio_write(VIRTIO_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);
        }

        status.available = 1;
        status.link_up = 1;
        net_puts("NET: Ready\r\n");
        return;
    }

    /* No device found - use placeholder MAC */
    status.detected = 1;
    status.mac[0] = 0x52;
    status.mac[1] = 0x54;
    status.mac[2] = 0x00;
    status.mac[3] = 0x12;
    status.mac[4] = 0x34;
    status.mac[5] = 0x56;
}

int virtio_net_available(void) {
    return status.available;
}

net_status_t* virtio_net_get_status(void) {
    /* Just return status - no MMIO access */
    return &status;
}

int virtio_net_send(const void* data, uint32_t len) {
    if (!status.available || !tx_buffer || len > PACKET_BUF_SIZE - VIRTIO_NET_HDR_SIZE) {
        return -1;
    }

    /* Get TX buffer */
    uint8_t* buf = tx_buffer;

    /* Prepare virtio-net header */
    struct virtio_net_hdr* hdr = (struct virtio_net_hdr*)buf;
    memset(hdr, 0, VIRTIO_NET_HDR_SIZE);

    /* Copy packet data after header */
    memcpy(buf + VIRTIO_NET_HDR_SIZE, data, len);

    /* Setup descriptor */
    tx_desc[tx_free_desc].addr = (uint64_t)buf;
    tx_desc[tx_free_desc].len = VIRTIO_NET_HDR_SIZE + len;
    tx_desc[tx_free_desc].flags = 0;
    tx_desc[tx_free_desc].next = 0;

    /* Add to available ring */
    uint16_t avail_idx = tx_avail->idx % QUEUE_SIZE;
    tx_avail->ring[avail_idx] = tx_free_desc;

    /* Memory barrier */
    __asm__ volatile("dmb sy" ::: "memory");

    tx_avail->idx++;

    /* Notify device */
    mmio_write(VIRTIO_QUEUE_NOTIFY, TX_QUEUE);

    /* Cycle through descriptors */
    tx_free_desc = (tx_free_desc + 1) % QUEUE_SIZE;

    return 0;
}

int virtio_net_recv(void* buffer, uint32_t max_len) {
    if (!status.available || !rx_used || !rx_desc) return 0;

    /* Memory barrier before reading used ring */
    __asm__ volatile("dmb ish" ::: "memory");

    /* Check if device has returned any buffers */
    volatile struct virtq_used* used = rx_used;
    uint16_t used_idx = used->idx;
    if (used_idx == rx_last_used) {
        return 0;  /* No new packets */
    }

    /* Get used buffer info */
    uint16_t ring_idx = rx_last_used % QUEUE_SIZE;
    uint32_t desc_idx = rx_used->ring[ring_idx].id;
    uint32_t total_len = rx_used->ring[ring_idx].len;

    /* Validate descriptor index */
    if (desc_idx >= QUEUE_SIZE) {
        rx_last_used++;
        return 0;
    }

    /* Skip virtio-net header */
    uint8_t* pkt = (uint8_t*)(uintptr_t)rx_desc[desc_idx].addr;
    if (total_len <= VIRTIO_NET_HDR_SIZE) {
        rx_last_used++;
        return 0;
    }

    uint32_t pkt_len = total_len - VIRTIO_NET_HDR_SIZE;
    if (pkt_len > max_len) pkt_len = max_len;

    /* Copy packet data (skip header) */
    memcpy(buffer, pkt + VIRTIO_NET_HDR_SIZE, pkt_len);

    /* Return buffer to available ring */
    uint16_t avail_idx = rx_avail->idx % QUEUE_SIZE;
    rx_avail->ring[avail_idx] = desc_idx;

    __asm__ volatile("dmb ish" ::: "memory");

    rx_avail->idx++;
    rx_last_used++;

    /* Notify device we have more buffers */
    mmio_write(VIRTIO_QUEUE_NOTIFY, RX_QUEUE);

    return pkt_len;
}

void virtio_net_poll(void) {
    if (!status.available || !mmio_base) return;

    /* Acknowledge any interrupts */
    uint32_t int_status = mmio_read(VIRTIO_INT_STATUS);
    if (int_status) {
        mmio_write(VIRTIO_INT_ACK, int_status);
    }
}
