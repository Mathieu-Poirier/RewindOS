#include "../../include/panic.h"
#include "../../include/uart.h"

static volatile uint32_t g_in_panic;

void kernel_panic(const char *msg, const char *file, uint32_t line)
{
    __asm__ volatile("cpsid i" ::: "memory");

    if (g_in_panic == 0u) {
        g_in_panic = 1u;
        uart_puts("\r\nPANIC: ");
        if (msg != 0) {
            uart_puts(msg);
        } else {
            uart_puts("(no message)");
        }
        uart_puts("\r\nat ");
        if (file != 0) {
            uart_puts(file);
        } else {
            uart_puts("(unknown)");
        }
        uart_putc(':');
        uart_put_u32(line);
        uart_puts("\r\n");
        uart_flush_tx();
    }

    for (;;) {
        __asm__ volatile("bkpt #0");
        __asm__ volatile("wfi");
    }
}
