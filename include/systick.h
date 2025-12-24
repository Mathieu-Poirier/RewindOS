#pragma once

#include "stdint.h"

/* Global tick counter */
extern volatile uint32_t g_ticks;

/* SysTick interrupt handler */
void SysTick_Handler(void);
uint32_t systick_now(void);
