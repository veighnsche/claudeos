/*
 * TinyFS - Simple Filesystem for TinyOS
 */

#ifndef FS_H
#define FS_H

#include "types.h"

/* Filesystem constants */
#define FS_MAGIC            0x54465321  /* "TFS!" */
#define FS_VERSION          1
#define FS_MAX_FILENAME     20
#define FS_MAX_FILES        64
#define FS_MAX_OPEN         8
#define FS_CLUSTER_SIZE     2048        /* 4 sectors per cluster */
#define FS_SECTORS_PER_CLUSTER  4

/* File flags */
#define FS_FLAG_DIR         0x01
#define FS_FLAG_READONLY    0x02

/* Open modes */
#define FS_O_READ           0x01
#define FS_O_WRITE          0x02
#define FS_O_CREATE         0x04
#define FS_O_TRUNC          0x08
#define FS_O_APPEND         0x10

/* Seek modes */
#define FS_SEEK_SET         0
#define FS_SEEK_CUR         1
#define FS_SEEK_END         2

/* FAT special values */
#define FAT_FREE            0x0000
#define FAT_EOF             0xFFFF
#define FAT_BAD             0xFFF7

/* Directory entry structure (32 bytes) */
typedef struct {
    char name[FS_MAX_FILENAME];     /* Filename (null-terminated) */
    uint32_t size;                  /* File size in bytes */
    uint16_t first_cluster;         /* Starting cluster */
    uint16_t flags;                 /* File flags */
    uint32_t reserved;              /* Reserved for future use */
} fs_dirent_t;

/* Superblock structure (512 bytes, one sector) */
typedef struct {
    uint32_t magic;                 /* FS_MAGIC */
    uint32_t version;               /* FS_VERSION */
    uint32_t total_sectors;         /* Total disk sectors */
    uint32_t total_clusters;        /* Total data clusters */
    uint32_t free_clusters;         /* Free clusters count */
    uint32_t fat_start;             /* FAT starting sector */
    uint32_t fat_sectors;           /* Sectors used by FAT */
    uint32_t root_start;            /* Root directory start sector */
    uint32_t root_sectors;          /* Sectors for root directory */
    uint32_t data_start;            /* Data area start sector */
    uint8_t reserved[472];          /* Pad to 512 bytes */
} fs_superblock_t;

/* Initialize filesystem (mount or detect unformatted) */
int fs_init(void);

/* Check if filesystem is mounted */
int fs_mounted(void);

/* Format disk with TinyFS */
int fs_format(void);

/* Open file
 * path: file path (just filename for now, no subdirs)
 * flags: FS_O_* flags
 * Returns: file descriptor (>= 0) or -1 on error
 */
int fs_open(const char* path, int flags);

/* Close file */
int fs_close(int fd);

/* Read from file
 * Returns: bytes read, 0 on EOF, -1 on error
 */
int fs_read(int fd, void* buf, int len);

/* Write to file
 * Returns: bytes written, -1 on error
 */
int fs_write(int fd, const void* buf, int len);

/* Seek in file
 * Returns: new position, -1 on error
 */
int fs_seek(int fd, int offset, int whence);

/* Get file size */
int fs_size(int fd);

/* List directory
 * path: directory path (only "/" supported)
 * entries: output array
 * max_entries: max entries to return
 * Returns: number of entries, -1 on error
 */
int fs_readdir(const char* path, fs_dirent_t* entries, int max_entries);

/* Delete file */
int fs_remove(const char* path);

/* Get filesystem stats */
typedef struct {
    uint32_t total_clusters;
    uint32_t free_clusters;
    uint32_t cluster_size;
    uint32_t files_count;
} fs_stats_t;

int fs_stats(fs_stats_t* stats);

#endif /* FS_H */
