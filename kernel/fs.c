/*
 * TinyFS - Simple Filesystem Implementation
 *
 * Disk Layout:
 * Sector 0:      Superblock
 * Sector 1-8:    FAT (File Allocation Table)
 * Sector 9-16:   Root directory (64 entries)
 * Sector 17+:    Data clusters (2KB each = 4 sectors)
 */

#include "fs.h"
#include "virtio_blk.h"
#include "memory.h"

/* Disk layout constants */
#define SUPERBLOCK_SECTOR   0
#define FAT_START_SECTOR    1
#define FAT_SECTORS         8       /* 8 * 512 = 4096 bytes = 2048 FAT entries */
#define ROOT_START_SECTOR   9
#define ROOT_SECTORS        4       /* 64 entries * 32 bytes = 2048 bytes = 4 sectors */
#define DATA_START_SECTOR   13

/* Open file descriptor */
typedef struct {
    int in_use;
    int dirent_idx;         /* Index in root directory */
    uint32_t size;          /* File size */
    uint32_t pos;           /* Current read/write position */
    uint16_t first_cluster; /* Starting cluster */
    int flags;              /* Open flags */
} open_file_t;

/* Cached filesystem state */
static fs_superblock_t superblock;
static uint16_t fat[2048];          /* FAT entries (up to 2048 clusters) */
static fs_dirent_t root_dir[64];    /* Root directory */
static open_file_t open_files[FS_MAX_OPEN];
static int fs_is_mounted = 0;

/* Sector buffer */
static uint8_t sector_buf[512];

/* String utilities */
static int str_len(const char* s) {
    int len = 0;
    while (s[len]) len++;
    return len;
}

static int str_cmp(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a - *b;
}

static void str_cpy(char* dst, const char* src, int max) {
    int i;
    for (i = 0; i < max - 1 && src[i]; i++) {
        dst[i] = src[i];
    }
    dst[i] = 0;
}

/* Read superblock */
static int read_superblock(void) {
    if (blk_read(SUPERBLOCK_SECTOR, 1, &superblock) != 0) {
        return -1;
    }
    return 0;
}

/* Write superblock */
static int write_superblock(void) {
    return blk_write(SUPERBLOCK_SECTOR, 1, &superblock);
}

/* Read FAT */
static int read_fat(void) {
    return blk_read(FAT_START_SECTOR, FAT_SECTORS, fat);
}

/* Write FAT */
static int write_fat(void) {
    return blk_write(FAT_START_SECTOR, FAT_SECTORS, fat);
}

/* Read root directory */
static int read_root_dir(void) {
    return blk_read(ROOT_START_SECTOR, ROOT_SECTORS, root_dir);
}

/* Write root directory */
static int write_root_dir(void) {
    return blk_write(ROOT_START_SECTOR, ROOT_SECTORS, root_dir);
}

/* Convert cluster number to sector number */
static uint32_t cluster_to_sector(uint16_t cluster) {
    return DATA_START_SECTOR + (cluster * FS_SECTORS_PER_CLUSTER);
}

/* Allocate a free cluster (start from 1, cluster 0 is reserved) */
static int alloc_cluster(void) {
    for (int i = 1; i < (int)superblock.total_clusters; i++) {
        if (fat[i] == FAT_FREE) {
            fat[i] = FAT_EOF;
            superblock.free_clusters--;
            return i;
        }
    }
    return -1;  /* No free clusters */
}

/* Free a cluster chain */
static void free_cluster_chain(uint16_t start) {
    while (start != FAT_EOF && start != FAT_FREE && start < 2048) {
        uint16_t next = fat[start];
        fat[start] = FAT_FREE;
        superblock.free_clusters++;
        start = next;
    }
}

/* Find file in root directory */
static int find_file(const char* name) {
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (root_dir[i].name[0] != 0 && str_cmp(root_dir[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/* Find free directory entry */
static int find_free_dirent(void) {
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (root_dir[i].name[0] == 0) {
            return i;
        }
    }
    return -1;
}

int fs_init(void) {
    if (!blk_available()) {
        return -1;
    }

    /* Clear open files */
    memset(open_files, 0, sizeof(open_files));

    /* Read superblock */
    if (read_superblock() != 0) {
        return -1;
    }

    /* Check magic */
    if (superblock.magic != FS_MAGIC) {
        /* Disk not formatted or corrupted */
        fs_is_mounted = 0;
        return 0;  /* Return success but not mounted */
    }

    /* Read FAT and root directory */
    if (read_fat() != 0 || read_root_dir() != 0) {
        return -1;
    }

    fs_is_mounted = 1;
    return 0;
}

int fs_mounted(void) {
    return fs_is_mounted;
}

int fs_format(void) {
    if (!blk_available()) {
        return -1;
    }

    disk_info_t* info = blk_get_info();

    /* Calculate filesystem geometry */
    uint32_t total_sectors = info->capacity;
    if (total_sectors < 32) {
        return -1;  /* Disk too small */
    }

    uint32_t data_sectors = total_sectors - DATA_START_SECTOR;
    uint32_t total_clusters = data_sectors / FS_SECTORS_PER_CLUSTER;
    if (total_clusters > 2048) total_clusters = 2048;  /* FAT limit */

    /* Initialize superblock */
    memset(&superblock, 0, sizeof(superblock));
    superblock.magic = FS_MAGIC;
    superblock.version = FS_VERSION;
    superblock.total_sectors = total_sectors;
    superblock.total_clusters = total_clusters;
    superblock.free_clusters = total_clusters;
    superblock.fat_start = FAT_START_SECTOR;
    superblock.fat_sectors = FAT_SECTORS;
    superblock.root_start = ROOT_START_SECTOR;
    superblock.root_sectors = ROOT_SECTORS;
    superblock.data_start = DATA_START_SECTOR;

    /* Initialize FAT - all free except cluster 0 (reserved) */
    memset(fat, 0, sizeof(fat));
    fat[0] = FAT_EOF;  /* Reserve cluster 0 */
    superblock.free_clusters--;  /* Adjust for reserved cluster */

    /* Initialize root directory - all empty */
    memset(root_dir, 0, sizeof(root_dir));

    /* Clear open files */
    memset(open_files, 0, sizeof(open_files));

    /* Write to disk */
    if (write_superblock() != 0) return -1;
    if (write_fat() != 0) return -1;
    if (write_root_dir() != 0) return -1;

    /* Flush */
    blk_flush();

    fs_is_mounted = 1;
    return 0;
}

int fs_open(const char* path, int flags) {
    if (!fs_is_mounted) return -1;

    /* Skip leading slash */
    if (*path == '/') path++;
    if (*path == 0) return -1;  /* Can't open root as file */

    /* Find free file descriptor */
    int fd = -1;
    for (int i = 0; i < FS_MAX_OPEN; i++) {
        if (!open_files[i].in_use) {
            fd = i;
            break;
        }
    }
    if (fd < 0) return -1;  /* Too many open files */

    /* Find file */
    int idx = find_file(path);

    if (idx < 0) {
        /* File not found */
        if (!(flags & FS_O_CREATE)) {
            return -1;
        }

        /* Create new file */
        idx = find_free_dirent();
        if (idx < 0) return -1;  /* Directory full */

        str_cpy(root_dir[idx].name, path, FS_MAX_FILENAME);
        root_dir[idx].size = 0;
        root_dir[idx].first_cluster = FAT_EOF;
        root_dir[idx].flags = 0;
        root_dir[idx].reserved = 0;

        if (write_root_dir() != 0) {
            root_dir[idx].name[0] = 0;
            return -1;
        }
    } else if (flags & FS_O_TRUNC) {
        /* Truncate existing file */
        if (root_dir[idx].first_cluster != FAT_EOF) {
            free_cluster_chain(root_dir[idx].first_cluster);
            root_dir[idx].first_cluster = FAT_EOF;
            root_dir[idx].size = 0;
            write_fat();
            write_root_dir();
        }
    }

    /* Setup file descriptor */
    open_files[fd].in_use = 1;
    open_files[fd].dirent_idx = idx;
    open_files[fd].size = root_dir[idx].size;
    open_files[fd].first_cluster = root_dir[idx].first_cluster;
    open_files[fd].flags = flags;

    if (flags & FS_O_APPEND) {
        open_files[fd].pos = root_dir[idx].size;
    } else {
        open_files[fd].pos = 0;
    }

    return fd;
}

int fs_close(int fd) {
    if (fd < 0 || fd >= FS_MAX_OPEN) return -1;
    if (!open_files[fd].in_use) return -1;

    open_files[fd].in_use = 0;
    return 0;
}

int fs_read(int fd, void* buf, int len) {
    if (fd < 0 || fd >= FS_MAX_OPEN) return -1;
    if (!open_files[fd].in_use) return -1;
    if (!(open_files[fd].flags & FS_O_READ)) return -1;

    open_file_t* f = &open_files[fd];
    uint8_t* out = (uint8_t*)buf;
    int bytes_read = 0;

    while (len > 0 && f->pos < f->size) {
        /* Calculate current cluster and offset */
        uint32_t cluster_num = f->pos / FS_CLUSTER_SIZE;
        uint32_t cluster_offset = f->pos % FS_CLUSTER_SIZE;

        /* Find the cluster */
        uint16_t cluster = f->first_cluster;
        for (uint32_t i = 0; i < cluster_num && cluster != FAT_EOF; i++) {
            cluster = fat[cluster];
        }
        if (cluster == FAT_EOF || cluster == FAT_FREE) {
            break;  /* Past end of file */
        }

        /* Read sector within cluster */
        uint32_t sector_in_cluster = cluster_offset / 512;
        uint32_t sector_offset = cluster_offset % 512;
        uint32_t sector = cluster_to_sector(cluster) + sector_in_cluster;

        if (blk_read(sector, 1, sector_buf) != 0) {
            return bytes_read > 0 ? bytes_read : -1;
        }

        /* Copy data */
        uint32_t to_copy = 512 - sector_offset;
        if ((int)to_copy > len) to_copy = len;
        if (f->pos + to_copy > f->size) to_copy = f->size - f->pos;

        memcpy(out, sector_buf + sector_offset, to_copy);
        out += to_copy;
        f->pos += to_copy;
        bytes_read += to_copy;
        len -= to_copy;
    }

    return bytes_read;
}

int fs_write(int fd, const void* buf, int len) {
    if (fd < 0 || fd >= FS_MAX_OPEN) return -1;
    if (!open_files[fd].in_use) return -1;
    if (!(open_files[fd].flags & FS_O_WRITE)) return -1;

    open_file_t* f = &open_files[fd];
    const uint8_t* in = (const uint8_t*)buf;
    int bytes_written = 0;
    int idx = f->dirent_idx;

    while (len > 0) {
        /* Calculate current cluster and offset */
        uint32_t cluster_num = f->pos / FS_CLUSTER_SIZE;
        uint32_t cluster_offset = f->pos % FS_CLUSTER_SIZE;

        /* Find or allocate the cluster */
        uint16_t cluster = f->first_cluster;
        uint16_t prev_cluster = FAT_EOF;

        if (cluster == FAT_EOF) {
            /* File is empty, allocate first cluster */
            int new_cluster = alloc_cluster();
            if (new_cluster < 0) {
                return bytes_written > 0 ? bytes_written : -1;
            }
            cluster = (uint16_t)new_cluster;
            f->first_cluster = cluster;
            root_dir[idx].first_cluster = cluster;
        }

        /* Navigate to target cluster */
        for (uint32_t i = 0; i < cluster_num; i++) {
            prev_cluster = cluster;
            if (fat[cluster] == FAT_EOF) {
                /* Need to extend file */
                int new_cluster = alloc_cluster();
                if (new_cluster < 0) {
                    write_fat();
                    write_root_dir();
                    write_superblock();
                    return bytes_written > 0 ? bytes_written : -1;
                }
                fat[prev_cluster] = new_cluster;
            }
            cluster = fat[cluster];
        }

        /* Calculate sector */
        uint32_t sector_in_cluster = cluster_offset / 512;
        uint32_t sector_offset = cluster_offset % 512;
        uint32_t sector = cluster_to_sector(cluster) + sector_in_cluster;

        /* Read-modify-write if not writing full sector */
        if (sector_offset != 0 || len < 512) {
            if (blk_read(sector, 1, sector_buf) != 0) {
                /* New sector, just clear it */
                memset(sector_buf, 0, 512);
            }
        }

        /* Copy data */
        uint32_t to_copy = 512 - sector_offset;
        if ((int)to_copy > len) to_copy = len;

        memcpy(sector_buf + sector_offset, in, to_copy);

        /* Write sector */
        if (blk_write(sector, 1, sector_buf) != 0) {
            return bytes_written > 0 ? bytes_written : -1;
        }

        in += to_copy;
        f->pos += to_copy;
        bytes_written += to_copy;
        len -= to_copy;

        /* Update size if needed */
        if (f->pos > f->size) {
            f->size = f->pos;
            root_dir[idx].size = f->size;
        }
    }

    /* Update metadata */
    write_fat();
    write_root_dir();
    write_superblock();

    return bytes_written;
}

int fs_seek(int fd, int offset, int whence) {
    if (fd < 0 || fd >= FS_MAX_OPEN) return -1;
    if (!open_files[fd].in_use) return -1;

    open_file_t* f = &open_files[fd];
    int new_pos;

    switch (whence) {
        case FS_SEEK_SET:
            new_pos = offset;
            break;
        case FS_SEEK_CUR:
            new_pos = f->pos + offset;
            break;
        case FS_SEEK_END:
            new_pos = f->size + offset;
            break;
        default:
            return -1;
    }

    if (new_pos < 0) new_pos = 0;
    f->pos = new_pos;
    return new_pos;
}

int fs_size(int fd) {
    if (fd < 0 || fd >= FS_MAX_OPEN) return -1;
    if (!open_files[fd].in_use) return -1;
    return open_files[fd].size;
}

int fs_readdir(const char* path, fs_dirent_t* entries, int max_entries) {
    if (!fs_is_mounted) return -1;
    (void)path;  /* Only root directory supported */

    int count = 0;
    for (int i = 0; i < FS_MAX_FILES && count < max_entries; i++) {
        if (root_dir[i].name[0] != 0) {
            memcpy(&entries[count], &root_dir[i], sizeof(fs_dirent_t));
            count++;
        }
    }
    return count;
}

int fs_remove(const char* path) {
    if (!fs_is_mounted) return -1;

    /* Skip leading slash */
    if (*path == '/') path++;
    if (*path == 0) return -1;

    /* Check if file is open */
    int idx = find_file(path);
    if (idx < 0) return -1;  /* File not found */

    for (int i = 0; i < FS_MAX_OPEN; i++) {
        if (open_files[i].in_use && open_files[i].dirent_idx == idx) {
            return -1;  /* File is open */
        }
    }

    /* Free clusters */
    if (root_dir[idx].first_cluster != FAT_EOF) {
        free_cluster_chain(root_dir[idx].first_cluster);
    }

    /* Clear directory entry */
    memset(&root_dir[idx], 0, sizeof(fs_dirent_t));

    /* Write changes */
    write_fat();
    write_root_dir();
    write_superblock();

    return 0;
}

int fs_stats(fs_stats_t* stats) {
    if (!fs_is_mounted) return -1;

    stats->total_clusters = superblock.total_clusters;
    stats->free_clusters = superblock.free_clusters;
    stats->cluster_size = FS_CLUSTER_SIZE;

    /* Count files */
    stats->files_count = 0;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (root_dir[i].name[0] != 0) {
            stats->files_count++;
        }
    }

    return 0;
}
