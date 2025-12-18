#include "../include/stdint.h"

/* GPIO + USART6 bring-up */
extern void enable_gpioc(void);
extern void enable_usart6_clock(void);
extern void pc6pc7_to_usart6(void);

/* USART6 driver */
extern void usart6_init(unsigned int brr);
extern void usart6_putc(char c);
extern char usart6_getc(void);

int main(void)
{
        /* 1) Enable clocks */
        enable_gpioc();
        enable_usart6_clock();

        /* 2) Configure PC6 (TX) / PC7 (RX) as USART6 AF8 */
        pc6pc7_to_usart6();

        /*
         * 3) Init USART6
         * PCLK2 = 16 MHz
         * 16_000_000 / 115200 ≈ 139
         */
        usart6_init(139);

        /* 4) Startup banner */
        usart6_putc('\r');
        usart6_putc('\n');
        usart6_putc('>');
        usart6_putc(' ');
        usart6_putc('R');
        usart6_putc('e');
        usart6_putc('a');
        usart6_putc('d');
        usart6_putc('y');
        usart6_putc('\r');
        usart6_putc('\n');

        /* 5) Echo loop */
        while (1)
        {
                char c = usart6_getc(); /* wait for RX */

                /* Optional CR → CRLF handling */
                if (c == '\r')
                        usart6_putc('\n');

                usart6_putc(c); /* echo back */
        }
}
