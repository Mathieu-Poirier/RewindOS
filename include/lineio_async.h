#pragma once

#include "lineio.h"
#include "stdint.h"

#define SHELL_LINE_MAX 128
#define SHELL_HISTORY_MAX 8

typedef struct {
    char line[SHELL_LINE_MAX];
    char history[SHELL_HISTORY_MAX][SHELL_LINE_MAX];
    char nav_scratch[SHELL_LINE_MAX];
    unsigned int len;
    unsigned int hist_head;
    unsigned int hist_count;
    unsigned int nav_scratch_len;
    int nav_index;
    const char *prompt_str;
    int escape_state;
    uint8_t eol_state;
} shell_state_t;

void shell_state_init(shell_state_t *state, const char *prompt_str);
int  shell_tick(shell_state_t *state, line_dispatch_fn dispatch);
void shell_rx_idle(shell_state_t *state);
