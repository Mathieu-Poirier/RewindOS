#pragma once

#include "scheduler.h"
#include "stdint.h"

int terminal_task_register(scheduler_t *sched);
int cmd_task_register(scheduler_t *sched);
int terminal_task_register_restore_descriptor(void);
int cmd_task_register_restore_descriptor(void);
int terminal_ckpt_save_sd_once(uint32_t *out_lba,
                               uint32_t *out_slot,
                               uint32_t *out_seq,
                               uint32_t *out_regions);
int terminal_ckpt_load_latest_sd(scheduler_t *sched,
                                 uint32_t *out_applied,
                                 uint32_t *out_skipped,
                                 uint32_t *out_failed,
                                 uint32_t *out_seq);

#define TERM_STDIN_MODE_RAW 1u

int terminal_stdin_acquire(uint8_t owner_ao, uint8_t mode);
int terminal_stdin_release(uint8_t owner_ao);
