/*
 * Virtio GPU driver for Android Emulator (gfxstream)
 * Proper implementation with continuous display updates
 */

#include "types.h"

/* Virtio MMIO base addresses - scan for GPU device */
#define VIRTIO_MMIO_START       0x0a000000
#define VIRTIO_MMIO_SIZE        0x200
#define VIRTIO_MMIO_COUNT       32

/* Virtio MMIO registers - common */
#define VIRTIO_MMIO_MAGIC       0x000
#define VIRTIO_MMIO_VERSION     0x004
#define VIRTIO_MMIO_DEVICE_ID   0x008
#define VIRTIO_MMIO_VENDOR_ID   0x00c
#define VIRTIO_MMIO_DEV_FEAT    0x010
#define VIRTIO_MMIO_DEV_FEAT_SEL 0x014
#define VIRTIO_MMIO_DRV_FEAT    0x020
#define VIRTIO_MMIO_DRV_FEAT_SEL 0x024
#define VIRTIO_MMIO_QUEUE_SEL   0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX 0x034
#define VIRTIO_MMIO_QUEUE_NUM   0x038
/* Version 1 (legacy) specific */
#define VIRTIO_MMIO_QUEUE_ALIGN 0x03c
#define VIRTIO_MMIO_QUEUE_PFN   0x040
/* Version 1 (legacy) specific */
#define VIRTIO_MMIO_GUEST_PAGE_SIZE 0x028
/* Version 2 (modern) specific */
#define VIRTIO_MMIO_QUEUE_READY 0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY 0x050
#define VIRTIO_MMIO_INT_STATUS  0x060
#define VIRTIO_MMIO_INT_ACK     0x064
#define VIRTIO_MMIO_STATUS      0x070
#define VIRTIO_MMIO_QUEUE_DESC_LOW  0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH 0x084
#define VIRTIO_MMIO_QUEUE_AVAIL_LOW 0x090
#define VIRTIO_MMIO_QUEUE_AVAIL_HIGH 0x094
#define VIRTIO_MMIO_QUEUE_USED_LOW  0x0a0
#define VIRTIO_MMIO_QUEUE_USED_HIGH 0x0a4

static int virtio_version = 0;

/* Virtio GPU commands */
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO     0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D   0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF       0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT          0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH       0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D  0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106
#define VIRTIO_GPU_CMD_CTX_CREATE           0x0200
#define VIRTIO_GPU_CMD_CTX_DESTROY          0x0201
#define VIRTIO_GPU_CMD_SUBMIT_3D            0x0207
#define VIRTIO_GPU_CMD_GET_CAPSET_INFO      0x0108
#define VIRTIO_GPU_CMD_GET_CAPSET           0x0109

/* Response types */
#define VIRTIO_GPU_RESP_OK_NODATA           0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO     0x1101

/* Formats */
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM    1
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM    2
#define VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM    67
#define VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM    68

/* Virtqueue descriptor flags */
#define VIRTQ_DESC_F_NEXT       1
#define VIRTQ_DESC_F_WRITE      2

/* Screen dimensions - will be updated from display info */
static uint32_t fb_width = 720;
static uint32_t fb_height = 1280;

/* Memory regions - MUST be within RAM (0x40080000 - 0x48080000) */
#define FRAMEBUFFER_ADDR    0x42000000
#define VIRTQUEUE_ADDR      0x46000000
#define CMD_BUFFER_ADDR     0x46100000

static uint64_t gpu_base = 0;
static uint32_t* framebuffer = (uint32_t*)FRAMEBUFFER_ADDR;
static int gpu_initialized = 0;

static inline void mmio_write(uint64_t base, uint32_t offset, uint32_t value) {
    *(volatile uint32_t*)(base + offset) = value;
    __asm__ volatile("dmb sy" ::: "memory");
}

static inline uint32_t mmio_read(uint64_t base, uint32_t offset) {
    __asm__ volatile("dmb sy" ::: "memory");
    return *(volatile uint32_t*)(base + offset);
}

/* Virtqueue structures - must be properly aligned */
struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed, aligned(16)));

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[256];
} __attribute__((packed, aligned(2)));

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[256];
} __attribute__((packed, aligned(4)));

/* GPU command structures */
struct virtio_gpu_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

struct virtio_gpu_display_one {
    struct virtio_gpu_rect r;
    uint32_t enabled;
    uint32_t flags;
} __attribute__((packed));

struct virtio_gpu_resp_display_info {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_display_one pmodes[16];
} __attribute__((packed));

struct virtio_gpu_resource_create_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

struct virtio_gpu_set_scanout {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t scanout_id;
    uint32_t resource_id;
} __attribute__((packed));

struct virtio_gpu_mem_entry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_resource_attach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
} __attribute__((packed));

struct virtio_gpu_transfer_to_host_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_resource_flush {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

/* Virtqueue state */
static struct virtq_desc* vq_desc;
static struct virtq_avail* vq_avail;
static struct virtq_used* vq_used;
static uint16_t vq_free_head = 0;
static uint16_t vq_last_used = 0;
static uint16_t vq_num = 0;

/* Command/response buffers */
static uint8_t* cmd_buf;
static uint8_t* resp_buf;

/* Track if scanout has been set (delay until first flush) */
static int scanout_set = 0;

/* Forward declarations */
void virtio_gpu_flush(void);
static void set_scanout(void);

static uint64_t find_virtio_gpu(void) {
    /* Scan MMIO for virtio-gpu (device ID 16) */
    for (int i = 0; i < VIRTIO_MMIO_COUNT; i++) {
        uint64_t base = VIRTIO_MMIO_START + (i * VIRTIO_MMIO_SIZE);
        uint32_t magic = mmio_read(base, VIRTIO_MMIO_MAGIC);
        uint32_t device_id = mmio_read(base, VIRTIO_MMIO_DEVICE_ID);

        if (magic == 0x74726976 && device_id == 16) {
            return base;
        }
    }
    return 0;
}

static void virtqueue_init(void) {
    /*
     * For virtio v1 (legacy), the queue must be laid out as:
     * - Descriptors at offset 0
     * - Available ring right after descriptors
     * - Used ring at next page boundary
     * All in one contiguous region pointed to by QUEUE_PFN
     */
    uint64_t queue_base = VIRTQUEUE_ADDR;

    /* Select queue 0 (controlq) */
    mmio_write(gpu_base, VIRTIO_MMIO_QUEUE_SEL, 0);

    /* Get max queue size */
    vq_num = mmio_read(gpu_base, VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (vq_num > 128) vq_num = 128;
    if (vq_num == 0) vq_num = 16;

    /* Set queue size */
    mmio_write(gpu_base, VIRTIO_MMIO_QUEUE_NUM, vq_num);

    /* Calculate layout for legacy virtio:
     * desc: vq_num * 16 bytes
     * avail: 6 + 2*vq_num bytes
     * used: 6 + 8*vq_num bytes (must be page aligned)
     */
    uint32_t desc_size = vq_num * 16;
    uint32_t avail_size = 6 + 2 * vq_num;
    uint32_t used_offset = (desc_size + avail_size + 4095) & ~4095;

    vq_desc = (struct virtq_desc*)(queue_base);
    vq_avail = (struct virtq_avail*)(queue_base + desc_size);
    vq_used = (struct virtq_used*)(queue_base + used_offset);
    cmd_buf = (uint8_t*)(CMD_BUFFER_ADDR);
    resp_buf = (uint8_t*)(CMD_BUFFER_ADDR + 0x1000);

    /* Clear structures */
    for (uint32_t i = 0; i < used_offset + 4096; i++) {
        ((volatile uint8_t*)queue_base)[i] = 0;
    }

    /* Initialize descriptor chain */
    for (int i = 0; i < (int)vq_num - 1; i++) {
        vq_desc[i].next = i + 1;
    }
    vq_free_head = 0;

    if (virtio_version == 1) {
        /* Version 1 (legacy): use page frame number */
        mmio_write(gpu_base, VIRTIO_MMIO_QUEUE_ALIGN, 4096);
        mmio_write(gpu_base, VIRTIO_MMIO_QUEUE_PFN, (uint32_t)(queue_base >> 12));
    } else {
        /* Version 2 (modern): use 64-bit addresses */
        mmio_write(gpu_base, VIRTIO_MMIO_QUEUE_DESC_LOW, (uint32_t)(uint64_t)vq_desc);
        mmio_write(gpu_base, VIRTIO_MMIO_QUEUE_DESC_HIGH, (uint32_t)((uint64_t)vq_desc >> 32));
        mmio_write(gpu_base, VIRTIO_MMIO_QUEUE_AVAIL_LOW, (uint32_t)(uint64_t)vq_avail);
        mmio_write(gpu_base, VIRTIO_MMIO_QUEUE_AVAIL_HIGH, (uint32_t)((uint64_t)vq_avail >> 32));
        mmio_write(gpu_base, VIRTIO_MMIO_QUEUE_USED_LOW, (uint32_t)(uint64_t)vq_used);
        mmio_write(gpu_base, VIRTIO_MMIO_QUEUE_USED_HIGH, (uint32_t)((uint64_t)vq_used >> 32));
        mmio_write(gpu_base, VIRTIO_MMIO_QUEUE_READY, 1);
    }
}

static int alloc_desc(void) {
    int desc = vq_free_head;
    vq_free_head = vq_desc[desc].next;
    return desc;
}

static void free_desc(int desc) {
    vq_desc[desc].next = vq_free_head;
    vq_free_head = desc;
}

static void send_command(void* cmd, uint32_t cmd_len, void* resp, uint32_t resp_len) {
    /* Allocate descriptors */
    int desc0 = alloc_desc();
    int desc1 = alloc_desc();

    /* Setup command descriptor */
    vq_desc[desc0].addr = (uint64_t)cmd;
    vq_desc[desc0].len = cmd_len;
    vq_desc[desc0].flags = VIRTQ_DESC_F_NEXT;
    vq_desc[desc0].next = desc1;

    /* Setup response descriptor */
    vq_desc[desc1].addr = (uint64_t)resp;
    vq_desc[desc1].len = resp_len;
    vq_desc[desc1].flags = VIRTQ_DESC_F_WRITE;
    vq_desc[desc1].next = 0;

    __asm__ volatile("dmb sy" ::: "memory");

    /* Add to available ring */
    uint16_t avail_idx = vq_avail->idx;
    vq_avail->ring[avail_idx % vq_num] = desc0;
    __asm__ volatile("dmb sy" ::: "memory");
    vq_avail->idx = avail_idx + 1;
    __asm__ volatile("dmb sy" ::: "memory");

    /* Notify device */
    mmio_write(gpu_base, VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    /* Wait for completion with timeout */
    volatile int timeout = 5000000;
    while (vq_used->idx == vq_last_used && timeout > 0) {
        __asm__ volatile("dmb sy" ::: "memory");
        timeout--;
    }

    /* Acknowledge any interrupts */
    uint32_t int_status = mmio_read(gpu_base, VIRTIO_MMIO_INT_STATUS);
    if (int_status) {
        mmio_write(gpu_base, VIRTIO_MMIO_INT_ACK, int_status);
    }

    vq_last_used = vq_used->idx;

    /* Free descriptors */
    free_desc(desc0);
    free_desc(desc1);
}

static void get_display_info(void) {
    struct virtio_gpu_ctrl_hdr* cmd = (void*)cmd_buf;
    struct virtio_gpu_resp_display_info* resp = (void*)resp_buf;

    cmd->type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;
    cmd->flags = 0;
    cmd->fence_id = 0;
    cmd->ctx_id = 0;
    cmd->padding = 0;

    send_command(cmd, sizeof(*cmd), resp, sizeof(*resp));

    if (resp->hdr.type == VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
        if (resp->pmodes[0].enabled) {
            fb_width = resp->pmodes[0].r.width;
            fb_height = resp->pmodes[0].r.height;
            if (fb_width == 0) fb_width = 720;
            if (fb_height == 0) fb_height = 1280;
        }
    }
}

static void create_resource(void) {
    struct virtio_gpu_resource_create_2d* cmd = (void*)cmd_buf;
    struct virtio_gpu_ctrl_hdr* resp = (void*)resp_buf;

    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    cmd->hdr.flags = 0;
    cmd->hdr.fence_id = 0;
    cmd->hdr.ctx_id = 0;
    cmd->hdr.padding = 0;
    cmd->resource_id = 1;
    cmd->format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
    cmd->width = fb_width;
    cmd->height = fb_height;

    send_command(cmd, sizeof(*cmd), resp, sizeof(*resp));
}

static void attach_backing(void) {
    /* Command with inline memory entry */
    struct {
        struct virtio_gpu_resource_attach_backing cmd;
        struct virtio_gpu_mem_entry entry;
    } __attribute__((packed)) *attach = (void*)cmd_buf;
    struct virtio_gpu_ctrl_hdr* resp = (void*)resp_buf;

    attach->cmd.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    attach->cmd.hdr.flags = 0;
    attach->cmd.hdr.fence_id = 0;
    attach->cmd.hdr.ctx_id = 0;
    attach->cmd.hdr.padding = 0;
    attach->cmd.resource_id = 1;
    attach->cmd.nr_entries = 1;
    attach->entry.addr = (uint64_t)framebuffer;
    attach->entry.length = fb_width * fb_height * 4;
    attach->entry.padding = 0;

    send_command(attach, sizeof(*attach), resp, sizeof(*resp));
}

static void set_scanout(void) {
    struct virtio_gpu_set_scanout* cmd = (void*)cmd_buf;
    struct virtio_gpu_ctrl_hdr* resp = (void*)resp_buf;

    cmd->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    cmd->hdr.flags = 0;
    cmd->hdr.fence_id = 0;
    cmd->hdr.ctx_id = 0;
    cmd->hdr.padding = 0;
    cmd->r.x = 0;
    cmd->r.y = 0;
    cmd->r.width = fb_width;
    cmd->r.height = fb_height;
    cmd->scanout_id = 0;
    cmd->resource_id = 1;

    send_command(cmd, sizeof(*cmd), resp, sizeof(*resp));
}

/* Goldfish Framebuffer - ARM64 ranchu address from device tree */
#define GOLDFISH_FB_BASE    0x9010000ULL
#define GOLDFISH_FB_GET_WIDTH   0x00
#define GOLDFISH_FB_GET_HEIGHT  0x04
#define GOLDFISH_FB_INT_STATUS  0x08
#define GOLDFISH_FB_INT_ENABLE  0x0C
#define GOLDFISH_FB_SET_BASE    0x10
#define GOLDFISH_FB_SET_ROTATION 0x14
#define GOLDFISH_FB_SET_BLANK   0x18
#define GOLDFISH_FB_GET_FORMAT  0x24

static void goldfish_fb_write(uint32_t offset, uint32_t value) {
    *(volatile uint32_t*)(GOLDFISH_FB_BASE + offset) = value;
    __asm__ volatile("dmb sy" ::: "memory");
}

static int use_goldfish_fb = 0;

void virtio_gpu_init(void) {
    /* Find virtio-gpu device */
    gpu_base = find_virtio_gpu();
    if (gpu_base == 0) {
        return;
    }

    /* Get version and reset device */
    virtio_version = mmio_read(gpu_base, VIRTIO_MMIO_VERSION);
    mmio_write(gpu_base, VIRTIO_MMIO_STATUS, 0);
    for (volatile int i = 0; i < 10000; i++);

    /* Legacy v1: Set guest page size */
    if (virtio_version == 1) {
        mmio_write(gpu_base, VIRTIO_MMIO_GUEST_PAGE_SIZE, 4096);
    }

    /* Acknowledge */
    mmio_write(gpu_base, VIRTIO_MMIO_STATUS, 1);

    /* Driver loaded */
    mmio_write(gpu_base, VIRTIO_MMIO_STATUS, 1 | 2);

    /* Read and accept features */
    mmio_write(gpu_base, VIRTIO_MMIO_DEV_FEAT_SEL, 0);
    uint32_t features = mmio_read(gpu_base, VIRTIO_MMIO_DEV_FEAT);
    mmio_write(gpu_base, VIRTIO_MMIO_DRV_FEAT_SEL, 0);
    mmio_write(gpu_base, VIRTIO_MMIO_DRV_FEAT, features & 0xFF);

    /* Initialize virtqueue */
    virtqueue_init();

    /* Set driver ready */
    if (virtio_version == 1) {
        mmio_write(gpu_base, VIRTIO_MMIO_STATUS, 1 | 2 | 4);
    } else {
        mmio_write(gpu_base, VIRTIO_MMIO_STATUS, 1 | 2 | 8);
        mmio_write(gpu_base, VIRTIO_MMIO_STATUS, 1 | 2 | 8 | 4);
    }

    /* Setup display - delay set_scanout until first flush */
    get_display_info();
    create_resource();
    attach_backing();
    /* set_scanout() called on first flush to avoid showing garbage */

    gpu_initialized = 1;
}

void virtio_gpu_flush(void) {
    if (!gpu_initialized) return;

    /* For Goldfish FB, just update the base address */
    if (use_goldfish_fb) {
        goldfish_fb_write(GOLDFISH_FB_SET_BASE, (uint32_t)(uint64_t)framebuffer);
        return;
    }

    if (gpu_base == 0) return;

    /* Set scanout on first flush (after framebuffer is drawn) */
    if (!scanout_set) {
        set_scanout();
        scanout_set = 1;
    }

    /* Transfer to host */
    struct virtio_gpu_transfer_to_host_2d* transfer = (void*)cmd_buf;
    struct virtio_gpu_ctrl_hdr* resp = (void*)resp_buf;

    transfer->hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    transfer->hdr.flags = 0;
    transfer->hdr.fence_id = 0;
    transfer->hdr.ctx_id = 0;
    transfer->hdr.padding = 0;
    transfer->r.x = 0;
    transfer->r.y = 0;
    transfer->r.width = fb_width;
    transfer->r.height = fb_height;
    transfer->offset = 0;
    transfer->resource_id = 1;
    transfer->padding = 0;

    send_command(transfer, sizeof(*transfer), resp, sizeof(*resp));

    /* Resource flush */
    struct virtio_gpu_resource_flush* flush = (void*)cmd_buf;

    flush->hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    flush->hdr.flags = 0;
    flush->hdr.fence_id = 0;
    flush->hdr.ctx_id = 0;
    flush->hdr.padding = 0;
    flush->r.x = 0;
    flush->r.y = 0;
    flush->r.width = fb_width;
    flush->r.height = fb_height;
    flush->resource_id = 1;
    flush->padding = 0;

    send_command(flush, sizeof(*flush), resp, sizeof(*resp));
}

uint32_t* virtio_gpu_get_framebuffer(void) {
    return framebuffer;
}

uint32_t virtio_gpu_get_width(void) {
    return fb_width;
}

uint32_t virtio_gpu_get_height(void) {
    return fb_height;
}
