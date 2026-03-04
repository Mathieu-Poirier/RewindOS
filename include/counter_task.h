#pragma once

#include "scheduler.h"
#include "stdint.h"

typedef struct {
    uint8_t active;
    uint8_t bg;
    uint8_t step_pending;
    uint32_t limit;
    uint32_t value;
    uint32_t next_tick;
} counter_task_state_t;

int counter_task_register(scheduler_t *sched);
int counter_task_request_start(uint32_t limit);
void counter_task_systick_hook(void);
int counter_task_register_restore_descriptor(void);

/* State helpers for checkpoint/restore plumbing */
int counter_task_get_state(counter_task_state_t *out);
int counter_task_restore_state(const counter_task_state_t *in);
void counter_task_reset_state(void);
