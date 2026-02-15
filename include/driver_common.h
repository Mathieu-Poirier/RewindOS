#pragma once

#include "stdint.h"

typedef enum {
    DRV_IDLE        = 0,
    DRV_IN_PROGRESS = 1,
    DRV_COMPLETE    = 2,
    DRV_ERROR       = 3
} drv_status_t;
