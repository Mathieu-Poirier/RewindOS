#pragma once

#include "stdint.h"
#include "event.h"
#include "task_spec.h"

#define SCHED_MAX_AO 32u

typedef struct {
    uint16_t task_id;
    uint8_t prio;
    uint8_t flags;
    ao_dispatch_fn run;
    void *ctx;
    const char *name;
} task_desc_t;

struct ao {
    uint8_t id;
    uint8_t prio;
    uint16_t reserved;
    event_queue_t q;
    ao_dispatch_fn dispatch;
    void *state;
    volatile uint32_t flags;
    volatile uint32_t rtc_max_ticks;
    volatile uint32_t events_handled;
};

typedef struct {
    ao_t *table[SCHED_MAX_AO];          /* indexed by AO id */
    volatile uint32_t ready_bitmap;     /* bit(id)=queue non-empty */
    uint8_t rr_cursor[SCHED_MAX_AO];    /* indexed by priority */
    void (*idle_hook)(void);
} scheduler_t;

enum {
    SCHED_OK = 0,
    SCHED_ERR_PARAM = -1,
    SCHED_ERR_FULL = -2,
    SCHED_ERR_EXISTS = -3,
    SCHED_ERR_NOT_FOUND = -4,
    SCHED_ERR_QUEUE_FULL = -5,
    SCHED_ERR_DISABLED = -6
};

enum {
    AO_FLAG_ACCEPT_EVENTS = (1u << 0)
};

void sched_init(scheduler_t *s, void (*idle_hook)(void));
int  sched_register(scheduler_t *s, ao_t *ao);
int  sched_register_task(scheduler_t *s, const task_spec_t *spec);
int  sched_unregister(scheduler_t *s, uint8_t ao_id);

int  sched_post(scheduler_t *s, uint8_t ao_id, const event_t *e);
int  sched_post_isr(scheduler_t *s, uint8_t ao_id, const event_t *e);

int  sched_pause_accept(scheduler_t *s, uint8_t ao_id);
int  sched_resume_accept(scheduler_t *s, uint8_t ao_id);

void sched_run(scheduler_t *s);
