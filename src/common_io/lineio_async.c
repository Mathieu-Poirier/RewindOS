#include "../../include/lineio_async.h"
#include "../../include/uart_async.h"
#include "../../include/console.h"

#define EOL_STATE_NONE           0u
#define EOL_STATE_AFTER_CR       1u
#define EOL_STATE_AFTER_CRCR     2u

static void erase_one(void)
{
    console_putc('\b');
    console_putc(' ');
    console_putc('\b');
}

static void shell_redraw_line(shell_state_t *state, const char *s, unsigned int n)
{
    unsigned int old_len = state->len;

    if (n > (SHELL_LINE_MAX - 1u)) {
        n = SHELL_LINE_MAX - 1u;
    }
    for (unsigned int i = 0u; i < n; i++) {
        state->line[i] = s[i];
    }
    state->line[n] = '\0';
    state->len = n;

    /* Repaint whole input row so prompt/history stay correct even if
     * background output arrived while user is editing. */
    console_putc('\r');
    console_puts(state->prompt_str);
    if (state->len > 0u) {
        console_write(state->line, (uint16_t)state->len);
    }
    if (old_len > state->len) {
        unsigned int pad = old_len - state->len;
        for (unsigned int i = 0u; i < pad; i++) {
            console_putc(' ');
        }
        console_putc('\r');
        console_puts(state->prompt_str);
        if (state->len > 0u) {
            console_write(state->line, (uint16_t)state->len);
        }
    }
}

static unsigned int shell_hist_recent_index(const shell_state_t *state, unsigned int nav_index)
{
    int idx = (int)state->hist_head - 1 - (int)nav_index;
    while (idx < 0) {
        idx += SHELL_HISTORY_MAX;
    }
    return (unsigned int)idx;
}

static void shell_history_push(shell_state_t *state, const char *line, unsigned int len)
{
    if (len == 0u) {
        return;
    }
    if (len > (SHELL_LINE_MAX - 1u)) {
        len = SHELL_LINE_MAX - 1u;
    }

    if (state->hist_count > 0u) {
        unsigned int last = shell_hist_recent_index(state, 0u);
        unsigned int i = 0u;
        while (i < len && state->history[last][i] == line[i]) {
            i++;
        }
        if (i == len && state->history[last][i] == '\0') {
            return;
        }
    }

    char *dst = state->history[state->hist_head];
    for (unsigned int i = 0u; i < len; i++) {
        dst[i] = line[i];
    }
    dst[len] = '\0';

    state->hist_head = (state->hist_head + 1u) % SHELL_HISTORY_MAX;
    if (state->hist_count < SHELL_HISTORY_MAX) {
        state->hist_count++;
    }
}

static void shell_history_up(shell_state_t *state)
{
    if (state->hist_count == 0u) {
        return;
    }

    if (state->nav_index < 0) {
        for (unsigned int i = 0u; i < state->len; i++) {
            state->nav_scratch[i] = state->line[i];
        }
        state->nav_scratch[state->len] = '\0';
        state->nav_scratch_len = state->len;
        state->nav_index = 0;
    } else if ((unsigned int)(state->nav_index + 1) < state->hist_count) {
        state->nav_index++;
    }

    unsigned int idx = shell_hist_recent_index(state, (unsigned int)state->nav_index);
    unsigned int n = 0u;
    while (state->history[idx][n] != '\0' && n < (SHELL_LINE_MAX - 1u)) {
        n++;
    }
    shell_redraw_line(state, state->history[idx], n);
}

static void shell_history_down(shell_state_t *state)
{
    if (state->nav_index < 0) {
        return;
    }

    if (state->nav_index == 0) {
        shell_redraw_line(state, state->nav_scratch, state->nav_scratch_len);
        state->nav_index = -1;
        return;
    }

    state->nav_index--;
    unsigned int idx = shell_hist_recent_index(state, (unsigned int)state->nav_index);
    unsigned int n = 0u;
    while (state->history[idx][n] != '\0' && n < (SHELL_LINE_MAX - 1u)) {
        n++;
    }
    shell_redraw_line(state, state->history[idx], n);
}

static int shell_submit_line(shell_state_t *state, line_dispatch_fn dispatch)
{
    console_puts("\r\n");
    state->line[state->len] = '\0';
    shell_history_push(state, state->line, state->len);
    state->nav_index = -1;
    state->nav_scratch_len = 0u;
    dispatch(state->line);
    state->len = 0;
    return 1;
}

void shell_state_init(shell_state_t *state, const char *prompt_str)
{
    state->len = 0;
    state->hist_head = 0u;
    state->hist_count = 0u;
    state->nav_scratch_len = 0u;
    state->nav_index = -1;
    state->prompt_str = prompt_str;
    state->escape_state = 0;
    state->eol_state = EOL_STATE_NONE;
    console_puts(prompt_str);
}

int shell_tick(shell_state_t *state, line_dispatch_fn dispatch)
{
    int c = uart_async_getc();
    if (c < 0) {
        return 0;
    }

    if (state->eol_state == EOL_STATE_AFTER_CR) {
        if (c == '\n') {
            state->eol_state = EOL_STATE_NONE;
            return 0;
        }
        if (c == '\r') {
            state->eol_state = EOL_STATE_AFTER_CRCR;
            return 0;
        }
        state->eol_state = EOL_STATE_NONE;
    } else if (state->eol_state == EOL_STATE_AFTER_CRCR) {
        if (c == '\n') {
            state->eol_state = EOL_STATE_NONE;
            return 0;
        }
        state->eol_state = EOL_STATE_NONE;
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
        if ((unsigned char)c == 'A') {
            shell_history_up(state);
            return 0;
        }
        if ((unsigned char)c == 'B') {
            shell_history_down(state);
            return 0;
        }
        return 0;
    }

    if ((unsigned char)c == 0x1B) {
        state->eol_state = EOL_STATE_NONE;
        state->escape_state = 1;
        return 0;
    }

    if (c == '\r') {
        state->eol_state = EOL_STATE_AFTER_CR;
        return shell_submit_line(state, dispatch);
    }
    if (c == '\n') {
        state->eol_state = EOL_STATE_NONE;
        return shell_submit_line(state, dispatch);
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
            console_putc((char)c);
        } else {
            console_putc('\a');
        }
    }

    return 0;
}

void shell_rx_idle(shell_state_t *state)
{
    if (state == 0) {
        return;
    }
    state->eol_state = EOL_STATE_NONE;
}
