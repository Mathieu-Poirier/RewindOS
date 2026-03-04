#include "../../include/systick.h"
#include "../../include/stdint.h"
#include "../../include/uart.h"
#include "../../include/counter_task.h"
#include "../../include/terminal.h"

volatile uint32_t g_ticks = 0;

void SysTick_Handler(void)
{
        g_ticks++;
        counter_task_systick_hook();
        terminal_task_systick_hook();
}

uint32_t systick_now(void){
        return g_ticks;
}
