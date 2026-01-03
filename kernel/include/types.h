#ifndef TYPES_H
#define TYPES_H

typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;

typedef signed char int8_t;
typedef signed short int16_t;
typedef signed int int32_t;
typedef signed long long int64_t;

/* Use 64-bit size_t for ARM64 */
#ifdef __aarch64__
typedef uint64_t size_t;
typedef int64_t ssize_t;
typedef uint64_t uintptr_t;
#else
typedef uint32_t size_t;
typedef int32_t ssize_t;
typedef uint32_t uintptr_t;
#endif

#define NULL ((void*)0)

/* Boolean type - use _Bool if available */
#ifndef __cplusplus
#ifndef bool
#define bool _Bool
#define true 1
#define false 0
#endif
#endif

#endif