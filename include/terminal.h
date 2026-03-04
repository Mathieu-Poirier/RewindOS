#pragma once

#include "scheduler.h"
#include "stdint.h"

int terminal_task_register(scheduler_t *sched);
int cmd_task_register(scheduler_t *sched);
int terminal_task_register_restore_descriptor(void);
int cmd_task_register_restore_descriptor(void);

#define TERM_STDIN_MODE_RAW 1u

int terminal_stdin_acquire(uint8_t owner_ao, uint8_t mode);
int terminal_stdin_release(uint8_t owner_ao);
