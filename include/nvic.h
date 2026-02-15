#pragma once

#include "stdint.h"

#define USART6_IRQn  71
#define SDMMC1_IRQn  49

void nvic_enable_irq(uint32_t irqn);
void nvic_disable_irq(uint32_t irqn);
void nvic_set_priority(uint32_t irqn, uint8_t priority);
void nvic_clear_pending(uint32_t irqn);
