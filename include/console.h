#pragma once

#include "stdint.h"
#include "scheduler.h"

int console_task_register(scheduler_t *sched);

int console_putc(char c);
int console_puts(const char *s);
int console_write(const char *s, uint16_t len);
int console_put_u32(uint32_t v);
int console_put_hex8(uint8_t v);
int console_put_hex32(uint32_t v);

#define CONSOLE_SINK_UART 0u
#define CONSOLE_SINK_LOG  1u

void    console_set_sink(uint8_t sink);
uint8_t console_get_sink(void);
