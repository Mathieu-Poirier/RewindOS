#include "../../include/gpio.h"
#include "../../include/uart.h"
#include "../../include/uart_async.h"
#include "../../include/sd_async.h"
#include "../../include/bump.h"
#include "../../include/clock.h"
#include "../../include/stdint.h"

extern void terminal_main(void);
extern void terminal_main_async(void);
extern void systick_init(uint32_t ticks);

int main(void)
{
        full_clock_init();
        enable_gpio_clock();
        uart_init(108000000u, 115200u);
        systick_init(215999);

        bump_init();
        uart_async_init();
        sd_async_init();

        terminal_main_async();
        for (;;)
        {
        }
}
