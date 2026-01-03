ENTRY(_head)

MEMORY
{
    /* Android emulator actually loads kernel at 0x40200000 */
    RAM (rwx) : ORIGIN = 0x40200000, LENGTH = 128M
}

SECTIONS
{
    . = 0x40200000;

    /* Linux ARM64 header must be first */
    .text.head : {
        *(.text.head)
    } > RAM

    .text : {
        *(.text.boot)
        . = ALIGN(0x800);
        *(.text.vectors)
        *(.text*)
    } > RAM

    .rodata : {
        *(.rodata*)
    } > RAM

    .data : {
        *(.data*)
    } > RAM

    .bss : {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    } > RAM

    . = ALIGN(16);
    . = . + 0x10000;
    stack_top = .;

    /* Heap starts after stack, ends before framebuffer at 0x42000000 */
    . = ALIGN(16);
    __heap_start = .;
    __heap_end = 0x41F00000;  /* Leave 1MB gap before framebuffer */

    _kernel_size = . - 0x40080000;
}
