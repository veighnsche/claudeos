/*
 * ARM64 Exception Vector Table
 * Each entry is 128 bytes (32 instructions max)
 * 16 total entries: 4 exception types x 4 source levels
 */

.section ".text.vectors", "ax"
.global vectors
.balign 0x800

vectors:
/* ============================================
 * Current EL with SP_EL0 (not used in kernel)
 * ============================================ */

/* 0x000: Synchronous */
.balign 0x80
curr_el_sp0_sync:
    b       exception_hang

/* 0x080: IRQ */
.balign 0x80
curr_el_sp0_irq:
    b       exception_hang

/* 0x100: FIQ */
.balign 0x80
curr_el_sp0_fiq:
    b       exception_hang

/* 0x180: SError */
.balign 0x80
curr_el_sp0_serror:
    b       exception_hang

/* ============================================
 * Current EL with SP_ELx (kernel uses this)
 * ============================================ */

/* 0x200: Synchronous */
.balign 0x80
curr_el_spx_sync:
    b       exception_hang

/* 0x280: IRQ - Main interrupt handler */
.balign 0x80
curr_el_spx_irq:
    /* Save context */
    sub     sp, sp, #256
    stp     x0, x1, [sp, #0]
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    stp     x8, x9, [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    str     x30, [sp, #240]

    /* Call C handler */
    bl      irq_handler

    /* Restore context */
    ldp     x0, x1, [sp, #0]
    ldp     x2, x3, [sp, #16]
    ldp     x4, x5, [sp, #32]
    ldp     x6, x7, [sp, #48]
    ldp     x8, x9, [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    ldr     x30, [sp, #240]
    add     sp, sp, #256

    eret

/* 0x300: FIQ */
.balign 0x80
curr_el_spx_fiq:
    b       exception_hang

/* 0x380: SError */
.balign 0x80
curr_el_spx_serror:
    b       exception_hang

/* ============================================
 * Lower EL using AArch64 (not used)
 * ============================================ */

/* 0x400: Synchronous */
.balign 0x80
lower_el_a64_sync:
    b       exception_hang

/* 0x480: IRQ */
.balign 0x80
lower_el_a64_irq:
    b       exception_hang

/* 0x500: FIQ */
.balign 0x80
lower_el_a64_fiq:
    b       exception_hang

/* 0x580: SError */
.balign 0x80
lower_el_a64_serror:
    b       exception_hang

/* ============================================
 * Lower EL using AArch32 (not used)
 * ============================================ */

/* 0x600: Synchronous */
.balign 0x80
lower_el_a32_sync:
    b       exception_hang

/* 0x680: IRQ */
.balign 0x80
lower_el_a32_irq:
    b       exception_hang

/* 0x700: FIQ */
.balign 0x80
lower_el_a32_fiq:
    b       exception_hang

/* 0x780: SError */
.balign 0x80
lower_el_a32_serror:
    b       exception_hang

/* Default exception handler - hangs */
exception_hang:
    wfe
    b       exception_hang
