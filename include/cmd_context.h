#pragma once

#include "stdint.h"

/*
 * Globals set by cmd_task_dispatch while a command is executing.
 * Read by sd_task_request_* to propagate the background flag into
 * the SD request event so the SD task knows where to route output.
 *
 * These are only accessed from task dispatch context (main thread).
 */

extern uint8_t g_cmd_bg_ctx;    /* 1 while CMD is dispatching a bg command  */
extern uint8_t g_cmd_bg_async;  /* set by sd_task_request_* when posting bg */
extern uint8_t g_cmd_fg_async;  /* set by sd_task_request_* when posting fg */
