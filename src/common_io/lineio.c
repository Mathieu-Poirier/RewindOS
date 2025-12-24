#include "../include/uart.h"
#include "../include/lineio.h"

#define LINE_MAX 128
#define MAX_ARGUMENTS 8

static void prompt(const char *p) { uart_puts(p); }

static void erase_one(void)
{
        uart_putc('\b');
        uart_putc(' ');
        uart_putc('\b');
}

void shell_loop(const char *prompt_str, line_dispatch_fn dispatch)
{
        char line[LINE_MAX];
        unsigned int len = 0;

        prompt(prompt_str);

        for (;;)
        {
                char c = uart_getc();

                /* ignore simple ANSI arrows */
                if ((unsigned char)c == 0x1B)
                {
                        char a = uart_getc();
                        if ((unsigned char)a == '[')
                                (void)uart_getc();
                        continue;
                }

                if (c == '\r' || c == '\n')
                {
                        uart_puts("\r\n");
                        line[len] = '\0';
                        dispatch(line);
                        len = 0;
                        prompt(prompt_str);
                        continue;
                }

                if (c == '\b' || (unsigned char)c == 0x7F)
                {
                        if (len > 0)
                        {
                                len--;
                                erase_one();
                        }
                        continue;
                }

                if ((unsigned char)c >= 0x20 && (unsigned char)c <= 0x7E)
                {
                        if (len < (LINE_MAX - 1))
                        {
                                line[len++] = c;
                                uart_putc(c);
                        }
                        else
                        {
                                uart_putc('\a');
                        }
                        continue;
                }
        }
}
