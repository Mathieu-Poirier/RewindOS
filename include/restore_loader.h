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

int restore_loader_apply_regions(scheduler_t *sched,
                                 const checkpoint_v2_region_t *regions,
                                 uint16_t region_count,
                                 const uint8_t *payload_base,
                                 uint32_t payload_len,
                                 uint32_t *out_applied,
                                 uint32_t *out_skipped,
                                 uint32_t *out_failed);
