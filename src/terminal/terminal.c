#include "../../include/lineio.h"
#include "../../include/parse.h"
#include "../../include/uart.h"
#include "../../include/systick.h"

#define MAX_ARGUMENTS 8
#define TICKS_PER_SEC 1000u

static void term_dispatch(char *line)
{
        char *argv[MAX_ARGUMENTS];
        int argc = tokenize(line, argv, MAX_ARGUMENTS);

        if (argc == 0)
                return;

        /* simple built-ins */
        if (streq(argv[0], "help"))
        {
                uart_puts("help\r\n");
                uart_puts("echo <text>\r\n");
                uart_puts("ticks\r\n");
                uart_puts("uptime\r\n");
                uart_puts("md <addr> [n]\r\n");
                uart_puts("reboot\r\n");
                return;
        }

        if (streq(argv[0], "echo"))
        {
                /* echo argv[1..] separated by spaces */
                for (int i = 1; i < argc; i++)
                {
                        uart_puts(argv[i]);
                        if (i + 1 < argc)
                                uart_putc(' ');
                }
                uart_puts("\r\n");
                return;
        }

        if (streq(argv[0], "reboot"))
        {
                uart_puts("rebooting...\r\n");
                uart_flush_tx();
                /* System reset: AIRCR */
                volatile uint32_t *AIRCR = (uint32_t *)0xE000ED0Cu;
                const uint32_t VECTKEY = 0x5FAu << 16;
                *AIRCR = VECTKEY | (1u << 2); /* SYSRESETREQ */

                for (;;)
                {
                }
        }

        if (streq(argv[0], "ticks"))
        {
                uint32_t t = systick_now();
                uart_puts("ticks=");
                uart_put_u32(t);
                uart_puts("\r\n");
                return;
        }

        if (streq(argv[0], "uptime"))
        {
                uint32_t t = systick_now();
                uint32_t ms = (t * 1000u) / TICKS_PER_SEC;
                uart_puts("uptime_ms=");
                uart_put_u32(ms);
                uart_puts("\r\n");
                return;
        }

        if (streq(argv[0], "md"))
        {
                if (argc < 2)
                {
                        uart_puts("usage: md <addr> [n]\r\n");
                        return;
                }

                uint32_t addr, n = 1;
                if (!parse_u32(argv[1], &addr))
                {
                        uart_puts("md: bad addr\r\n");
                        return;
                }
                if (argc >= 3 && !parse_u32(argv[2], &n))
                {
                        uart_puts("md: bad n\r\n");
                        return;
                }
                if (n == 0)
                        n = 1;
                if (n > 64)
                        n = 64; /* safety cap */

                volatile uint32_t *p = (volatile uint32_t *)addr;

                for (uint32_t i = 0; i < n; i++)
                {
                        uart_put_hex32((uint32_t)(addr + i * 4u));
                        uart_puts(": ");
                        uart_put_hex32(p[i]);
                        uart_puts("\r\n");
                }
                return;
        }

        uart_puts("unknown cmd: ");
        uart_puts(argv[0]);
        uart_puts("\r\n");
}

void terminal_main(void)
{
        shell_loop("rewind> ", term_dispatch);
}
