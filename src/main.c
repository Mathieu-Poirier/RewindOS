#include "../include/stdint.h"

extern void enable_gpioa1(void);
extern void enable_usart1_clock(void);
extern void pa9_to_tx(void);

extern void usart1_init_tx(unsigned int brr);
extern void usart1_putc(char c);

static void delay(volatile uint32_t n)
{
        while (n--)
        {
                __asm volatile("nop");
        }
}

int main(void)
{
        // 1) Enable clocks for GPIOA + USART1
        enable_gpioa1();
        enable_usart1_clock();

        // 2) Configure PA9 = AF7 (USART1_TX)
        pa9_to_tx();

        // 3) Init USART1 TX at 115200 assuming PCLK2 = 16 MHz (HSI)
        usart1_init_tx(139);

        // 4) Prove TX works: send a repeating pattern
        while (1)
        {
                usart1_putc('U'); // 0x55 pattern is easy to see on a scope too
                delay(800000);
        }
}
