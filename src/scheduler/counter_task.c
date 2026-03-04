#include "../../include/counter_task.h"
#include "../../include/task_ids.h"
#include "../../include/task_signals.h"
#include "../../include/task_spec.h"
#include "../../include/cmd_context.h"
#include "../../include/console.h"
#include "../../include/log.h"
#include "../../include/restore_registry.h"
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
    counter_task_state_t *state = (counter_task_state_t *)out;
    if (out == 0 || io_len == 0) {
        return SCHED_ERR_PARAM;
    }
    if (*io_len < sizeof(counter_task_state_t)) {
        return SCHED_ERR_PARAM;
    }
    *io_len = (uint32_t)sizeof(counter_task_state_t);
    return counter_task_get_state(state);
}

static int counter_restore_apply_state_fn(const void *blob, uint32_t len)
{
    const counter_task_state_t *state = (const counter_task_state_t *)blob;
    if (blob == 0 || len != sizeof(counter_task_state_t)) {
        return SCHED_ERR_PARAM;
    }
    return counter_task_restore_state(state);
}

int counter_task_register_restore_descriptor(void)
{
    static const restore_task_descriptor_t desc = {
        .task_id = AO_COUNTER,
        .task_class = TASK_CLASS_RESTORABLE_NOW,
        .state_version = 1u,
        .min_state_len = sizeof(counter_task_state_t),
        .max_state_len = sizeof(counter_task_state_t),
        .register_fn = counter_restore_register_fn,
        .get_state_fn = counter_restore_get_state_fn,
        .restore_fn = counter_restore_apply_state_fn,
        .ui_rehydrate_fn = 0
    };
    return restore_registry_register_descriptor(&desc);
}
