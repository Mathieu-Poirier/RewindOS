#pragma once
#include "stdint.h"
void uart_init(uint32_t pclk2_hz, uint32_t brr);
void uart_putc(char c);
char uart_getc(void);
void uart_puts(const char *s);
void uart_flush_tx(void);
void uart_put_hex32(uint32_t v);
void uart_put_u32(uint32_t v);
