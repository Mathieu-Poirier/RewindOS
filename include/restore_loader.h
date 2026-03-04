#pragma once

#include "stdint.h"
#include "scheduler.h"
#include "checkpoint_v2.h"

enum {
    RESTORE_LOADER_OK = 0,
    RESTORE_LOADER_ERR_PARAM = -1,
    RESTORE_LOADER_ERR_DESCRIPTOR = -2,
    RESTORE_LOADER_ERR_VERSION = -3,
    RESTORE_LOADER_ERR_SIZE = -4,
    RESTORE_LOADER_ERR_REGISTER = -5,
    RESTORE_LOADER_ERR_RESTORE = -6
};

typedef struct {
    uint32_t calls;
    uint32_t applied;
    uint32_t skipped;
    uint32_t failed;
    int32_t last_rc;
} restore_loader_stats_t;

int restore_loader_apply_regions(scheduler_t *sched,
                                 const checkpoint_v2_region_t *regions,
                                 uint16_t region_count,
                                 const uint8_t *payload_base,
                                 uint32_t payload_len,
                                 uint32_t *out_applied,
                                 uint32_t *out_skipped,
                                 uint32_t *out_failed);
void restore_loader_get_stats(restore_loader_stats_t *out);
