#include "../../../include/nvic.h"

#define NVIC_ISER_BASE  0xE000E100
#define NVIC_ICER_BASE  0xE000E180
#define NVIC_ICPR_BASE  0xE000E280
#define NVIC_IPR_BASE   0xE000E400

void nvic_enable_irq(uint32_t irqn)
{
    volatile uint32_t *iser = (volatile uint32_t *)(NVIC_ISER_BASE + (irqn / 32) * 4);
    *iser = (1u << (irqn % 32));
}

void nvic_disable_irq(uint32_t irqn)
{
    volatile uint32_t *icer = (volatile uint32_t *)(NVIC_ICER_BASE + (irqn / 32) * 4);
    *icer = (1u << (irqn % 32));
}

void nvic_set_priority(uint32_t irqn, uint8_t priority)
{
    volatile uint8_t *ipr = (volatile uint8_t *)(NVIC_IPR_BASE + irqn);
    *ipr = (priority << 4);
}

void nvic_clear_pending(uint32_t irqn)
{
    volatile uint32_t *icpr = (volatile uint32_t *)(NVIC_ICPR_BASE + (irqn / 32) * 4);
    *icpr = (1u << (irqn % 32));
}
