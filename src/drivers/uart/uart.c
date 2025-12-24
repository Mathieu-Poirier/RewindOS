// src/drivers/uart/uart.c
#include "../../../include/uart.h"
#include "../../../include/stdint.h"

extern void enable_usart6_clock(void);
extern void pc6pc7_to_usart6(void);
extern void usart6_init(unsigned int brr);
extern void usart6_putc(char c);
extern char usart6_getc(void);
extern void usart6_flush_tx(void);

void uart_flush_tx(void){
        usart6_flush_tx();
}

static uint32_t usart_brr_from_pclk_baud_ov16(uint32_t pclk_hz, uint32_t baud)
{
        /* Oversampling by 16, round to nearest */
        uint32_t div16 = (pclk_hz + (baud / 2u)) / baud; /* USARTDIV*16 */
        uint32_t mant = div16 / 16u;
        uint32_t frac = div16 % 16u;
        return (mant << 4) | (frac & 0xFu);
}

void uart_init(uint32_t pclk2_hz, uint32_t baud)
{
        uint32_t brr = usart_brr_from_pclk_baud_ov16(pclk2_hz, baud);

        enable_usart6_clock();
        pc6pc7_to_usart6();
        usart6_init((unsigned int)brr);
}

void uart_putc(char c) { usart6_putc(c); }
char uart_getc(void) { return usart6_getc(); }

void uart_puts(const char *s)
{
        if (!s)
                return;
        while (*s)
                uart_putc(*s++);
}

void uart_put_hex32(uint32_t v)
{
        static const char *hx = "0123456789ABCDEF";
        uart_puts("0x");
        for (int i = 7; i >= 0; i--)
                uart_putc(hx[(v >> (i * 4)) & 0xF]);
}

void uart_put_u32(uint32_t v)
{
        char buf[11];
        int i = 0;
        if (v == 0)
        {
                uart_putc('0');
                return;
        }
        while (v && i < 10)
        {
                buf[i++] = (char)('0' + (v % 10u));
                v /= 10u;
        }
        while (i--)
                uart_putc(buf[i]);
}
