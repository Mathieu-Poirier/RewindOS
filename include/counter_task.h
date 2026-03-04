#pragma once

#include "scheduler.h"
#include "stdint.h"
#include "restorable_envelope.h"

typedef struct {
    uint8_t active;
    uint8_t bg;
    uint8_t step_pending;
    uint32_t limit;
    uint32_t value;
    uint32_t next_tick;
} counter_task_state_t;

enum {
    COUNTER_RESTORE_ACTION_SET_STATE = 1,
    COUNTER_RESTORE_ACTION_APPLY_OP = 2
};

enum {
    COUNTER_RESTORE_OP_ADD = 1,
    COUNTER_RESTORE_OP_SUB = 2,
    COUNTER_RESTORE_OP_MUL = 3,
    COUNTER_RESTORE_OP_DIV = 4
};

typedef struct {
    uint8_t op;
    uint8_t reserved[3];
    uint32_t operand;
} counter_restore_op_t;

int counter_task_register(scheduler_t *sched);
int counter_task_request_start(uint32_t limit);
void counter_task_systick_hook(void);
int counter_task_register_restore_descriptor(void);
void counter_task_restore_rebind_stdin_if_needed(void);

/* State helpers for checkpoint/restore plumbing */
int counter_task_get_state(counter_task_state_t *out);
int counter_task_restore_state(const counter_task_state_t *in);
void counter_task_reset_state(void);
int counter_task_encode_restore_envelope(const counter_task_state_t *state,
                                         const counter_restore_op_t *ops,
                                         uint16_t op_count,
                                         void *out,
                                         uint32_t *io_len);
