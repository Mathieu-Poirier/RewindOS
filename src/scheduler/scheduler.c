#include "../../include/scheduler.h"
#include "../../include/panic.h"

static ao_t g_task_aos[SCHED_MAX_AO];

static uint32_t irq_save(void)
{
    uint32_t primask;
    __asm__ volatile("mrs %0, primask" : "=r"(primask));
    __asm__ volatile("cpsid i" ::: "memory");
    return primask;
}

static void irq_restore(uint32_t primask)
{
    __asm__ volatile("msr primask, %0" : : "r"(primask) : "memory");
}

static int highest_ready_prio(const scheduler_t *s)
{
    int best = -1;
    for (uint32_t id = 0; id < SCHED_MAX_AO; id++) {
        if ((s->ready_bitmap & (1u << id)) == 0u) {
            continue;
        }
        if (s->table[id] == 0) {
            continue;
        }
        int prio = (int)s->table[id]->prio;
        if (prio > best) {
            best = prio;
        }
    }
    return best;
}

static ao_t *pick_ready_rr(scheduler_t *s, uint8_t prio)
{
    uint8_t start = (uint8_t)(s->rr_cursor[prio] + 1u);
    for (uint32_t i = 0; i < SCHED_MAX_AO; i++) {
        uint8_t id = (uint8_t)((start + i) & 31u);
        ao_t *ao = s->table[id];
        if (ao == 0 || ao->prio != prio) {
            continue;
        }
        if ((s->ready_bitmap & (1u << id)) == 0u) {
            continue;
        }
        s->rr_cursor[prio] = id;
        return ao;
    }
    return 0;
}

void eq_init(event_queue_t *q, event_t *storage, uint16_t capacity)
{
    if (q == 0) {
        return;
    }
    q->buf = storage;
    q->cap = capacity;
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    q->high_watermark = 0;
    q->dropped = 0;
}

static int eq_push_common(event_queue_t *q, const event_t *e)
{
    if (q == 0 || e == 0 || q->buf == 0 || q->cap == 0) {
        return EQ_ERR_PARAM;
    }
    if (q->count >= q->cap) {
        q->dropped++;
        return EQ_ERR_FULL;
    }

    q->buf[q->head] = *e;
    q->head = (uint16_t)((q->head + 1u) % q->cap);
    q->count++;
    if (q->count > q->high_watermark) {
        q->high_watermark = q->count;
    }
    return EQ_OK;
}

int eq_push(event_queue_t *q, const event_t *e)
{
    uint32_t key = irq_save();
    int rc = eq_push_common(q, e);
    irq_restore(key);
    return rc;
}

int eq_push_isr(event_queue_t *q, const event_t *e)
{
    return eq_push_common(q, e);
}

int eq_pop(event_queue_t *q, event_t *out)
{
    if (q == 0 || out == 0 || q->buf == 0 || q->cap == 0) {
        return EQ_ERR_PARAM;
    }

    uint32_t key = irq_save();
    if (q->count == 0) {
        irq_restore(key);
        return EQ_ERR_EMPTY;
    }

    *out = q->buf[q->tail];
    q->tail = (uint16_t)((q->tail + 1u) % q->cap);
    q->count--;
    irq_restore(key);
    return EQ_OK;
}

int eq_is_empty(const event_queue_t *q)
{
    return (q == 0 || q->count == 0u);
}

void eq_drain(event_queue_t *q)
{
    if (q == 0) {
        return;
    }
    uint32_t key = irq_save();
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    irq_restore(key);
}

void sched_init(scheduler_t *s, void (*idle_hook)(void))
{
    if (s == 0) {
        return;
    }

    for (uint32_t i = 0; i < SCHED_MAX_AO; i++) {
        s->table[i] = 0;
        s->rr_cursor[i] = 0;
    }
    s->ready_bitmap = 0u;
    s->idle_hook = idle_hook;
}

int sched_register(scheduler_t *s, ao_t *ao)
{
    if (s == 0 || ao == 0 || ao->dispatch == 0) {
        return SCHED_ERR_PARAM;
    }
    if (ao->id >= SCHED_MAX_AO || ao->prio >= SCHED_MAX_AO) {
        return SCHED_ERR_PARAM;
    }
    if (ao->q.buf == 0 || ao->q.cap == 0) {
        return SCHED_ERR_PARAM;
    }
    if (s->table[ao->id] != 0) {
        return SCHED_ERR_EXISTS;
    }
    ao->flags |= AO_FLAG_ACCEPT_EVENTS;
    ao->rtc_max_ticks = 0;
    ao->events_handled = 0;

    s->table[ao->id] = ao;
    if (ao->q.count > 0u) {
        s->ready_bitmap |= (1u << ao->id);
    }
    return SCHED_OK;
}

int sched_register_task(scheduler_t *s, const task_spec_t *spec)
{
    if (s == 0 || spec == 0 || spec->dispatch == 0 || spec->ctx == 0) {
        return SCHED_ERR_PARAM;
    }
    if (spec->id >= SCHED_MAX_AO || spec->prio >= SCHED_MAX_AO) {
        return SCHED_ERR_PARAM;
    }
    if (spec->queue_storage == 0 || spec->queue_capacity == 0u) {
        return SCHED_ERR_PARAM;
    }
    if (s->table[spec->id] != 0) {
        return SCHED_ERR_EXISTS;
    }

    ao_t *ao = &g_task_aos[spec->id];
    ao->id = spec->id;
    ao->prio = spec->prio;
    ao->reserved = 0;
    eq_init(&ao->q, spec->queue_storage, spec->queue_capacity);
    ao->dispatch = spec->dispatch;
    ao->state = spec->ctx;
    ao->flags = AO_FLAG_ACCEPT_EVENTS;
    ao->rtc_max_ticks = spec->rtc_budget_ticks;
    ao->events_handled = 0;
    ao->name = spec->name;

    return sched_register(s, ao);
}

int sched_unregister(scheduler_t *s, uint8_t ao_id)
{
    if (s == 0 || ao_id >= SCHED_MAX_AO) {
        return SCHED_ERR_PARAM;
    }
    if (s->table[ao_id] == 0) {
        return SCHED_ERR_NOT_FOUND;
    }
    s->table[ao_id] = 0;
    s->ready_bitmap &= ~(1u << ao_id);
    return SCHED_OK;
}

static int sched_post_common(scheduler_t *s, uint8_t ao_id, const event_t *e, int is_isr)
{
    if (s == 0 || e == 0 || ao_id >= SCHED_MAX_AO) {
        return SCHED_ERR_PARAM;
    }

    ao_t *ao = s->table[ao_id];
    if (ao == 0) {
        return SCHED_ERR_NOT_FOUND;
    }
    if ((ao->flags & AO_FLAG_ACCEPT_EVENTS) == 0u) {
        return SCHED_ERR_DISABLED;
    }

    int rc = is_isr ? eq_push_isr(&ao->q, e) : eq_push(&ao->q, e);
    if (rc != EQ_OK) {
        return SCHED_ERR_QUEUE_FULL;
    }

    s->ready_bitmap |= (1u << ao_id);
    return SCHED_OK;
}

int sched_post(scheduler_t *s, uint8_t ao_id, const event_t *e)
{
    return sched_post_common(s, ao_id, e, 0);
}

int sched_post_isr(scheduler_t *s, uint8_t ao_id, const event_t *e)
{
    return sched_post_common(s, ao_id, e, 1);
}

int sched_pause_accept(scheduler_t *s, uint8_t ao_id)
{
    if (s == 0 || ao_id >= SCHED_MAX_AO) {
        return SCHED_ERR_PARAM;
    }
    if (s->table[ao_id] == 0) {
        return SCHED_ERR_NOT_FOUND;
    }
    s->table[ao_id]->flags &= ~AO_FLAG_ACCEPT_EVENTS;
    return SCHED_OK;
}

int sched_resume_accept(scheduler_t *s, uint8_t ao_id)
{
    if (s == 0 || ao_id >= SCHED_MAX_AO) {
        return SCHED_ERR_PARAM;
    }
    if (s->table[ao_id] == 0) {
        return SCHED_ERR_NOT_FOUND;
    }
    s->table[ao_id]->flags |= AO_FLAG_ACCEPT_EVENTS;
    return SCHED_OK;
}

void sched_run(scheduler_t *s)
{
    if (s == 0) {
        return;
    }

    for (;;) {
        int prio = highest_ready_prio(s);
        if (prio < 0) {
            if (s->idle_hook) {
                s->idle_hook();
            }
            continue;
        }

        ao_t *ao = pick_ready_rr(s, (uint8_t)prio);
        if (ao == 0) {
            continue;
        }
        PANIC_IF(ao->dispatch == 0, "ready AO has null dispatch");

        event_t e;
        if (eq_pop(&ao->q, &e) == EQ_OK) {
            ao->dispatch(ao, &e);
            ao->events_handled++;
        }

        if (eq_is_empty(&ao->q)) {
            s->ready_bitmap &= ~(1u << ao->id);
        }
    }
}
