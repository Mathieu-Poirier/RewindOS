#include "../include/boot.h"
#include "../include/uart.h"

#define LINE_MAX 128

static void prompt(void)
{
        uart_puts("boot> ");
}

static void erase_one(void)
{
        /* Move cursor back, overwrite with space, move back again */
        uart_putc('\b');
        uart_putc(' ');
        uart_putc('\b');
}

void boot_main(void)
{
        char line[LINE_MAX];
        unsigned int len = 0;

        prompt();

        for (;;)
        {
                char c = uart_getc();

                /* --- Handle ANSI escape sequences (arrow keys, etc.) by ignoring them --- */
                if ((unsigned char)c == 0x1B)
                {                             /* ESC */
                        char a = uart_getc(); /* usually '[' */
                        if ((unsigned char)a == '[')
                        {
                                /* Consume final byte of CSI (A/B/C/D for arrows, etc.) */
                                (void)uart_getc();
                        }
                        continue;
                }

                /* --- Enter / Return --- */
                if (c == '\r' || c == '\n')
                {
                        uart_puts("\r\n");

                        /* Null-terminate line so you can parse it later */
                        line[len] = '\0';

                        /* TODO: handle command here (for now just echo the line as debug) */
                        /* uart_puts("cmd: "); uart_puts(line); uart_puts("\r\n"); */

                        len = 0;
                        prompt();
                        continue;
                }

                /* --- Backspace (BS=0x08) or DEL=0x7F --- */
                if (c == '\b' || (unsigned char)c == 0x7F)
                {
                        if (len > 0)
                        {
                                len--;
                                erase_one();
                        }
                        continue;
                }

                /* --- Printable characters --- */
                if ((unsigned char)c >= 0x20 && (unsigned char)c <= 0x7E)
                {
                        if (len < (LINE_MAX - 1))
                        {
                                line[len++] = c;
                                uart_putc(c);
                        }
                        else
                        {
                                /* Optional: bell on overflow */
                                uart_putc('\a');
                        }
                        continue;
                }

                /* Ignore everything else (control chars) */
        }
}
