/*
 * Virtio Block Driver definitions
 * Based on virtio-blk specification
 */

#ifndef VIRTIO_BLK_H
#define VIRTIO_BLK_H

#include "types.h"

/* Virtio device ID for block device */
#define VIRTIO_DEVICE_BLOCK     2

/* Block device sector size */
#define SECTOR_SIZE             512

/* Virtio-blk request types */
#define VIRTIO_BLK_T_IN         0   /* Read */
#define VIRTIO_BLK_T_OUT        1   /* Write */
#define VIRTIO_BLK_T_FLUSH      4   /* Flush */
#define VIRTIO_BLK_T_GET_ID     8   /* Get device ID string */

/* Virtio-blk status codes */
#define VIRTIO_BLK_S_OK         0
#define VIRTIO_BLK_S_IOERR      1
#define VIRTIO_BLK_S_UNSUPP     2

/* Disk info structure */
typedef struct {
    uint64_t capacity;      /* Total sectors */
    uint32_t sector_size;   /* Bytes per sector (usually 512) */
    int available;          /* 1 if disk detected */
} disk_info_t;

/* Initialize virtio-blk driver */
void blk_init(void);

/* Get disk info */
disk_info_t* blk_get_info(void);

/* Check if disk is available */
int blk_available(void);

/* Read sectors from disk
 * sector: starting sector number
 * count: number of sectors to read
 * buf: output buffer (must be at least count * 512 bytes)
 * Returns: 0 on success, -1 on error
 */
int blk_read(uint64_t sector, uint32_t count, void* buf);

/* Write sectors to disk
 * sector: starting sector number
 * count: number of sectors to write
 * buf: input buffer (must be at least count * 512 bytes)
 * Returns: 0 on success, -1 on error
 */
int blk_write(uint64_t sector, uint32_t count, const void* buf);

/* Flush disk cache */
int blk_flush(void);

#endif /* VIRTIO_BLK_H */
