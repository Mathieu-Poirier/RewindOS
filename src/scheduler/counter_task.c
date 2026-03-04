#include "../../include/counter_task.h"
#include "../../include/task_ids.h"
#include "../../include/task_signals.h"
#include "../../include/task_spec.h"
#include "../../include/cmd_context.h"
#include "../../include/console.h"
#include "../../include/log.h"
#include "../../include/restore_registry.h"
#include "../../include/restorable_envelope.h"
#include "../../include/systick.h"
#include "../../include/terminal.h"

#define COUNTER_STEP_TICKS 250u

static counter_task_state_t g_counter_ctx;
static event_t g_counter_queue_storage[8];
static scheduler_t *g_counter_sched;
static uint8_t g_counter_state_restored;

void counter_task_reset_state(void)
{
    g_counter_ctx.active = 0u;
    g_counter_ctx.bg = 0u;
    g_counter_ctx.step_pending = 0u;
    g_counter_ctx.limit = 0u;
    g_counter_ctx.value = 0u;
    g_counter_ctx.next_tick = 0u;
}

int counter_task_get_state(counter_task_state_t *out)
{
    if (out == 0) {
        return SCHED_ERR_PARAM;
    }
    *out = g_counter_ctx;
    return SCHED_OK;
}

int counter_task_restore_state(const counter_task_state_t *in)
{
    if (in == 0) {
        return SCHED_ERR_PARAM;
    }
    g_counter_ctx = *in;
    /* Pending queue entries are not checkpointed. Re-arm from timer logic. */
    g_counter_ctx.step_pending = 0u;
    g_counter_state_restored = 1u;
    return SCHED_OK;
}

int counter_task_encode_restore_envelope(const counter_task_state_t *state,
                                         const counter_restore_op_t *ops,
                                         uint16_t op_count,
                                         void *out,
                                         uint32_t *io_len)
{
    restorable_envelope_t *env = (restorable_envelope_t *)out;
    uint16_t i;

    if (state == 0 || out == 0 || io_len == 0) {
        return SCHED_ERR_PARAM;
    }
    if (op_count > (RESTORE_ENV_MAX_ENTRIES - 1u)) {
        return SCHED_ERR_PARAM;
    }
    if (op_count > 0u && ops == 0) {
        return SCHED_ERR_PARAM;
    }
    if (*io_len < sizeof(restorable_envelope_t)) {
        return SCHED_ERR_PARAM;
    }

    env->hdr.magic = RESTORE_ENV_MAGIC;
    env->hdr.version = RESTORE_ENV_VERSION;
    env->hdr.entry_count = (uint16_t)(1u + op_count);
    env->hdr.read_idx = 0u;
    env->hdr.write_idx = env->hdr.entry_count;
    env->hdr.generation = 1u;
    env->hdr.next_seq = (uint32_t)(env->hdr.entry_count + 1u);
    env->hdr.program_id = AO_COUNTER;
    env->hdr.reserved[0] = 0u;
    env->hdr.reserved[1] = 0u;
    env->hdr.reserved[2] = 0u;

    env->entries[0].seq = 1u;
    env->entries[0].action = COUNTER_RESTORE_ACTION_SET_STATE;
    env->entries[0].entry_state = RESTORE_ENV_ENTRY_PENDING;
    env->entries[0].data_len = (uint16_t)sizeof(counter_task_state_t);
    env->entries[0].reserved = 0u;
    for (i = 0u; i < RESTORE_ENV_ENTRY_DATA_MAX; i++) {
        env->entries[0].data[i] = 0u;
    }
    for (i = 0u; i < sizeof(counter_task_state_t); i++) {
        env->entries[0].data[i] = ((const uint8_t *)state)[i];
    }

    for (i = 0u; i < op_count; i++) {
        uint16_t idx = (uint16_t)(i + 1u);
        env->entries[idx].seq = (uint32_t)(idx + 1u);
        env->entries[idx].action = COUNTER_RESTORE_ACTION_APPLY_OP;
        env->entries[idx].entry_state = RESTORE_ENV_ENTRY_PENDING;
        env->entries[idx].data_len = (uint16_t)sizeof(counter_restore_op_t);
        env->entries[idx].reserved = 0u;
        for (uint16_t j = 0u; j < RESTORE_ENV_ENTRY_DATA_MAX; j++) {
            env->entries[idx].data[j] = 0u;
        }
        for (uint16_t j = 0u; j < sizeof(counter_restore_op_t); j++) {
            env->entries[idx].data[j] = ((const uint8_t *)&ops[i])[j];
        }
    }

    for (i = env->hdr.entry_count; i < RESTORE_ENV_MAX_ENTRIES; i++) {
        env->entries[i].seq = 0u;
        env->entries[i].action = 0u;
        env->entries[i].entry_state = RESTORE_ENV_ENTRY_DONE;
        env->entries[i].data_len = 0u;
        env->entries[i].reserved = 0u;
        for (uint16_t j = 0u; j < RESTORE_ENV_ENTRY_DATA_MAX; j++) {
            env->entries[i].data[j] = 0u;
        }
    }

    *io_len = (uint32_t)sizeof(restorable_envelope_t);
    return SCHED_OK;
}

static void counter_out_puts(const char *s)
{
    if (g_counter_ctx.bg) {
        log_puts(s);
    } else {
        console_puts(s);
    }
}

static void counter_out_put_u32(uint32_t v)
{
    if (g_counter_ctx.bg) {
        log_put_u32(v);
    } else {
        console_put_u32(v);
    }
}

static void counter_task_stop(void)
{
    g_counter_ctx.active = 0u;
    g_counter_ctx.step_pending = 0u;
    if (!g_counter_ctx.bg) {
        (void)terminal_stdin_release(AO_COUNTER);
    }
    if (g_counter_sched != 0) {
        (void)sched_unregister(g_counter_sched, AO_COUNTER);
    }
}

static void counter_task_dispatch(ao_t *self, const event_t *e)
{
    (void)self;
    if (e == 0) {
        return;
    }

    if (e->sig == COUNTER_SIG_START) {
        if (g_counter_ctx.active) {
            console_puts("counter: busy\r\n");
            return;
        }

        g_counter_ctx.limit = (uint32_t)e->arg0;
        if (g_counter_ctx.limit == 0u) {
            g_counter_ctx.limit = 1u;
        }
        g_counter_ctx.value = 1u;
        g_counter_ctx.bg = (uint8_t)(e->src & 1u);
        g_counter_ctx.step_pending = 0u;
        g_counter_ctx.active = 1u;
        g_counter_ctx.next_tick = systick_now() + COUNTER_STEP_TICKS;

        counter_out_puts("counter: start\r\n");
        return;
    }

    if (e->sig == TERM_SIG_STDIN_RAW) {
        /* Foreground lock owner receives raw keys while running.
         * Ctrl-C is handled by terminal task; other keys are ignored. */
        return;
    }

    if (e->sig != COUNTER_SIG_STEP || !g_counter_ctx.active) {
        return;
    }

    counter_out_put_u32(g_counter_ctx.value);
    counter_out_puts("\r\n");

    if (g_counter_ctx.value >= g_counter_ctx.limit) {
        g_counter_ctx.step_pending = 0u;
        counter_out_puts("counter: done\r\n");
        if (g_counter_ctx.bg) {
            ui_notify_bg_done("counter");
        }
        counter_task_stop();
        return;
    }

    g_counter_ctx.value++;
    g_counter_ctx.next_tick = systick_now() + COUNTER_STEP_TICKS;
    g_counter_ctx.step_pending = 0u;
}

int counter_task_register(scheduler_t *sched)
{
    if (sched == 0) {
        return SCHED_ERR_PARAM;
    }

    g_counter_sched = sched;
    if (!g_counter_state_restored) {
        counter_task_reset_state();
    }
    g_counter_state_restored = 0u;

    task_spec_t spec;
    spec.id = AO_COUNTER;
    spec.prio = 2;
    spec.dispatch = counter_task_dispatch;
    spec.ctx = &g_counter_ctx;
    spec.queue_storage = g_counter_queue_storage;
    spec.queue_capacity = (uint16_t)(sizeof(g_counter_queue_storage) / sizeof(g_counter_queue_storage[0]));
    spec.rtc_budget_ticks = 1;
    spec.name = "counter";

    int rc = sched_register_task(sched, &spec);
    if (rc == SCHED_ERR_EXISTS) {
        return SCHED_OK;
    }
    return rc;
}

int counter_task_request_start(uint32_t limit)
{
    if (g_counter_sched == 0) {
        return SCHED_ERR_PARAM;
    }

    uint8_t is_bg = (uint8_t)(g_cmd_bg_ctx & 1u);
    if (!is_bg) {
        int lock_rc = terminal_stdin_acquire(AO_COUNTER, TERM_STDIN_MODE_RAW);
        if (lock_rc != SCHED_OK) {
            return lock_rc;
        }
    }

    int rc = sched_post(g_counter_sched, AO_COUNTER,
                        &(event_t){ .sig = COUNTER_SIG_START,
                                    .src = (uint16_t)is_bg,
                                    .arg0 = (uintptr_t)limit });
    if (rc != SCHED_OK) {
        if (!is_bg) {
            (void)terminal_stdin_release(AO_COUNTER);
        }
        return rc;
    }

    if (is_bg) {
        g_cmd_bg_async = 1u;
    } else {
        g_cmd_fg_async = 1u;
    }
    return SCHED_OK;
}

void counter_task_systick_hook(void)
{
    if (g_counter_sched == 0) {
        return;
    }
    if (g_counter_sched->table[AO_COUNTER] == 0) {
        g_counter_ctx.active = 0u;
        g_counter_ctx.step_pending = 0u;
        return;
    }
    if (!g_counter_ctx.active || g_counter_ctx.step_pending) {
        return;
    }
    if ((int32_t)(systick_now() - g_counter_ctx.next_tick) < 0) {
        return;
    }

    if (sched_post_isr(g_counter_sched, AO_COUNTER,
                       &(event_t){ .sig = COUNTER_SIG_STEP }) == SCHED_OK) {
        g_counter_ctx.step_pending = 1u;
    }
}

static int counter_restore_register_fn(scheduler_t *sched, const launch_intent_t *intent)
{
    (void)intent;
    return counter_task_register(sched);
}

static int counter_restore_get_state_fn(void *out, uint32_t *io_len)
{
    counter_task_state_t state;
    uint32_t cap;

    if (out == 0 || io_len == 0) {
        return SCHED_ERR_PARAM;
    }
    cap = *io_len;
    if (cap < sizeof(restorable_envelope_t)) {
        return SCHED_ERR_PARAM;
    }
    if (counter_task_get_state(&state) != SCHED_OK) {
        return SCHED_ERR_PARAM;
    }
    return counter_task_encode_restore_envelope(&state, 0, 0, out, io_len);
}

static int counter_restore_apply_state_fn(const void *blob, uint32_t len)
{
    const restorable_envelope_t *env = (const restorable_envelope_t *)blob;
    counter_task_state_t temp_state = g_counter_ctx;
    uint8_t have_base_state = 0u;
    uint8_t consumed[RESTORE_ENV_MAX_ENTRIES];
    uint16_t i;

    if (blob == 0) {
        return SCHED_ERR_PARAM;
    }

    /* Backward-compat fallback for direct state blobs. */
    if (len == sizeof(counter_task_state_t)) {
        return counter_task_restore_state((const counter_task_state_t *)blob);
    }
    if (len < sizeof(restorable_envelope_t)) {
        return SCHED_ERR_PARAM;
    }
    if (env->hdr.magic != RESTORE_ENV_MAGIC || env->hdr.version != RESTORE_ENV_VERSION) {
        return SCHED_ERR_PARAM;
    }
    if (env->hdr.program_id != AO_COUNTER) {
        return SCHED_ERR_PARAM;
    }
    if (env->hdr.entry_count == 0u || env->hdr.entry_count > RESTORE_ENV_MAX_ENTRIES) {
        return SCHED_ERR_PARAM;
    }
    if (env->hdr.read_idx > env->hdr.entry_count || env->hdr.write_idx > env->hdr.entry_count) {
        return SCHED_ERR_PARAM;
    }

    for (i = 0u; i < RESTORE_ENV_MAX_ENTRIES; i++) {
        consumed[i] = 0u;
    }

    for (i = 0u; i < env->hdr.entry_count; i++) {
        uint32_t best_seq = 0xFFFFFFFFu;
        int best_idx = -1;
        for (uint16_t j = 0u; j < env->hdr.entry_count; j++) {
            if (consumed[j]) {
                continue;
            }
            if (env->entries[j].seq < best_seq) {
                best_seq = env->entries[j].seq;
                best_idx = (int)j;
            }
        }
        if (best_idx < 0) {
            return SCHED_ERR_PARAM;
        }
        consumed[best_idx] = 1u;

        {
            const restorable_envelope_entry_t *e = &env->entries[best_idx];
            if (e->entry_state != RESTORE_ENV_ENTRY_PENDING) {
                continue;
            }
            if (e->action == COUNTER_RESTORE_ACTION_SET_STATE) {
                if (e->data_len != sizeof(counter_task_state_t)) {
                    return SCHED_ERR_PARAM;
                }
                temp_state = *(const counter_task_state_t *)e->data;
                temp_state.step_pending = 0u;
                have_base_state = 1u;
                continue;
            }
            if (e->action == COUNTER_RESTORE_ACTION_APPLY_OP) {
                const counter_restore_op_t *op;
                if (!have_base_state) {
                    return SCHED_ERR_PARAM;
                }
                if (e->data_len != sizeof(counter_restore_op_t)) {
                    return SCHED_ERR_PARAM;
                }
                op = (const counter_restore_op_t *)e->data;
                if (op->op == COUNTER_RESTORE_OP_ADD) {
                    temp_state.value += op->operand;
                } else if (op->op == COUNTER_RESTORE_OP_SUB) {
                    if (temp_state.value > op->operand) {
                        temp_state.value -= op->operand;
                    } else {
                        temp_state.value = 1u;
                    }
                } else if (op->op == COUNTER_RESTORE_OP_MUL) {
                    temp_state.value *= op->operand;
                } else if (op->op == COUNTER_RESTORE_OP_DIV) {
                    if (op->operand == 0u) {
                        return SCHED_ERR_PARAM;
                    }
                    temp_state.value /= op->operand;
                    if (temp_state.value == 0u) {
                        temp_state.value = 1u;
                    }
                } else {
                    return SCHED_ERR_PARAM;
                }
                if (temp_state.value > temp_state.limit && temp_state.limit != 0u) {
                    temp_state.value = temp_state.limit;
                }
                continue;
            }
            return SCHED_ERR_PARAM;
        }
    }
    if (!have_base_state) {
        return SCHED_ERR_PARAM;
    }
    g_counter_ctx = temp_state;
    g_counter_ctx.step_pending = 0u;
    counter_out_puts("counter: restore value=");
    counter_out_put_u32(g_counter_ctx.value);
    counter_out_puts(" limit=");
    counter_out_put_u32(g_counter_ctx.limit);
    counter_out_puts("\r\n");
    g_counter_state_restored = 1u;
    return SCHED_OK;
}

int counter_task_register_restore_descriptor(void)
{
    static const restore_task_descriptor_t desc = {
        .task_id = AO_COUNTER,
        .task_class = TASK_CLASS_RESTORABLE_NOW,
        .state_version = 2u,
        .min_state_len = sizeof(restorable_envelope_header_t) + sizeof(restorable_envelope_entry_t),
        .max_state_len = sizeof(restorable_envelope_t),
        .register_fn = counter_restore_register_fn,
        .get_state_fn = counter_restore_get_state_fn,
        .restore_fn = counter_restore_apply_state_fn,
        .ui_rehydrate_fn = 0
    };
    return restore_registry_register_descriptor(&desc);
}
