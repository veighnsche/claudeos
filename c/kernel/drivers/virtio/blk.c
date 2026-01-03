/*
 * Virtio Block Driver
 * Handles disk I/O through virtio-blk device
 */

#include "types.h"
#include "virtio_blk.h"
#include "memory.h"

/* Virtio MMIO scan range */
#define VIRTIO_MMIO_START       0x0a000000
#define VIRTIO_MMIO_SIZE        0x200
#define VIRTIO_MMIO_COUNT       32

/* Virtio MMIO registers */
#define VIRTIO_MMIO_MAGIC           0x000
#define VIRTIO_MMIO_VERSION         0x004
#define VIRTIO_MMIO_DEVICE_ID       0x008
#define VIRTIO_MMIO_VENDOR_ID       0x00c
#define VIRTIO_MMIO_DEV_FEAT        0x010
#define VIRTIO_MMIO_DEV_FEAT_SEL    0x014
#define VIRTIO_MMIO_DRV_FEAT        0x020
#define VIRTIO_MMIO_DRV_FEAT_SEL    0x024
#define VIRTIO_MMIO_GUEST_PAGE_SIZE 0x028
#define VIRTIO_MMIO_QUEUE_SEL       0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX   0x034
#define VIRTIO_MMIO_QUEUE_NUM       0x038
#define VIRTIO_MMIO_QUEUE_ALIGN     0x03c
#define VIRTIO_MMIO_QUEUE_PFN       0x040
#define VIRTIO_MMIO_QUEUE_READY     0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY    0x050
#define VIRTIO_MMIO_INT_STATUS      0x060
#define VIRTIO_MMIO_INT_ACK         0x064
#define VIRTIO_MMIO_STATUS          0x070
#define VIRTIO_MMIO_QUEUE_DESC_LOW  0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH 0x084
#define VIRTIO_MMIO_QUEUE_AVAIL_LOW 0x090
#define VIRTIO_MMIO_QUEUE_AVAIL_HIGH 0x094
#define VIRTIO_MMIO_QUEUE_USED_LOW  0x0a0
#define VIRTIO_MMIO_QUEUE_USED_HIGH 0x0a4

/* Virtio-blk config space (at offset 0x100) */
#define VIRTIO_BLK_CFG_CAPACITY     0x100  /* 64-bit capacity in sectors */
#define VIRTIO_BLK_CFG_SIZE_MAX     0x108  /* Max segment size */
#define VIRTIO_BLK_CFG_SEG_MAX      0x10c  /* Max segments per request */

/* Virtqueue descriptor flags */
#define VIRTQ_DESC_F_NEXT       1
#define VIRTQ_DESC_F_WRITE      2

/* Memory regions - must be in valid RAM */
#define BLK_VIRTQUEUE_BASE      0x47100000
#define BLK_REQUEST_BASE        0x47110000
#define BLK_DATA_BASE           0x47120000

/* Virtqueue structures */
struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed, aligned(16)));

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[16];
} __attribute__((packed, aligned(2)));

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[16];
} __attribute__((packed, aligned(4)));

/* Virtio-blk request header */
struct virtio_blk_req {
    uint32_t type;      /* VIRTIO_BLK_T_* */
    uint32_t reserved;
    uint64_t sector;    /* Sector number */
} __attribute__((packed));

/* Device state */
static uint64_t blk_base = 0;
static int blk_version = 0;
static int blk_initialized = 0;
static disk_info_t disk_info;

/* Virtqueue state */
static struct virtq_desc* vq_desc;
static struct virtq_avail* vq_avail;
static struct virtq_used* vq_used;
static uint16_t vq_num = 0;
static uint16_t vq_free_head = 0;
static uint16_t vq_last_used = 0;
static int descs_in_use = 0;

/* Request/data buffers */
static struct virtio_blk_req* req_header;
static uint8_t* data_buffer;
static uint8_t* status_byte;

static inline void mmio_write(uint64_t base, uint32_t offset, uint32_t value) {
    *(volatile uint32_t*)(base + offset) = value;
    __asm__ volatile("dmb sy" ::: "memory");
}

static inline uint32_t mmio_read(uint64_t base, uint32_t offset) {
    __asm__ volatile("dmb sy" ::: "memory");
    return *(volatile uint32_t*)(base + offset);
}

/* Debug UART */
#define UART0_BASE 0x09000000
static void debug_putc(char c) {
    *(volatile uint32_t*)UART0_BASE = c;
}

static void debug_puts(const char* s) {
    while (*s) debug_putc(*s++);
}

static void debug_hex32(uint32_t val) {
    const char hex[] = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4) {
        debug_putc(hex[(val >> i) & 0xF]);
    }
}

static void debug_hex64(uint64_t val) {
    const char hex[] = "0123456789ABCDEF";
    for (int i = 60; i >= 0; i -= 4) {
        debug_putc(hex[(val >> i) & 0xF]);
    }
}

/* Find virtio-blk device */
static uint64_t find_virtio_blk(void) {
    for (int i = 0; i < VIRTIO_MMIO_COUNT; i++) {
        uint64_t base = VIRTIO_MMIO_START + (i * VIRTIO_MMIO_SIZE);
        uint32_t magic = mmio_read(base, VIRTIO_MMIO_MAGIC);
        uint32_t device_id = mmio_read(base, VIRTIO_MMIO_DEVICE_ID);

        if (magic == 0x74726976 && device_id == VIRTIO_DEVICE_BLOCK) {
            return base;
        }
    }
    return 0;
}

/* Initialize virtqueue */
static void virtqueue_init(void) {
    uint64_t queue_base = BLK_VIRTQUEUE_BASE;

    /* Select queue 0 */
    mmio_write(blk_base, VIRTIO_MMIO_QUEUE_SEL, 0);

    /* Get max queue size - use small fixed size for reliability */
    uint32_t max_num = mmio_read(blk_base, VIRTIO_MMIO_QUEUE_NUM_MAX);
    vq_num = 16;  /* Use small queue size */
    if (max_num > 0 && max_num < vq_num) vq_num = max_num;

    /* Set queue size */
    mmio_write(blk_base, VIRTIO_MMIO_QUEUE_NUM, vq_num);

    /* Calculate layout - must match virtio legacy spec */
    uint32_t desc_size = vq_num * 16;
    uint32_t avail_size = 6 + 2 * vq_num;
    uint32_t used_offset = (desc_size + avail_size + 4095) & ~4095;

    vq_desc = (struct virtq_desc*)(queue_base);
    vq_avail = (struct virtq_avail*)(queue_base + desc_size);
    vq_used = (struct virtq_used*)(queue_base + used_offset);

    /* Clear all structures */
    volatile uint8_t* ptr = (volatile uint8_t*)queue_base;
    for (uint32_t i = 0; i < used_offset + 2048; i++) {
        ptr[i] = 0;
    }

    /* Initialize available ring explicitly */
    vq_avail->flags = 0;
    vq_avail->idx = 0;

    /* Initialize used ring explicitly */
    vq_used->flags = 0;
    vq_used->idx = 0;

    /* Initialize descriptor free list */
    for (uint16_t i = 0; i < vq_num; i++) {
        vq_desc[i].addr = 0;
        vq_desc[i].len = 0;
        vq_desc[i].flags = 0;
        vq_desc[i].next = (i + 1) % vq_num;
    }
    vq_free_head = 0;
    vq_last_used = 0;
    descs_in_use = 0;

    __asm__ volatile("dmb sy" ::: "memory");

    if (blk_version == 1) {
        mmio_write(blk_base, VIRTIO_MMIO_QUEUE_ALIGN, 4096);
        mmio_write(blk_base, VIRTIO_MMIO_QUEUE_PFN, (uint32_t)(queue_base >> 12));
    } else {
        mmio_write(blk_base, VIRTIO_MMIO_QUEUE_DESC_LOW, (uint32_t)(uint64_t)vq_desc);
        mmio_write(blk_base, VIRTIO_MMIO_QUEUE_DESC_HIGH, (uint32_t)((uint64_t)vq_desc >> 32));
        mmio_write(blk_base, VIRTIO_MMIO_QUEUE_AVAIL_LOW, (uint32_t)(uint64_t)vq_avail);
        mmio_write(blk_base, VIRTIO_MMIO_QUEUE_AVAIL_HIGH, (uint32_t)((uint64_t)vq_avail >> 32));
        mmio_write(blk_base, VIRTIO_MMIO_QUEUE_USED_LOW, (uint32_t)(uint64_t)vq_used);
        mmio_write(blk_base, VIRTIO_MMIO_QUEUE_USED_HIGH, (uint32_t)((uint64_t)vq_used >> 32));
        mmio_write(blk_base, VIRTIO_MMIO_QUEUE_READY, 1);
    }
}

/* Allocate descriptor */
static int alloc_desc(void) {
    if (descs_in_use >= vq_num) {
        return -1;  /* No free descriptors */
    }
    int desc = vq_free_head;
    vq_free_head = vq_desc[desc].next;
    descs_in_use++;
    return desc;
}

/* Free descriptor */
static void free_desc(int desc) {
    if (desc < 0 || desc >= vq_num) return;
    vq_desc[desc].next = vq_free_head;
    vq_free_head = desc;
    if (descs_in_use > 0) descs_in_use--;
}

/* Perform I/O operation */
static int do_blk_io(uint32_t type, uint64_t sector, uint32_t count, void* buf) {
    if (!blk_initialized) return -1;
    if (count == 0) return 0;
    if (count > 128) count = 128;  /* Limit to data buffer size */

    /* Setup request header */
    req_header->type = type;
    req_header->reserved = 0;
    req_header->sector = sector;

    /* For writes, copy data to buffer */
    if (type == VIRTIO_BLK_T_OUT && buf) {
        memcpy(data_buffer, buf, count * SECTOR_SIZE);
    }

    /* Clear status */
    *status_byte = 0xFF;

    /* Allocate descriptors: header, data, status */
    int desc0 = alloc_desc();
    int desc1 = alloc_desc();
    int desc2 = alloc_desc();

    /* Check allocation succeeded */
    if (desc0 < 0 || desc1 < 0 || desc2 < 0) {
        if (desc0 >= 0) free_desc(desc0);
        if (desc1 >= 0) free_desc(desc1);
        if (desc2 >= 0) free_desc(desc2);
        return -1;
    }

    /* Header descriptor (device-readable) */
    vq_desc[desc0].addr = (uint64_t)req_header;
    vq_desc[desc0].len = sizeof(struct virtio_blk_req);
    vq_desc[desc0].flags = VIRTQ_DESC_F_NEXT;
    vq_desc[desc0].next = desc1;

    /* Data descriptor */
    vq_desc[desc1].addr = (uint64_t)data_buffer;
    vq_desc[desc1].len = count * SECTOR_SIZE;
    if (type == VIRTIO_BLK_T_IN) {
        vq_desc[desc1].flags = VIRTQ_DESC_F_WRITE | VIRTQ_DESC_F_NEXT;
    } else {
        vq_desc[desc1].flags = VIRTQ_DESC_F_NEXT;
    }
    vq_desc[desc1].next = desc2;

    /* Status descriptor (device-writable) */
    vq_desc[desc2].addr = (uint64_t)status_byte;
    vq_desc[desc2].len = 1;
    vq_desc[desc2].flags = VIRTQ_DESC_F_WRITE;
    vq_desc[desc2].next = 0;

    __asm__ volatile("dmb sy" ::: "memory");

    /* Add to available ring */
    uint16_t avail_idx = vq_avail->idx;
    vq_avail->ring[avail_idx % vq_num] = desc0;
    __asm__ volatile("dmb sy" ::: "memory");
    vq_avail->idx = avail_idx + 1;
    __asm__ volatile("dmb sy" ::: "memory");

    /* Notify device */
    mmio_write(blk_base, VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    /* Wait for completion */
    volatile int timeout = 10000000;
    while (vq_used->idx == vq_last_used && timeout > 0) {
        __asm__ volatile("dmb sy" ::: "memory");
        timeout--;
    }

    /* Acknowledge interrupts */
    uint32_t int_status = mmio_read(blk_base, VIRTIO_MMIO_INT_STATUS);
    if (int_status) {
        mmio_write(blk_base, VIRTIO_MMIO_INT_ACK, int_status);
    }

    vq_last_used = vq_used->idx;

    /* Free descriptors */
    free_desc(desc0);
    free_desc(desc1);
    free_desc(desc2);

    /* Check status */
    if (*status_byte != VIRTIO_BLK_S_OK) {
        return -1;
    }

    /* For reads, copy data from buffer */
    if (type == VIRTIO_BLK_T_IN && buf) {
        memcpy(buf, data_buffer, count * SECTOR_SIZE);
    }

    return 0;
}

void blk_init(void) {
    memset(&disk_info, 0, sizeof(disk_info));

    /* Find device */
    blk_base = find_virtio_blk();
    if (blk_base == 0) {
        debug_puts("virtio-blk: not found\r\n");
        return;
    }

    debug_puts("virtio-blk: found at 0x");
    debug_hex64(blk_base);
    debug_puts("\r\n");

    /* Get version and reset */
    blk_version = mmio_read(blk_base, VIRTIO_MMIO_VERSION);
    mmio_write(blk_base, VIRTIO_MMIO_STATUS, 0);
    for (volatile int i = 0; i < 10000; i++);

    /* Legacy v1: set guest page size */
    if (blk_version == 1) {
        mmio_write(blk_base, VIRTIO_MMIO_GUEST_PAGE_SIZE, 4096);
    }

    /* Acknowledge */
    mmio_write(blk_base, VIRTIO_MMIO_STATUS, 1);

    /* Driver loaded */
    mmio_write(blk_base, VIRTIO_MMIO_STATUS, 1 | 2);

    /* Accept features (none special needed) */
    mmio_write(blk_base, VIRTIO_MMIO_DEV_FEAT_SEL, 0);
    mmio_write(blk_base, VIRTIO_MMIO_DRV_FEAT_SEL, 0);
    mmio_write(blk_base, VIRTIO_MMIO_DRV_FEAT, 0);

    /* Initialize virtqueue */
    virtqueue_init();

    /* Set driver ready */
    if (blk_version == 1) {
        mmio_write(blk_base, VIRTIO_MMIO_STATUS, 1 | 2 | 4);
    } else {
        mmio_write(blk_base, VIRTIO_MMIO_STATUS, 1 | 2 | 8);
        mmio_write(blk_base, VIRTIO_MMIO_STATUS, 1 | 2 | 8 | 4);
    }

    /* Setup buffers */
    req_header = (struct virtio_blk_req*)BLK_REQUEST_BASE;
    data_buffer = (uint8_t*)BLK_DATA_BASE;
    status_byte = (uint8_t*)(BLK_REQUEST_BASE + sizeof(struct virtio_blk_req));

    /* Read capacity from config space */
    uint32_t cap_low = mmio_read(blk_base, VIRTIO_BLK_CFG_CAPACITY);
    uint32_t cap_high = mmio_read(blk_base, VIRTIO_BLK_CFG_CAPACITY + 4);
    disk_info.capacity = ((uint64_t)cap_high << 32) | cap_low;
    disk_info.sector_size = SECTOR_SIZE;
    disk_info.available = 1;

    debug_puts("virtio-blk: capacity ");
    debug_hex64(disk_info.capacity);
    debug_puts(" sectors (");
    /* Print size in MB */
    uint64_t mb = (disk_info.capacity * 512) / (1024 * 1024);
    char buf[16];
    int i = 15;
    buf[i--] = 0;
    if (mb == 0) buf[i--] = '0';
    while (mb > 0 && i >= 0) {
        buf[i--] = '0' + (mb % 10);
        mb /= 10;
    }
    debug_puts(&buf[i + 1]);
    debug_puts(" MB)\r\n");

    blk_initialized = 1;
}

disk_info_t* blk_get_info(void) {
    return &disk_info;
}

int blk_available(void) {
    return blk_initialized;
}

int blk_read(uint64_t sector, uint32_t count, void* buf) {
    /* Read in chunks if count is large (data buffer is 64KB = 128 sectors) */
    while (count > 0) {
        uint32_t chunk = count > 128 ? 128 : count;
        if (do_blk_io(VIRTIO_BLK_T_IN, sector, chunk, buf) != 0) {
            return -1;
        }
        sector += chunk;
        count -= chunk;
        buf = (uint8_t*)buf + chunk * SECTOR_SIZE;
    }
    return 0;
}

int blk_write(uint64_t sector, uint32_t count, const void* buf) {
    /* Write in chunks */
    while (count > 0) {
        uint32_t chunk = count > 128 ? 128 : count;
        if (do_blk_io(VIRTIO_BLK_T_OUT, sector, chunk, (void*)buf) != 0) {
            return -1;
        }
        sector += chunk;
        count -= chunk;
        buf = (const uint8_t*)buf + chunk * SECTOR_SIZE;
    }
    return 0;
}

int blk_flush(void) {
    if (!blk_initialized) return -1;

    /* Setup flush request */
    req_header->type = VIRTIO_BLK_T_FLUSH;
    req_header->reserved = 0;
    req_header->sector = 0;
    *status_byte = 0xFF;

    int desc0 = alloc_desc();
    int desc1 = alloc_desc();

    /* Header */
    vq_desc[desc0].addr = (uint64_t)req_header;
    vq_desc[desc0].len = sizeof(struct virtio_blk_req);
    vq_desc[desc0].flags = VIRTQ_DESC_F_NEXT;
    vq_desc[desc0].next = desc1;

    /* Status */
    vq_desc[desc1].addr = (uint64_t)status_byte;
    vq_desc[desc1].len = 1;
    vq_desc[desc1].flags = VIRTQ_DESC_F_WRITE;
    vq_desc[desc1].next = 0;

    __asm__ volatile("dmb sy" ::: "memory");

    uint16_t avail_idx = vq_avail->idx;
    vq_avail->ring[avail_idx % vq_num] = desc0;
    __asm__ volatile("dmb sy" ::: "memory");
    vq_avail->idx = avail_idx + 1;
    __asm__ volatile("dmb sy" ::: "memory");

    mmio_write(blk_base, VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    volatile int timeout = 10000000;
    while (vq_used->idx == vq_last_used && timeout > 0) {
        __asm__ volatile("dmb sy" ::: "memory");
        timeout--;
    }

    uint32_t int_status = mmio_read(blk_base, VIRTIO_MMIO_INT_STATUS);
    if (int_status) {
        mmio_write(blk_base, VIRTIO_MMIO_INT_ACK, int_status);
    }

    vq_last_used = vq_used->idx;
    free_desc(desc0);
    free_desc(desc1);

    return (*status_byte == VIRTIO_BLK_S_OK) ? 0 : -1;
}
