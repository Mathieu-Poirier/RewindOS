#pragma once

typedef void (*line_dispatch_fn)(char *line);

void shell_loop(const char *prompt_str, line_dispatch_fn dispatch);
