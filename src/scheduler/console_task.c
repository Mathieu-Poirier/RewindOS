#include "../../include/console.h"
#include "../../include/log.h"
#include "../../include/task_ids.h"
#include "../../include/task_signals.h"
#include "../../include/task_spec.h"
#include "../../include/uart_async.h"
#include "../../include/panic.h"

#define CONSOLE_MSG_SLOTS 64u
#define CONSOLE_MSG_MAX 96u

typedef struct {
    uint16_t len;
    uint16_t off;
    char data[CONSOLE_MSG_MAX];
} console_msg_t;

typedef struct {
    scheduler_t *sched;
    console_msg_t slots[CONSOLE_MSG_SLOTS];
    uint8_t head;
    uint8_t tail;
    uint8_t count;
    uint8_t wake_posted;
    uint32_t dropped;
} console_ctx_t;

static console_ctx_t g_console;
static event_t g_console_queue_storage[8];
static uint8_t g_console_sink = CONSOLE_SINK_UART;

void console_set_sink(uint8_t sink)
{
    g_console_sink = sink;
}

uint8_t console_get_sink(void)
{
    return g_console_sink;
}

static int console_kick(void)
{
    if (g_console.sched == 0) {
        return SCHED_ERR_PARAM;
    }
    if (g_console.wake_posted) {
        return SCHED_OK;
    }

    int rc = sched_post(g_console.sched, AO_CONSOLE, &(event_t){ .sig = CONSOLE_SIG_KICK });
    if (rc == SCHED_OK) {
        g_console.wake_posted = 1u;
    }
    return rc;
}

static int console_enqueue(const char *s, uint16_t len)
{
    if (s == 0 || len == 0u) {
        return SCHED_ERR_PARAM;
    }
    if (g_console_sink == CONSOLE_SINK_LOG) {
        log_write(s, len);
        return SCHED_OK;
    }
    if (g_console.sched == 0) {
        return SCHED_ERR_PARAM;
    }
    if (g_console.count >= CONSOLE_MSG_SLOTS) {
        g_console.dropped++;
        return SCHED_ERR_FULL;
    }

    uint8_t idx = g_console.head;
    console_msg_t *m = &g_console.slots[idx];
    if (len > CONSOLE_MSG_MAX) {
        len = CONSOLE_MSG_MAX;
    }
    for (uint16_t i = 0; i < len; i++) {
        m->data[i] = s[i];
    }
    m->len = len;
    m->off = 0;

    g_console.head = (uint8_t)((g_console.head + 1u) % CONSOLE_MSG_SLOTS);
    g_console.count++;
    return console_kick();
}

static void console_task_dispatch(ao_t *self, const event_t *e)
{
    (void)self;
    PANIC_IF(e == 0, "console dispatch: null event");
    if (e->sig != CONSOLE_SIG_KICK && e->sig != CONSOLE_SIG_TX_READY) {
        return;
    }

    while (g_console.count > 0u) {
        console_msg_t *m = &g_console.slots[g_console.tail];
        PANIC_IF(m->off > m->len, "console msg offset invalid");
        while (m->off < m->len) {
            if (!uart_async_putc(m->data[m->off])) {
                g_console.wake_posted = 0u;
                return;
            }
            m->off++;
        }

        g_console.tail = (uint8_t)((g_console.tail + 1u) % CONSOLE_MSG_SLOTS);
        g_console.count--;
    }

    g_console.wake_posted = 0u;
}

int console_task_register(scheduler_t *sched)
{
    if (sched == 0) {
        return SCHED_ERR_PARAM;
    }

    g_console.sched = sched;
    g_console.head = 0u;
    g_console.tail = 0u;
    g_console.count = 0u;
    g_console.wake_posted = 0u;
    g_console.dropped = 0u;

    task_spec_t spec;
    spec.id = AO_CONSOLE;
    spec.prio = 3;
    spec.dispatch = console_task_dispatch;
    spec.ctx = &g_console;
    spec.queue_storage = g_console_queue_storage;
    spec.queue_capacity = (uint16_t)(sizeof(g_console_queue_storage) / sizeof(g_console_queue_storage[0]));
    spec.rtc_budget_ticks = 5;
    spec.name = "console";

    int rc = sched_register_task(sched, &spec);
    if (rc != SCHED_OK) {
        return rc;
    }

    uart_async_bind_tx_notifier(sched, AO_CONSOLE, CONSOLE_SIG_TX_READY);
    return SCHED_OK;
}

int console_putc(char c)
{
    return console_enqueue(&c, 1u);
}

int console_puts(const char *s)
{
    if (s == 0) {
        return SCHED_ERR_PARAM;
    }

    uint16_t len = 0u;
    while (s[len] != '\0') {
        len++;
    }
    return console_write(s, len);
}

int console_write(const char *s, uint16_t len)
{
    if (s == 0 || len == 0u) {
        return SCHED_ERR_PARAM;
    }

    uint16_t off = 0u;
    while (off < len) {
        uint16_t chunk = (uint16_t)(len - off);
        if (chunk > CONSOLE_MSG_MAX) {
            chunk = CONSOLE_MSG_MAX;
        }
        int rc = console_enqueue(s + off, chunk);
        if (rc != SCHED_OK) {
            return rc;
        }
        off = (uint16_t)(off + chunk);
    }
    return SCHED_OK;
}

int console_put_u32(uint32_t v)
{
    char buf[10];
    uint16_t n = 0u;
    if (v == 0u) {
        buf[n++] = '0';
        return console_write(buf, n);
    }

    while (v > 0u && n < sizeof(buf)) {
        buf[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }

    for (uint16_t i = 0; i < n / 2u; i++) {
        char t = buf[i];
        buf[i] = buf[n - 1u - i];
        buf[n - 1u - i] = t;
    }
    return console_write(buf, n);
}

int console_put_hex8(uint8_t v)
{
    static const char *hx = "0123456789ABCDEF";
    char buf[2];
    buf[0] = hx[(v >> 4) & 0xFu];
    buf[1] = hx[v & 0xFu];
    return console_write(buf, 2u);
}

int console_put_hex32(uint32_t v)
{
    static const char *hx = "0123456789ABCDEF";
    char buf[8];
    for (uint32_t i = 0; i < 8u; i++) {
        uint32_t sh = (7u - i) * 4u;
        buf[i] = hx[(v >> sh) & 0xFu];
    }
    return console_write(buf, 8u);
}
