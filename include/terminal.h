#pragma once

#include "scheduler.h"
#include "stdint.h"

int terminal_task_register(scheduler_t *sched);
int cmd_task_register(scheduler_t *sched);
int terminal_task_register_restore_descriptor(void);
int cmd_task_register_restore_descriptor(void);
void terminal_task_systick_hook(void);
void terminal_ckpt_set_interval_ms(uint32_t interval_ms);
uint32_t terminal_ckpt_get_interval_ms(void);
int terminal_ckpt_load_latest_sd(scheduler_t *sched,
                                 uint32_t *out_applied,
                                 uint32_t *out_skipped,
                                 uint32_t *out_failed,
                                 uint32_t *out_seq);

#define TERM_STDIN_MODE_RAW 1u

int terminal_stdin_acquire(uint8_t owner_ao, uint8_t mode);
int terminal_stdin_release(uint8_t owner_ao);
