#include "../../include/sd_task.h"
#include "../../include/sd_async.h"
#include "../../include/sd.h"
#include "../../include/task_ids.h"
#include "../../include/task_signals.h"
#include "../../include/task_spec.h"
#include "../../include/console.h"
#include "../../include/log.h"
#include "../../include/cmd_context.h"
#include "../../include/panic.h"

#define SD_TASK_MAX_BLOCKS 4u
#define SD_DUMP_BYTES 64u

typedef enum {
    SD_TASK_OP_NONE = 0,
    SD_TASK_OP_READ_DUMP = 1,
    SD_TASK_OP_TEST = 2
} sd_task_op_t;

typedef struct {
    volatile uint32_t completed;
    volatile uint32_t errors;
    volatile int32_t last_error;

    volatile sd_task_op_t op;
    uint32_t lba;
    uint32_t count;
    uint8_t  bg;
    uint32_t buf_words[SD_TASK_MAX_BLOCKS * (SD_BLOCK_SIZE / 4u)];
} sd_task_ctx_t;

static sd_task_ctx_t g_sd_task_ctx;
static event_t g_sd_queue_storage[8];
static scheduler_t *g_sd_sched;

static void sd_task_request_prompt(void)
{
    sched_post(g_sd_sched, AO_TERMINAL,
              &(event_t){ .sig = TERM_SIG_REPRINT_PROMPT });
}

/* Output helpers: route to log or console depending on bg flag. */

static void sd_out_putc(char c)
{
    if (g_sd_task_ctx.bg) { log_putc(c); } else { console_putc(c); }
}

static void sd_out_puts(const char *s)
{
    if (g_sd_task_ctx.bg) { log_puts(s); } else { console_puts(s); }
}

static void sd_out_put_u32(uint32_t v)
{
    if (g_sd_task_ctx.bg) { log_put_u32(v); } else { console_put_u32(v); }
}

static void sd_out_put_hex8(uint8_t v)
{
    if (g_sd_task_ctx.bg) { log_put_hex8(v); } else { console_put_hex8(v); }
}

static void sd_out_put_s32(int v)
{
    if (v < 0) {
        sd_out_putc('-');
        sd_out_put_u32((uint32_t)(-v));
        return;
    }
    sd_out_put_u32((uint32_t)v);
}

static void sd_dump_bytes(const uint8_t *buf, uint32_t count)
{
    static const char hx[] = "0123456789ABCDEF";
    /* "XX XX ... XX\r\n" — 16 bytes × 3 chars - 1 space + 2 = 49 chars max */
    char line[50];
    uint8_t pos = 0u;

    for (uint32_t i = 0; i < count; i++) {
        uint8_t v = buf[i];
        line[pos++] = hx[(v >> 4) & 0xFu];
        line[pos++] = hx[v & 0xFu];

        uint8_t end_of_row = (uint8_t)((i & 0x0Fu) == 0x0Fu);
        uint8_t last_byte  = (uint8_t)(i == count - 1u);

        if (end_of_row || last_byte) {
            line[pos++] = '\r';
            line[pos++] = '\n';
            if (g_sd_task_ctx.bg) {
                log_write(line, (uint16_t)pos);
            } else {
                console_write(line, (uint16_t)pos);
            }
            pos = 0u;
        } else {
            line[pos++] = ' ';
        }
    }
}

static void sd_task_on_done(void)
{
    PANIC_IF(g_sd_task_ctx.op == SD_TASK_OP_NONE, "sd done without active op");

    if (g_sd_task_ctx.op == SD_TASK_OP_READ_DUMP) {
        const uint8_t *base = (const uint8_t *)g_sd_task_ctx.buf_words;
        for (uint32_t i = 0; i < g_sd_task_ctx.count; i++) {
            const uint8_t *blk = base + (i * SD_BLOCK_SIZE);
            sd_out_puts("lba ");
            sd_out_put_u32(g_sd_task_ctx.lba + i);
            sd_out_puts(":\r\n");
            sd_dump_bytes(blk, SD_DUMP_BYTES);
        }
        sd_out_puts("sdread: done\r\n");
    } else if (g_sd_task_ctx.op == SD_TASK_OP_TEST) {
        const uint8_t *buf = (const uint8_t *)g_sd_task_ctx.buf_words;
        sd_out_puts("sdtest: sig=");
        sd_out_put_hex8(buf[510]);
        sd_out_put_hex8(buf[511]);
        if (buf[510] == 0x55 && buf[511] == 0xAA) {
            sd_out_puts(" (MBR)\r\n");
        } else {
            sd_out_puts("\r\n");
        }
        sd_out_puts("sdtest: PASS\r\n");
    }

    if (g_sd_task_ctx.bg) {
        console_puts("[done: ");
        console_puts(g_sd_task_ctx.op == SD_TASK_OP_TEST ? "sdtest" : "sdread");
        console_puts("]\r\n");
    }
    sd_task_request_prompt();
}

static void sd_task_on_error(int32_t err)
{
    PANIC_IF(g_sd_task_ctx.op == SD_TASK_OP_NONE, "sd error without active op");

    if (g_sd_task_ctx.op == SD_TASK_OP_TEST) {
        sd_out_puts("sdtest: read err=");
    } else {
        sd_out_puts("sdread: err=");
    }
    sd_out_put_s32((int)err);
    
    /* Provide more context about the error */
    if (err == SD_ERR_TIMEOUT) {
        sd_out_puts(" (timeout)");
    } else if (err == SD_ERR_CRC) {
        sd_out_puts(" (crc)");
    } else if (err == SD_ERR_DATA) {
        sd_out_puts(" (data)");
    } else if (err == SD_ERR_NO_INIT) {
        sd_out_puts(" (not init)");
    }
    sd_out_puts("\r\n");

    if (g_sd_task_ctx.bg) {
        console_puts("[done: ");
        console_puts(g_sd_task_ctx.op == SD_TASK_OP_TEST ? "sdtest" : "sdread");
        console_puts("]\r\n");
    }
    sd_task_request_prompt();
}

static int sd_task_start_read(uint32_t lba, uint32_t count)
{
    if (count == 0u || count > SD_TASK_MAX_BLOCKS) {
        return SD_ERR_PARAM;
    }

    int rc = sd_async_read_start(lba, count, g_sd_task_ctx.buf_words);
    if (rc == SD_OK) {
        g_sd_task_ctx.lba = lba;
        g_sd_task_ctx.count = count;
    }
    return rc;
}

static void sd_task_dispatch(ao_t *self, const event_t *e)
{
    (void)self;
    PANIC_IF(e == 0, "sd dispatch: null event");

    if (e->sig == SD_SIG_REQ_READ_DUMP) {
        if (g_sd_task_ctx.op != SD_TASK_OP_NONE) {
            console_puts("sd: busy\r\n");
            return;
        }
        g_sd_task_ctx.bg = (uint8_t)(e->src & 1u);
        g_sd_task_ctx.op = SD_TASK_OP_READ_DUMP;
        /* Clean hardware state before starting new operation */
        sd_async_init();
        int rc = sd_task_start_read((uint32_t)e->arg0, (uint32_t)e->arg1);
        if (rc != SD_OK) {
            g_sd_task_ctx.op = SD_TASK_OP_NONE;
            sd_out_puts("sdread: err=");
            sd_out_put_s32(rc);
            sd_out_puts("\r\n");
            if (g_sd_task_ctx.bg) {
                console_puts("[done: sdread]\r\n");
            }
            sd_task_request_prompt();
        }
        return;
    }

    if (e->sig == SD_SIG_REQ_TEST) {
        if (g_sd_task_ctx.op != SD_TASK_OP_NONE) {
            console_puts("sd: busy\r\n");
            return;
        }

        g_sd_task_ctx.bg = (uint8_t)(e->src & 1u);
        g_sd_task_ctx.op = SD_TASK_OP_TEST;
        sd_out_puts("sdtest: initializing...\r\n");
        sd_use_pll48(1);
        sd_set_data_clkdiv(SD_CLKDIV_FAST);
        /* Clean hardware state before init */
        sd_async_init();
        int rc = sd_init();
        if (rc != SD_OK) {
            g_sd_task_ctx.op = SD_TASK_OP_NONE;
            sd_out_puts("sdtest: init failed err=");
            sd_out_put_s32(rc);
            sd_out_puts("\r\n");
            if (g_sd_task_ctx.bg) {
                console_puts("[done: sdtest]\r\n");
            }
            sd_task_request_prompt();
            return;
        }

        const sd_info_t *info = sd_get_info();
        sd_out_puts("sdtest: ok ");
        sd_out_put_u32(info->capacity_blocks / 2048u);
        sd_out_puts("MB\r\n");
        sd_out_puts("sdtest: reading block 0...\r\n");

        rc = sd_task_start_read(0u, 1u);
        if (rc != SD_OK) {
            g_sd_task_ctx.op = SD_TASK_OP_NONE;
            sd_out_puts("sdtest: read err=");
            sd_out_put_s32(rc);
            sd_out_puts("\r\n");
            if (g_sd_task_ctx.bg) {
                console_puts("[done: sdtest]\r\n");
            }
            sd_task_request_prompt();
        }
        return;
    }

    if (e->sig == SD_SIG_TRANSFER_DONE) {
        g_sd_task_ctx.completed++;
        g_sd_task_ctx.last_error = SD_OK;
        sd_task_on_done();
        g_sd_task_ctx.op = SD_TASK_OP_NONE;
        /* Don't call sd_async_init() here - preserve state for next operation */
        return;
    }

    if (e->sig == SD_SIG_TRANSFER_ERROR) {
        g_sd_task_ctx.errors++;
        g_sd_task_ctx.last_error = (int32_t)e->arg0;
        sd_task_on_error((int32_t)e->arg0);
        g_sd_task_ctx.op = SD_TASK_OP_NONE;
        /* Don't call sd_async_init() - preserve error details for debugging */
        return;
    }
}

int sd_task_register(scheduler_t *sched)
{
    if (sched == 0) {
        return SCHED_ERR_PARAM;
    }

    g_sd_sched = sched;
    g_sd_task_ctx.completed = 0u;
    g_sd_task_ctx.errors = 0u;
    g_sd_task_ctx.last_error = SD_OK;
    g_sd_task_ctx.op = SD_TASK_OP_NONE;
    g_sd_task_ctx.lba = 0u;
    g_sd_task_ctx.count = 0u;

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

int sd_task_request_read_dump(uint32_t lba, uint32_t count)
{
    if (g_sd_sched == 0) {
        return SCHED_ERR_PARAM;
    }
    if (count == 0u) {
        count = 1u;
    }
    if (count > SD_TASK_MAX_BLOCKS) {
        count = SD_TASK_MAX_BLOCKS;
    }
    if (g_cmd_bg_ctx) {
        g_cmd_bg_async = 1u;
    } else {
        g_cmd_fg_async = 1u;
    }
    return sched_post(g_sd_sched, AO_SD,
                      &(event_t){ .sig = SD_SIG_REQ_READ_DUMP,
                                  .src = (uint16_t)g_cmd_bg_ctx,
                                  .arg0 = (uintptr_t)lba,
                                  .arg1 = (uintptr_t)count });
}

int sd_task_request_test(void)
{
    if (g_sd_sched == 0) {
        return SCHED_ERR_PARAM;
    }
    if (g_cmd_bg_ctx) {
        g_cmd_bg_async = 1u;
    } else {
        g_cmd_fg_async = 1u;
    }
    return sched_post(g_sd_sched, AO_SD,
                      &(event_t){ .sig = SD_SIG_REQ_TEST, .src = (uint16_t)g_cmd_bg_ctx });
}
