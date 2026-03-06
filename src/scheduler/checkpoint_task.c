#include "../../include/checkpoint_task.h"

#include "../../include/log.h"
#include "../../include/panic.h"
#include "../../include/systick.h"
#include "../../include/task_ids.h"
#include "../../include/task_signals.h"
#include "../../include/task_spec.h"
#include "../../include/terminal.h"

#define TICKS_PER_SEC 1000u

typedef struct {
    uint32_t interval_ms;
    uint32_t interval_ticks;
    uint32_t next_tick;
    uint8_t timer_event_pending;
} checkpoint_task_ctx_t;

static checkpoint_task_ctx_t g_checkpoint_ctx;
static event_t g_checkpoint_queue_storage[4];
static scheduler_t *g_checkpoint_sched;

static void checkpoint_log_s32(int value)
{
    if (value < 0) {
        log_putc('-');
        log_put_u32((uint32_t)(-value));
        return;
    }
    log_put_u32((uint32_t)value);
}

void checkpoint_task_set_interval_ms(uint32_t interval_ms)
{
    if (interval_ms == 0u) {
        g_checkpoint_ctx.interval_ms = 0u;
        g_checkpoint_ctx.interval_ticks = 0u;
        g_checkpoint_ctx.next_tick = 0u;
        g_checkpoint_ctx.timer_event_pending = 0u;
        return;
    }

    uint32_t ticks = ((interval_ms * TICKS_PER_SEC) + 999u) / 1000u;
    if (ticks == 0u) {
        ticks = 1u;
    }

    g_checkpoint_ctx.interval_ms = interval_ms;
    g_checkpoint_ctx.interval_ticks = ticks;
    g_checkpoint_ctx.next_tick = systick_now() + ticks;
    g_checkpoint_ctx.timer_event_pending = 0u;
}

uint32_t checkpoint_task_get_interval_ms(void)
{
    return g_checkpoint_ctx.interval_ms;
}

void checkpoint_task_systick_hook(void)
{
    event_t ev;
    uint32_t now;

    if (g_checkpoint_sched == 0 || g_checkpoint_ctx.interval_ticks == 0u) {
        return;
    }

    now = systick_now();
    if ((int32_t)(now - g_checkpoint_ctx.next_tick) < 0) {
        return;
    }

    g_checkpoint_ctx.next_tick = now + g_checkpoint_ctx.interval_ticks;
    if (g_checkpoint_ctx.timer_event_pending) {
        return;
    }

    ev.sig = CKPT_SIG_TIMER;
    ev.src = 0u;
    ev.arg0 = 0u;
    ev.arg1 = 0u;

    if (sched_post_isr(g_checkpoint_sched, AO_CHECKPOINT, &ev) == SCHED_OK) {
        g_checkpoint_ctx.timer_event_pending = 1u;
    }
}

static void checkpoint_task_dispatch(ao_t *self, const event_t *e)
{
    (void)self;
    PANIC_IF(e == 0, "checkpoint dispatch: null event");
    if (e == 0) {
        return;
    }

    if (e->sig != CKPT_SIG_TIMER) {
        return;
    }

    g_checkpoint_ctx.timer_event_pending = 0u;
    if (g_checkpoint_ctx.interval_ticks == 0u) {
        return;
    }

    uint32_t lba = 0u;
    uint32_t slot = 0u;
    uint32_t seq = 0u;
    uint32_t regions = 0u;
    int rc = terminal_ckpt_save_sd_once(&lba, &slot, &seq, &regions);

    if (rc == SCHED_OK) {
        log_puts("ckpt:auto seq=");
        log_put_u32(seq);
        log_puts(" slot=");
        log_put_u32(slot);
        log_puts(" regions=");
        log_put_u32(regions);
        log_puts("\r\n");
        return;
    }

    if (rc == SCHED_ERR_NOT_FOUND) {
        return;
    }

    log_puts("ckpt:auto err=");
    checkpoint_log_s32(rc);
    log_puts("\r\n");
}

int checkpoint_task_register(scheduler_t *sched)
{
    if (sched == 0) {
        return SCHED_ERR_PARAM;
    }

    g_checkpoint_sched = sched;
    g_checkpoint_ctx.timer_event_pending = 0u;

    task_spec_t spec;
    spec.id = AO_CHECKPOINT;
    spec.prio = 2;
    spec.dispatch = checkpoint_task_dispatch;
    spec.ctx = &g_checkpoint_ctx;
    spec.queue_storage = g_checkpoint_queue_storage;
    spec.queue_capacity = (uint16_t)(sizeof(g_checkpoint_queue_storage) / sizeof(g_checkpoint_queue_storage[0]));
    spec.rtc_budget_ticks = 1;
    spec.name = "checkpoint";

    return sched_register_task(sched, &spec);
}
