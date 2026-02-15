#include "../../include/sd_task.h"
#include "../../include/sd_async.h"
#include "../../include/task_ids.h"
#include "../../include/task_signals.h"
#include "../../include/task_spec.h"
#include "../../include/uart_async.h"

typedef struct {
    volatile uint32_t completed;
    volatile uint32_t errors;
    volatile int32_t last_error;
} sd_task_ctx_t;

static sd_task_ctx_t g_sd_task_ctx;
static event_t g_sd_queue_storage[8];

static void sd_task_dispatch(ao_t *self, const event_t *e)
{
    (void)self;
    if (e == 0) {
        return;
    }

    if (e->sig == SD_SIG_TRANSFER_DONE) {
        g_sd_task_ctx.completed++;
        g_sd_task_ctx.last_error = SD_OK;
        uart_async_puts("sd: transfer complete\r\n");
        sd_async_init();
        return;
    }

    if (e->sig == SD_SIG_TRANSFER_ERROR) {
        g_sd_task_ctx.errors++;
        g_sd_task_ctx.last_error = (int32_t)e->arg0;
        uart_async_puts("sd: error\r\n");
        sd_async_init();
        return;
    }
}

int sd_task_register(scheduler_t *sched)
{
    if (sched == 0) {
        return SCHED_ERR_PARAM;
    }

    task_spec_t spec;
    spec.id = AO_SD;
    spec.prio = 2;
    spec.dispatch = sd_task_dispatch;
    spec.ctx = &g_sd_task_ctx;
    spec.queue_storage = g_sd_queue_storage;
    spec.queue_capacity = (uint16_t)(sizeof(g_sd_queue_storage) / sizeof(g_sd_queue_storage[0]));
    spec.rtc_budget_ticks = 1;
    spec.name = "sd";

    int rc = sched_register_task(sched, &spec);
    if (rc != SCHED_OK) {
        return rc;
    }

    sd_async_bind_scheduler(sched, AO_SD, SD_SIG_TRANSFER_DONE, SD_SIG_TRANSFER_ERROR);
    return SCHED_OK;
}
