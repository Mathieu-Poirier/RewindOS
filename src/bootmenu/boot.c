#include "../include/boot.h"
#include "../include/uart.h"
#include "../include/bump.h"

#define LINE_MAX 128
#define MAX_ARGUMENTS 8

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

static int streq(const char *a, const char *b)
{
        while (*a && *b)
        {
                if (*a != *b)
                        return 0;
                a++;
                b++;
        }
        return (*a == 0 && *b == 0);
}

static int tokenize(char *line, char **argv, int argv_max)
{
        char *p = line;
        int argc = 0;

        while (*p)
        {
                /* skip leading spaces */
                while (*p == ' ' || *p == '\t')
                {
                        p++;
                }

                if (*p == '\0')
                        break;

                /* record start of token */
                if (argc < argv_max)
                {
                        argv[argc++] = p;
                }

                /* scan until space or end */
                while (*p && *p != ' ' && *p != '\t')
                {
                        p++;
                }

                /* terminate token */
                if (*p)
                {
                        *p = '\0';
                        p++;
                }
        }

        return argc;
}

static void dispatch_line(char *line)
{
        char *argv[MAX_ARGUMENTS];
        int argc;

        argc = tokenize(line, argv, MAX_ARGUMENTS);

        if (argc == 0)
                return;

        if (streq(argv[0], "help"))
        {
                uart_puts("help\r\n");
                return;
        }

        uart_puts("unknown cmd: ");
        uart_puts(argv[0]);
        uart_puts("\r\n");
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

                        dispatch_line(line);

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
