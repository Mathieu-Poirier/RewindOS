#include "../../include/restore_registry.h"

static const restore_task_descriptor_t *g_registry[RESTORE_REGISTRY_MAX_TASKS];

void restore_registry_init(void)
{
    uint32_t i;
    for (i = 0; i < RESTORE_REGISTRY_MAX_TASKS; i++) {
        g_registry[i] = 0;
    }
}

int restore_registry_register_descriptor(const restore_task_descriptor_t *desc)
{
    if (desc == 0) {
        return SCHED_ERR_PARAM;
    }
    if (desc->task_class > TASK_CLASS_NON_RESTORABLE) {
        return SCHED_ERR_PARAM;
    }
    if (desc->min_state_len > desc->max_state_len) {
        return SCHED_ERR_PARAM;
    }
    if (desc->register_fn == 0) {
        return SCHED_ERR_PARAM;
    }
    if (desc->task_class == TASK_CLASS_RESTORABLE_NOW &&
        (desc->get_state_fn == 0 || desc->restore_fn == 0)) {
        return SCHED_ERR_PARAM;
    }
    if (g_registry[desc->task_id] != 0) {
        return SCHED_ERR_EXISTS;
    }

    g_registry[desc->task_id] = desc;
    return SCHED_OK;
}

const restore_task_descriptor_t *restore_registry_find(uint8_t task_id)
{
    return g_registry[task_id];
}
