#pragma once

#include "stdint.h"
#include "scheduler.h"

#define RESTORE_REGISTRY_MAX_TASKS 256u

enum {
    TASK_CLASS_RESTORABLE_NOW = 0,
    TASK_CLASS_RESTART_ONLY = 1,
    TASK_CLASS_NON_RESTORABLE = 2
};

enum {
    TASK_LIFECYCLE_RUNNING = 0,
    TASK_LIFECYCLE_EXITED = 1,
    TASK_LIFECYCLE_FAILED = 2,
    TASK_LIFECYCLE_STOPPED = 3
};

typedef struct {
    uint8_t program_id;
    uint8_t lifecycle_state;
    uint8_t launch_mode_fg;
    uint8_t owner_session;
    uint32_t instance_id;
    uint32_t launch_seq;
    uint32_t arg0;
    uint32_t arg1;
} launch_intent_t;

typedef int (*restore_task_register_fn)(scheduler_t *sched, const launch_intent_t *intent);
typedef int (*restore_task_get_state_fn)(void *out, uint32_t *io_len);
typedef int (*restore_task_restore_fn)(const void *blob, uint32_t len);
typedef void (*restore_task_ui_rehydrate_fn)(void);

typedef struct {
    uint8_t task_id;
    uint8_t task_class;
    uint16_t state_version;
    uint32_t min_state_len;
    uint32_t max_state_len;
    restore_task_register_fn register_fn;
    restore_task_get_state_fn get_state_fn;
    restore_task_restore_fn restore_fn;
    restore_task_ui_rehydrate_fn ui_rehydrate_fn;
} restore_task_descriptor_t;

void restore_registry_init(void);
int restore_registry_register_descriptor(const restore_task_descriptor_t *desc);
const restore_task_descriptor_t *restore_registry_find(uint8_t task_id);
