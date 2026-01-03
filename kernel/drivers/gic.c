/*
 * ARM GIC-400 driver for Android emulator
 * Handles interrupt distribution and CPU interface
 */

#include "gic.h"

/* Memory-mapped register access */
static inline void gicd_write(uint32_t offset, uint32_t value) {
    *(volatile uint32_t*)((uint64_t)GICD_BASE + offset) = value;
    __asm__ volatile("dmb sy" ::: "memory");
}

static inline uint32_t gicd_read(uint32_t offset) {
    __asm__ volatile("dmb sy" ::: "memory");
    return *(volatile uint32_t*)((uint64_t)GICD_BASE + offset);
}

static inline void gicc_write(uint32_t offset, uint32_t value) {
    *(volatile uint32_t*)((uint64_t)GICC_BASE + offset) = value;
    __asm__ volatile("dmb sy" ::: "memory");
}

static inline uint32_t gicc_read(uint32_t offset) {
    __asm__ volatile("dmb sy" ::: "memory");
    return *(volatile uint32_t*)((uint64_t)GICC_BASE + offset);
}

/* IRQ handler table */
static irq_handler_fn irq_handlers[GIC_MAX_IRQ];

void gic_init(void) {
    uint32_t i;
    uint32_t typer;
    uint32_t num_irqs;

    /* Clear all handler pointers */
    for (i = 0; i < GIC_MAX_IRQ; i++) {
        irq_handlers[i] = NULL;
    }

    /* Disable distributor while configuring */
    gicd_write(GICD_CTLR, 0);

    /* Get number of interrupt lines */
    typer = gicd_read(GICD_TYPER);
    num_irqs = ((typer & 0x1F) + 1) * 32;
    if (num_irqs > GIC_MAX_IRQ) num_irqs = GIC_MAX_IRQ;

    /* Disable all interrupts */
    for (i = 0; i < num_irqs / 32; i++) {
        gicd_write(GICD_ICENABLER + i * 4, 0xFFFFFFFF);
    }

    /* Clear all pending interrupts */
    for (i = 0; i < num_irqs / 32; i++) {
        gicd_write(GICD_ICPENDR + i * 4, 0xFFFFFFFF);
    }

    /* Set all interrupts to lowest priority (0xA0) */
    for (i = 0; i < num_irqs / 4; i++) {
        gicd_write(GICD_IPRIORITYR + i * 4, 0xA0A0A0A0);
    }

    /* Target all SPIs to CPU0 */
    for (i = GIC_SPI_START / 4; i < num_irqs / 4; i++) {
        gicd_write(GICD_ITARGETSR + i * 4, 0x01010101);
    }

    /* Configure all SPIs as level-triggered */
    for (i = GIC_SPI_START / 16; i < num_irqs / 16; i++) {
        gicd_write(GICD_ICFGR + i * 4, 0);
    }

    /* Enable distributor */
    gicd_write(GICD_CTLR, 1);

    /* Configure CPU interface */
    /* Set priority mask to allow all priorities */
    gicc_write(GICC_PMR, 0xFF);

    /* Enable CPU interface */
    gicc_write(GICC_CTLR, 1);
}

void gic_enable_irq(uint32_t irq) {
    if (irq >= GIC_MAX_IRQ) return;
    uint32_t reg = irq / 32;
    uint32_t bit = irq % 32;
    gicd_write(GICD_ISENABLER + reg * 4, 1 << bit);
}

void gic_disable_irq(uint32_t irq) {
    if (irq >= GIC_MAX_IRQ) return;
    uint32_t reg = irq / 32;
    uint32_t bit = irq % 32;
    gicd_write(GICD_ICENABLER + reg * 4, 1 << bit);
}

void gic_set_priority(uint32_t irq, uint8_t priority) {
    if (irq >= GIC_MAX_IRQ) return;
    uint32_t reg = irq / 4;
    uint32_t offset = (irq % 4) * 8;
    uint32_t val = gicd_read(GICD_IPRIORITYR + reg * 4);
    val &= ~(0xFF << offset);
    val |= (priority << offset);
    gicd_write(GICD_IPRIORITYR + reg * 4, val);
}

void gic_set_target(uint32_t irq, uint8_t cpu_mask) {
    if (irq < GIC_SPI_START || irq >= GIC_MAX_IRQ) return;
    uint32_t reg = irq / 4;
    uint32_t offset = (irq % 4) * 8;
    uint32_t val = gicd_read(GICD_ITARGETSR + reg * 4);
    val &= ~(0xFF << offset);
    val |= (cpu_mask << offset);
    gicd_write(GICD_ITARGETSR + reg * 4, val);
}

void gic_register_handler(uint32_t irq, irq_handler_fn handler) {
    if (irq < GIC_MAX_IRQ) {
        irq_handlers[irq] = handler;
    }
}

uint32_t gic_acknowledge(void) {
    return gicc_read(GICC_IAR) & 0x3FF;
}

void gic_end_interrupt(uint32_t irq) {
    gicc_write(GICC_EOIR, irq);
}

void enable_interrupts(void) {
    __asm__ volatile("msr daifclr, #2" ::: "memory");
}

void disable_interrupts(void) {
    __asm__ volatile("msr daifset, #2" ::: "memory");
}

/* Called from vectors.S on IRQ */
void irq_handler(void) {
    uint32_t irq = gic_acknowledge();

    /* Spurious interrupt? */
    if (irq >= 1020) {
        return;
    }

    /* Call registered handler */
    if (irq < GIC_MAX_IRQ && irq_handlers[irq] != NULL) {
        irq_handlers[irq](irq);
    }

    /* Signal end of interrupt */
    gic_end_interrupt(irq);
}
