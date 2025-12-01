#ifndef ISR_H
#define ISR_H

#include "stdint.h"

/* Global tick counter */
extern volatile uint32_t g_ticks;

/* SysTick interrupt handler */
void SysTick_Handler(void);

#endif
