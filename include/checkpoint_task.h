#pragma once

#include "scheduler.h"
#include "stdint.h"

int checkpoint_task_register(scheduler_t *sched);
void checkpoint_task_set_interval_ms(uint32_t interval_ms);
uint32_t checkpoint_task_get_interval_ms(void);
void checkpoint_task_systick_hook(void);
