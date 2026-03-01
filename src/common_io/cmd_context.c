#include "../../include/cmd_context.h"
#include "../../include/console.h"

uint8_t g_cmd_bg_ctx   = 0u;
uint8_t g_cmd_bg_async = 0u;
uint8_t g_cmd_fg_async = 0u;

__attribute__((weak)) void ui_notify_bg_done(const char *name)
{
    if (name == 0 || name[0] == '\0') {
        name = "?";
    }
    console_puts("[done: ");
    console_puts(name);
    console_puts("]\r\n");
}
