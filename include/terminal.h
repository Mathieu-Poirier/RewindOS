#pragma once

#include "scheduler.h"
#include "stdint.h"

int terminal_task_register(scheduler_t *sched);
int cmd_task_register(scheduler_t *sched);

#define TERM_STDIN_MODE_RAW 1u

int terminal_stdin_acquire(uint8_t owner_ao, uint8_t mode);
int terminal_stdin_release(uint8_t owner_ao);

/* Synchronously drain all pending UART RX bytes through the shell.
 * Used by journal replay to avoid overflowing the small RX ring buffer. */
void terminal_replay_drain(void);
