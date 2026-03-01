#pragma once

#include "scheduler.h"
#include "stdint.h"

int counter_task_register(scheduler_t *sched);
int counter_task_request_start(uint32_t limit);
void counter_task_systick_hook(void);
