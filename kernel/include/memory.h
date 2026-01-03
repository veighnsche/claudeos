#ifndef MEMORY_H
#define MEMORY_H

#include "types.h"

/*
 * TinyOS Memory Allocator
 *
 * A simple first-fit free list allocator that supports:
 * - malloc: Allocate memory
 * - free: Release memory
 * - calloc: Allocate and zero memory
 * - realloc: Resize allocation
 */

/* Initialize the heap - called automatically on first malloc */
void heap_init(void);

/* Standard memory allocation functions */
void* malloc(size_t size);
void free(void* ptr);
void* calloc(size_t num, size_t size);
void* realloc(void* ptr, size_t size);

/* Memory utilities */
void* memset(void* ptr, int value, size_t num);
void* memcpy(void* dest, const void* src, size_t num);
void* memmove(void* dest, const void* src, size_t num);
int memcmp(const void* s1, const void* s2, size_t n);

/* Debug: get heap statistics */
size_t heap_free_bytes(void);
size_t heap_used_bytes(void);

#endif /* MEMORY_H */
