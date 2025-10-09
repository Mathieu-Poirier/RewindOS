/* Some calling considerations that we need */
/* We need to setup: */
/* - SysTick Management */
/* - Dynamic Memory */
/* - Peripherials like UART needed for terminal */
/* - Task stacks and scheduler structure */
#include "systick.h"

extern int _sdata, _edata, _sbss, _ebss;

int main(void)
{
        while (1)
        {
                systick_init();
        }
        return 0;
}
