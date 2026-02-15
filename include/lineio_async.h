#pragma once

#include "lineio.h"

#define SHELL_LINE_MAX 128

typedef struct {
    char line[SHELL_LINE_MAX];
    unsigned int len;
    const char *prompt_str;
    int escape_state;
} shell_state_t;

void shell_state_init(shell_state_t *state, const char *prompt_str);
int  shell_tick(shell_state_t *state, line_dispatch_fn dispatch);
