/*
 * ARM GIC-400 (Generic Interrupt Controller) definitions
 * For Android emulator ARM64
 */

#ifndef GIC_H
#define GIC_H

#include "types.h"

/* GIC-400 base addresses for Android emulator */
#define GICD_BASE       0x08000000  /* Distributor */
#define GICC_BASE       0x08010000  /* CPU Interface */

/* Distributor registers (GICD) */
#define GICD_CTLR           0x000   /* Control register */
#define GICD_TYPER          0x004   /* Interrupt controller type */
#define GICD_IIDR           0x008   /* Implementer identification */
#define GICD_IGROUPR        0x080   /* Interrupt group registers */
#define GICD_ISENABLER      0x100   /* Interrupt set-enable registers */
#define GICD_ICENABLER      0x180   /* Interrupt clear-enable registers */
#define GICD_ISPENDR        0x200   /* Interrupt set-pending registers */
#define GICD_ICPENDR        0x280   /* Interrupt clear-pending registers */
#define GICD_ISACTIVER      0x300   /* Interrupt set-active registers */
#define GICD_ICACTIVER      0x380   /* Interrupt clear-active registers */
#define GICD_IPRIORITYR     0x400   /* Interrupt priority registers */
#define GICD_ITARGETSR      0x800   /* Interrupt processor targets */
#define GICD_ICFGR          0xC00   /* Interrupt configuration registers */

/* CPU Interface registers (GICC) */
#define GICC_CTLR           0x000   /* CPU interface control */
#define GICC_PMR            0x004   /* Interrupt priority mask */
#define GICC_BPR            0x008   /* Binary point register */
#define GICC_IAR            0x00C   /* Interrupt acknowledge register */
#define GICC_EOIR           0x010   /* End of interrupt register */
#define GICC_RPR            0x014   /* Running priority register */
#define GICC_HPPIR          0x018   /* Highest priority pending interrupt */
#define GICC_AIAR           0x020   /* Aliased interrupt acknowledge */
#define GICC_AEOIR          0x024   /* Aliased end of interrupt */

/* Interrupt IDs */
#define GIC_SGI_START       0       /* Software Generated Interrupts 0-15 */
#define GIC_PPI_START       16      /* Private Peripheral Interrupts 16-31 */
#define GIC_SPI_START       32      /* Shared Peripheral Interrupts 32+ */

/* Virtio MMIO interrupts start at SPI 32 (IRQ 32+32 = 64 in GIC terms) */
#define VIRTIO_IRQ_BASE     (GIC_SPI_START + 16)  /* IRQ 48 */
#define VIRTIO_GPU_IRQ      (VIRTIO_IRQ_BASE + 0)
#define VIRTIO_INPUT_IRQ    (VIRTIO_IRQ_BASE + 1)

/* Maximum supported interrupts */
#define GIC_MAX_IRQ         256

/* IRQ handler function type */
typedef void (*irq_handler_fn)(uint32_t irq);

/* Initialize the GIC */
void gic_init(void);

/* Enable a specific interrupt */
void gic_enable_irq(uint32_t irq);

/* Disable a specific interrupt */
void gic_disable_irq(uint32_t irq);

/* Set interrupt priority (0 = highest, 255 = lowest) */
void gic_set_priority(uint32_t irq, uint8_t priority);

/* Set interrupt target CPU(s) (bitmask) */
void gic_set_target(uint32_t irq, uint8_t cpu_mask);

/* Register an IRQ handler */
void gic_register_handler(uint32_t irq, irq_handler_fn handler);

/* Acknowledge interrupt (returns IRQ number) */
uint32_t gic_acknowledge(void);

/* Signal end of interrupt */
void gic_end_interrupt(uint32_t irq);

/* Enable interrupts globally (unmask DAIF) */
void enable_interrupts(void);

/* Disable interrupts globally (mask DAIF) */
void disable_interrupts(void);

#endif /* GIC_H */
