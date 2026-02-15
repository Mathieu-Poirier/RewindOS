#include "../../include/lineio_async.h"
#include "../../include/uart_async.h"

static void erase_one(void)
{
    uart_async_putc('\b');
    uart_async_putc(' ');
    uart_async_putc('\b');
}

void shell_state_init(shell_state_t *state, const char *prompt_str)
{
    state->len = 0;
    state->prompt_str = prompt_str;
    state->escape_state = 0;
    uart_async_puts(prompt_str);
}

int shell_tick(shell_state_t *state, line_dispatch_fn dispatch)
{
    int c = uart_async_getc();
    if (c < 0) {
        return 0;
    }

    if (state->escape_state == 1) {
        if ((unsigned char)c == '[') {
            state->escape_state = 2;
        } else {
            state->escape_state = 0;
        }
        return 0;
    }

    if (state->escape_state == 2) {
        state->escape_state = 0;
        return 0;
    }

    if ((unsigned char)c == 0x1B) {
        state->escape_state = 1;
        return 0;
    }

    if (c == '\r' || c == '\n') {
        uart_async_puts("\r\n");
        state->line[state->len] = '\0';
        dispatch(state->line);
        state->len = 0;
        uart_async_puts(state->prompt_str);
        return 1;
    }

    if (c == '\b' || (unsigned char)c == 0x7F) {
        if (state->len > 0) {
            state->len--;
            erase_one();
        }
        return 0;
    }

    if ((unsigned char)c >= 0x20 && (unsigned char)c <= 0x7E) {
        if (state->len < (SHELL_LINE_MAX - 1)) {
            state->line[state->len++] = (char)c;
            uart_async_putc((char)c);
        } else {
            uart_async_putc('\a');
        }
    }

    return 0;
}
