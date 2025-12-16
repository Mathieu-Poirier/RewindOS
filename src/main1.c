/* Some calling considerations that we need */
/* We need to setup: */
/* - SysTick Management */
/* - Dynamic Memory */
/* - Peripherials like UART needed for terminal */
/* - Task stacks and scheduler structure */
/* - Watchdog for safe resetting */


#include "../include/stdint.h"
#include "../include/isr.h"

extern int _sdata, _edata, _sbss, _ebss;
extern void systick_init(uint32_t ticks);
extern void hse_clock_init(void);
extern void switch_to_pll_clock(void);
extern void pll_init(void);
extern void pll_enable(void);
extern void flash_latency_init(void);
extern void set_bus_prescalers(void);
extern void enable_gpioa1(void);
extern void enable_usart1_clock(void);
extern void pa9_to_tx(void);
extern void usart1_init_tx(unsigned int brr);
extern void usart1_putc(char c);

static uint32_t last = 0;

int main(void)
{
        // Clock management
        hse_clock_init();
        flash_latency_init();
        set_bus_prescalers();
        pll_init();
        pll_enable();
        switch_to_pll_clock();

        // Enable onboard peripheral clocks
        enable_gpioa1();
        enable_usart1_clock();
        pa9_to_tx();

        

        systick_init(215999);
        while (1)
        {
                if (g_ticks != last && g_ticks % 1000 == 0)
                {
                        last = g_ticks;
                        // checking for interupts
                }
        }
        return 0;
}
