/* Some calling considerations that we need */
/* We need to setup: */
/* - SysTick Management */
/* - Dynamic Memory */
/* - Peripherials like UART needed for terminal */
/* - Task stacks and scheduler structure */
/* - Watchdog for safe resetting */


#include "../include/stdint.h"

extern int _sdata, _edata, _sbss, _ebss;
extern void systick_init(uint32_t ticks);

/* Put this handler in isr.c */
volatile uint32_t g_ticks = 0;
static uint32_t last = 0;

void SysTick_Handler(void)
{
        g_ticks++;
}

int main(void)
{
        systick_init(215999);
        while (1)
        {
                if (g_ticks != last && g_ticks % 1000 == 0)
                {
                        last = g_ticks;
                        // breakpoint here
                }
        }
        return 0;
}
