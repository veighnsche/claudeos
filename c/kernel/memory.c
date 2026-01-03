/*
 * TinyOS Memory Allocator
 *
 * First-fit free list allocator with block coalescing.
 * Designed for simplicity while providing real malloc/free support.
 */

#include "memory.h"

/* Block header - placed before each allocation */
typedef struct block_header {
    size_t size;                    /* Total size including header */
    struct block_header* next;      /* Next block in memory order */
    uint32_t is_free;               /* 1 = free, 0 = allocated */
    uint32_t magic;                 /* Magic number for validation */
} block_header_t;

#define BLOCK_MAGIC     0xDEADBEEF
#define HEADER_SIZE     sizeof(block_header_t)
#define MIN_BLOCK_SIZE  (HEADER_SIZE + 16)  /* Minimum useful block */

/* Alignment for ARM64 - 16 bytes */
#define ALIGN_SIZE  16
#define ALIGN(x)    (((x) + (ALIGN_SIZE - 1)) & ~(ALIGN_SIZE - 1))

/* Heap boundaries - defined in linker script */
extern char __heap_start[];
extern char __heap_end[];

/* Head of the block list */
static block_header_t* heap_head = NULL;
static int heap_initialized = 0;

/* Statistics */
static size_t total_allocated = 0;
static size_t total_freed = 0;

/*
 * Initialize the heap with a single large free block
 */
void heap_init(void) {
    if (heap_initialized) return;

    /* Calculate heap size */
    size_t heap_size = (size_t)(__heap_end - __heap_start);

    /* Align the start address */
    char* aligned_start = (char*)ALIGN((uintptr_t)__heap_start);
    heap_size -= (aligned_start - __heap_start);
    heap_size = heap_size & ~(ALIGN_SIZE - 1);  /* Align size down */

    /* Create initial free block spanning entire heap */
    heap_head = (block_header_t*)aligned_start;
    heap_head->size = heap_size;
    heap_head->next = NULL;
    heap_head->is_free = 1;
    heap_head->magic = BLOCK_MAGIC;

    heap_initialized = 1;
}

/*
 * Allocate memory of given size
 * Returns NULL if allocation fails
 */
void* malloc(size_t size) {
    if (!heap_initialized) heap_init();
    if (size == 0) return NULL;

    /* Calculate total size needed (header + aligned payload) */
    size_t total_size = ALIGN(HEADER_SIZE + size);
    if (total_size < MIN_BLOCK_SIZE) {
        total_size = MIN_BLOCK_SIZE;
    }

    /* First-fit: find first free block that's big enough */
    block_header_t* current = heap_head;

    while (current != NULL) {
        /* Validate block */
        if (current->magic != BLOCK_MAGIC) {
            /* Heap corruption detected! */
            return NULL;
        }

        if (current->is_free && current->size >= total_size) {
            /* Found a suitable block */

            /* Split if block is significantly larger than needed */
            if (current->size >= total_size + MIN_BLOCK_SIZE) {
                /* Create new block from remainder */
                block_header_t* new_block = (block_header_t*)((char*)current + total_size);
                new_block->size = current->size - total_size;
                new_block->next = current->next;
                new_block->is_free = 1;
                new_block->magic = BLOCK_MAGIC;

                current->size = total_size;
                current->next = new_block;
            }

            current->is_free = 0;
            total_allocated += current->size;

            /* Return pointer to payload (after header) */
            return (void*)((char*)current + HEADER_SIZE);
        }

        current = current->next;
    }

    /* No suitable block found */
    return NULL;
}

/*
 * Free previously allocated memory
 */
void free(void* ptr) {
    if (ptr == NULL) return;

    /* Get block header */
    block_header_t* block = (block_header_t*)((char*)ptr - HEADER_SIZE);

    /* Validate magic number */
    if (block->magic != BLOCK_MAGIC) {
        /* Invalid pointer - corruption or double free */
        return;
    }

    /* Already free? */
    if (block->is_free) {
        return;  /* Double free - ignore */
    }

    /* Mark as free */
    block->is_free = 1;
    total_freed += block->size;

    /* Coalesce with next block if it's free */
    if (block->next != NULL && block->next->is_free) {
        block->size += block->next->size;
        block->next = block->next->next;
    }

    /* Coalesce with previous block if it's free */
    /* Need to find the previous block by walking from head */
    block_header_t* current = heap_head;
    while (current != NULL && current->next != NULL) {
        if (current->is_free && current->next == block) {
            /* Merge current with block */
            current->size += block->size;
            current->next = block->next;
            break;
        }
        /* Also check if current->next is free and adjacent to its next */
        if (current->next != NULL && current->next->is_free &&
            current->next->next != NULL && current->next->next == block && block->is_free) {
            /* Three-way merge */
            current->next->size += block->size;
            current->next->next = block->next;
        }
        current = current->next;
    }
}

/*
 * Allocate and zero-initialize memory
 */
void* calloc(size_t num, size_t size) {
    size_t total = num * size;

    /* Check for overflow */
    if (num != 0 && total / num != size) {
        return NULL;
    }

    void* ptr = malloc(total);
    if (ptr != NULL) {
        memset(ptr, 0, total);
    }
    return ptr;
}

/*
 * Resize a previously allocated block
 */
void* realloc(void* ptr, size_t size) {
    if (ptr == NULL) {
        return malloc(size);
    }
    if (size == 0) {
        free(ptr);
        return NULL;
    }

    /* Get current block */
    block_header_t* block = (block_header_t*)((char*)ptr - HEADER_SIZE);

    /* Validate */
    if (block->magic != BLOCK_MAGIC) {
        return NULL;
    }

    size_t current_payload = block->size - HEADER_SIZE;

    /* If new size fits in current block, return same pointer */
    if (size <= current_payload) {
        return ptr;
    }

    /* Need to allocate new block */
    void* new_ptr = malloc(size);
    if (new_ptr != NULL) {
        memcpy(new_ptr, ptr, current_payload);
        free(ptr);
    }
    return new_ptr;
}

/*
 * Fill memory with a value
 */
void* memset(void* ptr, int value, size_t num) {
    unsigned char* p = (unsigned char*)ptr;
    unsigned char v = (unsigned char)value;
    for (size_t i = 0; i < num; i++) {
        p[i] = v;
    }
    return ptr;
}

/*
 * Copy memory from src to dest
 */
void* memcpy(void* dest, const void* src, size_t num) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;
    for (size_t i = 0; i < num; i++) {
        d[i] = s[i];
    }
    return dest;
}

/*
 * Get total free bytes in heap
 */
size_t heap_free_bytes(void) {
    if (!heap_initialized) heap_init();

    size_t free_bytes = 0;
    block_header_t* current = heap_head;

    while (current != NULL) {
        if (current->is_free) {
            free_bytes += current->size - HEADER_SIZE;
        }
        current = current->next;
    }
    return free_bytes;
}

/*
 * Get total used bytes in heap
 */
size_t heap_used_bytes(void) {
    if (!heap_initialized) heap_init();

    size_t used_bytes = 0;
    block_header_t* current = heap_head;

    while (current != NULL) {
        if (!current->is_free) {
            used_bytes += current->size - HEADER_SIZE;
        }
        current = current->next;
    }
    return used_bytes;
}

/*
 * Move memory (handles overlapping regions)
 */
void* memmove(void* dest, const void* src, size_t num) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;

    if (d < s) {
        /* Copy forward */
        for (size_t i = 0; i < num; i++) {
            d[i] = s[i];
        }
    } else if (d > s) {
        /* Copy backward (handles overlap) */
        for (size_t i = num; i > 0; i--) {
            d[i - 1] = s[i - 1];
        }
    }
    return dest;
}

/*
 * Compare memory
 */
int memcmp(const void* s1, const void* s2, size_t n) {
    const unsigned char* p1 = (const unsigned char*)s1;
    const unsigned char* p2 = (const unsigned char*)s2;

    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) {
            return p1[i] - p2[i];
        }
    }
    return 0;
}
