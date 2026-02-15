#pragma once

#include "stdint.h"
#include "stddef.h"

typedef struct {
    uint16_t sig;
    uint16_t src;
    uintptr_t arg0;
    uintptr_t arg1;
    uint32_t tick;
} event_t;

typedef struct {
    event_t *buf;
    uint16_t cap;
    volatile uint16_t head;
    volatile uint16_t tail;
    volatile uint16_t count;
    volatile uint16_t high_watermark;
    volatile uint32_t dropped;
} event_queue_t;

enum {
    EQ_OK = 0,
    EQ_ERR_PARAM = -1,
    EQ_ERR_FULL = -2,
    EQ_ERR_EMPTY = -3
};

void eq_init(event_queue_t *q, event_t *storage, uint16_t capacity);
int  eq_push(event_queue_t *q, const event_t *e);
int  eq_push_isr(event_queue_t *q, const event_t *e);
int  eq_pop(event_queue_t *q, event_t *out);
int  eq_is_empty(const event_queue_t *q);
void eq_drain(event_queue_t *q);
