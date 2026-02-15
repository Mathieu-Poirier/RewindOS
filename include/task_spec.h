#pragma once

#include "stdint.h"
#include "event.h"

typedef struct ao ao_t;
typedef void (*ao_dispatch_fn)(ao_t *self, const event_t *e);

typedef struct {
    uint8_t id;
    uint8_t prio;
    ao_dispatch_fn dispatch;
    void *ctx;
    event_t *queue_storage;
    uint16_t queue_capacity;
    uint32_t rtc_budget_ticks;
    const char *name;
} task_spec_t;
