#pragma once

#include "scheduler.h"
#include "stdint.h"

int sd_task_register(scheduler_t *sched);
int sd_task_request_read_dump(uint32_t lba, uint32_t count);
int sd_task_request_test(void);
