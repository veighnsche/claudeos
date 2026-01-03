/*
 * ARM64 Linux Kernel Image Header + Boot Code
 * This format is required for the Android emulator
 */

.section ".text.head", "ax"
.global _head
.global _start

_head:
    /* ARM64 kernel header - DO NOT MODIFY structure */
    b       _start                  /* Branch to kernel start */
    .long   0                       /* Reserved */
    .quad   0x0                     /* Image load offset */
    .quad   _end - _head            /* Image size */
    .quad   0x0A                    /* Flags: LE kernel, 4K pages */
    .quad   0                       /* Reserved */
    .quad   0                       /* Reserved */
    .quad   0                       /* Reserved */
    .ascii  "ARM\x64"               /* Magic: ARM64 marker */
    .long   0                       /* Reserved (PE-COFF offset) */

.section ".text.boot", "ax"
_start:
    /* Disable interrupts */
    msr     daifset, #0xf

    /* Check processor ID - only primary CPU continues */
    mrs     x0, mpidr_el1
    and     x0, x0, #0xFF
    cbz     x0, primary_cpu

    /* Secondary CPUs halt */
secondary_halt:
    wfe
    b       secondary_halt

primary_cpu:
    /* Set up stack pointer - use high memory */
    ldr     x0, =0x48000000
    mov     sp, x0

    /* Set up exception vector table */
    ldr     x0, =vectors
    msr     vbar_el1, x0
    isb

    /* Clear BSS section */
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
    mov     x2, #0
bss_loop:
    cmp     x0, x1
    b.ge    bss_done
    str     x2, [x0], #8
    b       bss_loop
bss_done:

    /* Jump to C code */
    bl      kernel_main

    /* If main returns, halt */
halt:
    wfe
    b       halt

.section ".data"
.global _end
_end:
