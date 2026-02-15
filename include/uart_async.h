#pragma once

#include "stdint.h"
#include "driver_common.h"

#define UART_TX_BUF_SIZE 256
#define UART_RX_BUF_SIZE 128

typedef struct {
    volatile uint8_t  *tx_buf;
    volatile uint16_t  tx_head;
    volatile uint16_t  tx_tail;
    volatile uint16_t  tx_size;
    volatile drv_status_t tx_status;

    volatile uint8_t  *rx_buf;
    volatile uint16_t  rx_head;
    volatile uint16_t  rx_tail;
    volatile uint16_t  rx_size;
    volatile drv_status_t rx_status;

    volatile uint32_t  rx_overrun_count;
} uart_context_t;

extern uart_context_t g_uart_ctx;

void uart_async_init(void);
int  uart_async_putc(char c);
int  uart_async_puts(const char *s);
int  uart_async_write(const uint8_t *data, uint16_t n);
int  uart_async_getc(void);
int  uart_async_read(uint8_t *data, uint16_t max_n);
int  uart_tx_done(void);
int  uart_rx_available(void);

void USART6_IRQHandler(void);
