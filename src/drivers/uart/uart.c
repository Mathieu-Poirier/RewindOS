// src/drivers/uart/uart.c
#include "../../../include/uart.h"

extern void enable_usart6_clock(void);
extern void pc6pc7_to_usart6(void);
extern void usart6_init(unsigned int brr);
extern void usart6_putc(char c);
extern char usart6_getc(void);

void uart_init(unsigned int brr)
{
        enable_usart6_clock();
        pc6pc7_to_usart6();
        usart6_init(brr);
}

void uart_putc(char c) { usart6_putc(c); }
char uart_getc(void) { return usart6_getc(); }
