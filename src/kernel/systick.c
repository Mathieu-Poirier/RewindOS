#include "../../include/systick.h"
#include "../../include/stdint.h"
#include "../../include/uart.h"

volatile uint32_t g_ticks = 0;

void SysTick_Handler(void)
{
        g_ticks++;
}

uint32_t systick_now(void){
        return g_ticks;
}