#include "../include/stdint.h"
#include "../include/isr.h"
#include "../include/clock.h"
#include "../include/gpio.h"
#include "../include/uart.h"

extern int _sdata, _edata, _sbss, _ebss;


int main(void)
{
        enable_gpio_clock();
        uart_init(139);
        while (1)
        {
                char c = uart_getc(); /* wait for RX */

                /* Optional CR → CRLF handling */
                if (c == '\r')
                        uart_putc('\n');

                uart_putc(c); /* echo back */
        }
        return 0;
}
