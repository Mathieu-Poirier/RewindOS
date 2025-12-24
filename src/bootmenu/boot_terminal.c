#include "../include/lineio.h"
#include "../include/parse.h"
#include "../include/uart.h"
#include "../include/jump.h"

#define APP_BASE 0x08020000u
#define MAX_ARGUMENTS 8

static void boot_dispatch(char *line)
{
        char *argv[MAX_ARGUMENTS];
        int argc = tokenize(line, argv, MAX_ARGUMENTS);

        if (argc == 0)
                return;

        if (streq(argv[0], "help"))
        {
                uart_puts("help\r\n");
                uart_puts("bootfast\r\n");
                return;
        }

        if (streq(argv[0], "bootfast"))
        {
                uart_puts("booting...\r\n");
                uart_flush_tx();
                jump_to_image(APP_BASE);
                for (;;)
                {
                }
        }

        uart_puts("unknown cmd: ");
        uart_puts(argv[0]);
        uart_puts("\r\n");
}

void boot_main(void)
{
        shell_loop("boot> ", boot_dispatch);
}
