#pragma once

#include "stdint.h"
#include "scheduler.h"

#define RESTORE_SIM_MAX_ITEMS 8u
#define RESTORE_SIM_MAX_BLOB 512u

int restore_sim_reset(void);
int restore_sim_enqueue(uint16_t region_id, uint16_t state_version, const void *blob, uint32_t len);
int restore_sim_apply(scheduler_t *sched, uint32_t *out_applied, uint32_t *out_skipped, uint32_t *out_failed);
uint32_t restore_sim_pending(void);
uint32_t restore_sim_generation(void);
