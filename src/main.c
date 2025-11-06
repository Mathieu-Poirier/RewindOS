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

int main(void)
{
        systick_init(10000);
        while (1)
        {

        }
        return 0;
}
