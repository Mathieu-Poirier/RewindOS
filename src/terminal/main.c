#include "../../include/gpio.h"
#include "../../include/uart.h"
#include "../../include/clock.h"
#include "../../include/stdint.h"

extern void terminal_main(void);
extern void systick_init(uint32_t ticks);

int main(void)
{
        full_clock_init();
        enable_gpio_clock();
        uart_init(108000000u, 115200u);
        systick_init(215999);
        terminal_main();
        for (;;)
        {
        }
}
