#include "../../include/isr.h"

volatile uint32_t g_ticks = 0;

void SysTick_Handler(void)
{
        g_ticks++;
}
