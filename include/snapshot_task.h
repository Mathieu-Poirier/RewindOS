#pragma once

#include "scheduler.h"
#include "stdint.h"

typedef struct {
    uint8_t enabled;
    uint8_t busy;
    uint8_t restore_mode;
    uint8_t restore_has_candidate;
    uint32_t interval_s;
    uint32_t next_tick;
    uint32_t saves_ok;
    uint32_t saves_err;
    int32_t last_err;
    uint32_t last_slot;
    uint32_t last_seq;
    uint32_t last_capture_tick;
    uint32_t last_ready_bitmap;
    uint32_t restore_slot;
    uint32_t restore_seq;
} snapshot_task_stats_t;

int snapshot_task_register(scheduler_t *sched);
int snapshot_task_boot_init(void);
int snapshot_task_request_now(void);
int snapshot_task_list_slots(void);
void snapshot_task_systick_hook(void);
void snapshot_task_disarm_hook(void);
void snapshot_task_rearm_hook(scheduler_t *sched);
void snapshot_task_get_stats(snapshot_task_stats_t *out);
