#include "../../include/lineio_async.h"
#include "../../include/parse.h"
#include "../../include/uart_async.h"
#include "../../include/uart.h"
#include "../../include/systick.h"
#include "../../include/sd.h"
#include "../../include/sd_async.h"
#include "../../include/sd_task.h"
#include "../../include/console.h"
#include "../../include/log.h"
#include "../../include/cmd_context.h"
#include "../../include/scheduler.h"
#include "../../include/task_spec.h"
#include "../../include/task_ids.h"
#include "../../include/task_signals.h"
#include "../../include/shutdown.h"
#include "../../include/panic.h"

#define MAX_ARGUMENTS 8
#define TICKS_PER_SEC 1000u
#define CMD_MAILBOX_CAP 8u

typedef struct {
        shell_state_t shell;
} terminal_task_ctx_t;

static terminal_task_ctx_t g_term_ctx;
static event_t g_term_queue_storage[8];
static scheduler_t *g_sched;

typedef struct {
        char line[SHELL_LINE_MAX];
        uint8_t used;
} cmd_slot_t;

static event_t g_cmd_queue_storage[8];
static cmd_slot_t g_cmd_slots[CMD_MAILBOX_CAP];
static uint8_t g_cmd_alloc_cursor;

static void uart_put_s32(int v)
{
        if (v < 0)
        {
                console_putc('-');
                console_put_u32((uint32_t)(-v));
                return;
        }
        console_put_u32((uint32_t)v);
}

static void sd_print_info(void)
{
        const sd_info_t *info = sd_get_info();
        if (!info->initialized)
        {
                console_puts("sd not initialized\r\n");
                return;
        }
        console_puts("rca=");
        console_put_hex32(info->rca);
        console_puts(" ocr=");
        console_put_hex32(info->ocr);
        console_puts("\r\n");
        console_puts("capacity=");
        console_put_u32(info->capacity_blocks / 2048u);
        console_puts("MB hc=");
        console_put_u32(info->high_capacity);
        console_puts(" bus=");
        console_put_u32(info->bus_width);
        console_puts("bit\r\n");
}

/* ---- ps helpers ---------------------------------------------------------- */

static void ps_str_pad(char *row, uint8_t *pos, uint8_t cap,
                       const char *s, uint8_t width)
{
        uint8_t n = 0;
        while (s && s[n] && *pos < cap) {
                row[(*pos)++] = s[n++];
        }
        while (n < width && *pos < cap) {
                row[(*pos)++] = ' ';
                n++;
        }
}

static void ps_u32_pad(char *row, uint8_t *pos, uint8_t cap,
                       uint32_t v, uint8_t width)
{
        char tmp[10];
        uint8_t n = 0;
        if (v == 0u) {
                tmp[n++] = '0';
        } else {
                while (v > 0u && n < (uint8_t)sizeof(tmp)) {
                        tmp[n++] = (char)('0' + (v % 10u));
                        v /= 10u;
                }
                for (uint8_t i = 0; i < n / 2u; i++) {
                        char t = tmp[i];
                        tmp[i] = tmp[n - 1u - i];
                        tmp[n - 1u - i] = t;
                }
        }
        for (uint8_t i = 0; i < n && *pos < cap; i++) {
                row[(*pos)++] = tmp[i];
        }
        while (n < width && *pos < cap) {
                row[(*pos)++] = ' ';
                n++;
        }
}

/* ---- cmd slot management ------------------------------------------------- */

static int cmd_slot_alloc_copy(const char *line)
{
        for (uint32_t i = 0; i < CMD_MAILBOX_CAP; i++)
        {
                uint8_t idx = (uint8_t)((g_cmd_alloc_cursor + i) % CMD_MAILBOX_CAP);
                if (g_cmd_slots[idx].used)
                        continue;

                uint32_t j = 0;
                while (j < (SHELL_LINE_MAX - 1u) && line[j] != '\0')
                {
                        g_cmd_slots[idx].line[j] = line[j];
                        j++;
                }
                g_cmd_slots[idx].line[j] = '\0';
                g_cmd_slots[idx].used = 1u;
                g_cmd_alloc_cursor = (uint8_t)((idx + 1u) % CMD_MAILBOX_CAP);
                return (int)idx;
        }
        return -1;
}

static void cmd_slot_release(uint8_t idx)
{
        PANIC_IF(idx >= CMD_MAILBOX_CAP, "cmd slot idx out of range");
        g_cmd_slots[idx].used = 0u;
}

/* ---- command executor ---------------------------------------------------- */

static void term_execute(char *line)
{
        char *argv[MAX_ARGUMENTS];
        int argc = tokenize(line, argv, MAX_ARGUMENTS);

        if (argc == 0)
                return;

        if (streq(argv[0], "help"))
        {
                console_puts("\r\n");
                console_puts("  System\r\n");
                console_puts("    reboot            Reboot system\r\n");
                console_puts("    shutdown          Enter low-power standby\r\n");
                console_puts("    uptime            Show uptime\r\n");
                console_puts("    ticks             Show tick count\r\n");
                console_puts("    ps                Show active tasks\r\n");
                console_puts("\r\n");
                console_puts("  SD Card\r\n");
                console_puts("    sdinit            Initialize SD card\r\n");
                console_puts("    sdinfo            Show card info\r\n");
                console_puts("    sdtest            Init + read test\r\n");
                console_puts("    sdread <lba> [n]  Read blocks (n<=4)\r\n");
                console_puts("    sdaread <lba>     Async read one block\r\n");
                console_puts("    sddetect          Check card presence\r\n");
                console_puts("\r\n");
                console_puts("  Debug\r\n");
                console_puts("    md <addr> [n]     Memory dump\r\n");
                console_puts("    echo <text>       Echo text\r\n");
                console_puts("    logcat            Drain background log\r\n");
                console_puts("    sdmmcdump         Dump SDMMC registers\r\n");
                console_puts("    sderror           Show SD error details\r\n");
                console_puts("    sdreset           Reset SD hardware state\r\n");
                console_puts("\r\n");
                return;
        }

        if (streq(argv[0], "echo"))
        {
                for (int i = 1; i < argc; i++)
                {
                        console_puts(argv[i]);
                        if (i + 1 < argc)
                                console_putc(' ');
                }
                console_puts("\r\n");
                return;
        }

        if (streq(argv[0], "reboot"))
        {
                /* Drain async TX so all pending console output reaches the
                 * terminal, then send the final message via synchronous UART
                 * which is guaranteed to complete before the reset fires.   */
                while (!uart_tx_done()) {
                        __asm__ volatile("wfi");
                }
                uart_puts("rebooting...\r\n");
                uart_flush_tx();

                volatile uint32_t *AIRCR = (uint32_t *)0xE000ED0Cu;
                const uint32_t VECTKEY = 0x5FAu << 16;
                *AIRCR = VECTKEY | (1u << 2); /* SYSRESETREQ */

                for (;;) {}
        }

        if (streq(argv[0], "shutdown"))
        {
                /* Drain async TX so all pending console output reaches the
                 * terminal, then send the final message via synchronous UART
                 * before handing off to shutdown_now() for Standby entry.  */
                while (!uart_tx_done()) {
                        __asm__ volatile("wfi");
                }
                uart_puts("halted.\r\n");
                shutdown_now();
                /* unreachable */
                for (;;) {}
        }

        if (streq(argv[0], "ticks"))
        {
                uint32_t t = systick_now();
                console_puts("ticks=");
                console_put_u32(t);
                console_puts("\r\n");
                return;
        }

        if (streq(argv[0], "uptime"))
        {
                uint32_t t = systick_now();
                uint32_t ms = (t * 1000u) / TICKS_PER_SEC;
                console_puts("uptime_ms=");
                console_put_u32(ms);
                console_puts("\r\n");
                return;
        }

        if (streq(argv[0], "ps"))
        {
                PANIC_IF(g_sched == 0, "ps: scheduler not bound");
                console_puts("NAME        ID  PRIO  QLEN  QCAP  HANDLED  DROPPED\r\n");
                for (uint32_t i = 0; i < SCHED_MAX_AO; i++)
                {
                        const ao_t *ao = g_sched->table[i];
                        if (ao == 0)
                                continue;
                        char row[80];
                        uint8_t pos = 0u;
                        const uint8_t cap = (uint8_t)(sizeof(row) - 3u);
                        ps_str_pad(row, &pos, cap, ao->name ? ao->name : "?", 12u);
                        ps_u32_pad(row, &pos, cap, ao->id, 4u);
                        ps_u32_pad(row, &pos, cap, ao->prio, 6u);
                        ps_u32_pad(row, &pos, cap, ao->q.count, 6u);
                        ps_u32_pad(row, &pos, cap, ao->q.cap, 6u);
                        ps_u32_pad(row, &pos, cap, ao->events_handled, 9u);
                        ps_u32_pad(row, &pos, cap, ao->q.dropped, 8u);
                        row[pos++] = '\r';
                        row[pos++] = '\n';
                        row[pos]   = '\0';
                        console_puts(row);
                }
                return;
        }

        if (streq(argv[0], "md"))
        {
                if (argc < 2)
                {
                        console_puts("usage: md <addr> [n]\r\n");
                        return;
                }

                uint32_t addr, n = 1;
                if (!parse_u32(argv[1], &addr))
                {
                        console_puts("md: bad addr\r\n");
                        return;
                }
                if (argc >= 3 && !parse_u32(argv[2], &n))
                {
                        console_puts("md: bad n\r\n");
                        return;
                }
                if (n == 0)
                        n = 1;
                if (n > 64)
                        n = 64;

                volatile uint32_t *p = (volatile uint32_t *)addr;
                for (uint32_t i = 0; i < n; i++)
                {
                        console_put_hex32((uint32_t)(addr + i * 4u));
                        console_puts(": ");
                        console_put_hex32(p[i]);
                        console_puts("\r\n");
                }
                return;
        }

        if (streq(argv[0], "sdinit"))
        {
                sd_use_pll48(1);
                sd_set_data_clkdiv(SD_CLKDIV_FAST);
                int rc = sd_init();
                if (rc == SD_OK)
                {
                        sd_async_init();
                        console_puts("sdinit: ok\r\n");
                        return;
                }
                sd_async_init();
                console_puts("sdinit: err=");
                uart_put_s32(rc);
                console_puts("\r\n");
                return;
        }

        if (streq(argv[0], "sdinfo"))
        {
                sd_print_info();
                return;
        }

        if (streq(argv[0], "sddetect"))
        {
                sd_detect_init();
                if (sd_is_detected())
                        console_puts("sd: present\r\n");
                else
                        console_puts("sd: not present\r\n");
                return;
        }

        if (streq(argv[0], "sdtest"))
        {
                int rc = sd_task_request_test();
                if (rc != SCHED_OK)
                {
                        console_puts("sdtest: queue err=");
                        uart_put_s32(rc);
                        console_puts("\r\n");
                }
                return;
        }

        if (streq(argv[0], "sdread"))
        {
                if (argc < 2)
                {
                        console_puts("usage: sdread <lba> [count]\r\n");
                        return;
                }
                uint32_t lba = 0;
                uint32_t count = 1;
                if (!parse_u32(argv[1], &lba))
                {
                        console_puts("sdread: bad lba\r\n");
                        return;
                }
                if (argc >= 3 && !parse_u32(argv[2], &count))
                {
                        console_puts("sdread: bad count\r\n");
                        return;
                }
                if (count == 0)
                        count = 1;
                int rc = sd_task_request_read_dump(lba, count);
                if (rc != SCHED_OK)
                {
                        console_puts("sdread: queue err=");
                        uart_put_s32(rc);
                        console_puts("\r\n");
                }
                return;
        }

        if (streq(argv[0], "sdaread"))
        {
                if (argc < 2)
                {
                        console_puts("usage: sdaread <lba>\r\n");
                        return;
                }
                uint32_t lba = 0;
                if (!parse_u32(argv[1], &lba))
                {
                        console_puts("sdaread: bad lba\r\n");
                        return;
                }
                int rc = sd_task_request_read_dump(lba, 1);
                if (rc != SCHED_OK)
                {
                        console_puts("sdaread: queue err=");
                        uart_put_s32(rc);
                        console_puts("\r\n");
                }
                return;
        }

        if (streq(argv[0], "logcat"))
        {
                char buf[64];
                uint16_t n;
                uint8_t any = 0u;
                while ((n = log_read(buf, (uint16_t)sizeof(buf))) > 0u)
                {
                        console_write(buf, n);
                        any = 1u;
                }
                if (!any)
                        console_puts("(log empty)\r\n");
                return;
        }

        if (streq(argv[0], "sdmmcdump"))
        {
                volatile uint32_t *sdmmc = (volatile uint32_t *)0x40012C00;
                console_puts("SDMMC Registers:\r\n");
                console_puts("POWER=0x"); console_put_hex32(sdmmc[0x00/4]); console_puts("\r\n");
                console_puts("CLKCR=0x"); console_put_hex32(sdmmc[0x04/4]); console_puts("\r\n");
                console_puts("DCTRL=0x"); console_put_hex32(sdmmc[0x2C/4]); console_puts("\r\n");
                console_puts("DLEN=0x");  console_put_hex32(sdmmc[0x28/4]); console_puts("\r\n");
                console_puts("DTIMER=0x"); console_put_hex32(sdmmc[0x24/4]); console_puts("\r\n");
                console_puts("STA=0x");   console_put_hex32(sdmmc[0x34/4]); console_puts("\r\n");
                console_puts("MASK=0x");  console_put_hex32(sdmmc[0x3C/4]); console_puts("\r\n");
                console_puts("FIFOCNT=0x"); console_put_hex32(sdmmc[0x48/4]); console_puts("\r\n");
                return;
        }

        if (streq(argv[0], "sderror"))
        {
                extern sd_context_t g_sd_ctx;
                console_puts("SD Context:\r\n");
                console_puts("error_code="); uart_put_s32(g_sd_ctx.error_code); console_puts("\r\n");
                console_puts("error_detail=0x"); console_put_hex32(g_sd_ctx.error_detail); console_puts("\r\n");
                console_puts("status="); console_put_u32(g_sd_ctx.status); console_puts("\r\n");
                console_puts("operation="); console_put_u32(g_sd_ctx.operation); console_puts("\r\n");
                if (g_sd_ctx.error_detail) {
                        console_puts("Flags: ");
                        if (g_sd_ctx.error_detail & (1<<1)) console_puts("DCRCFAIL ");
                        if (g_sd_ctx.error_detail & (1<<3)) console_puts("DTIMEOUT ");
                        if (g_sd_ctx.error_detail & (1<<5)) console_puts("RXOVERR ");
                        if (g_sd_ctx.error_detail & (1<<8)) console_puts("DATAEND ");
                        if (g_sd_ctx.error_detail & (1<<21)) console_puts("RXDAVL ");
                        console_puts("\r\n");
                }
                return;
        }

        if (streq(argv[0], "sdreset"))
        {
                extern void sd_async_init(void);
                sd_async_init();
                console_puts("SD hardware state reset\r\n");
                return;
        }

        console_puts("unknown cmd: ");
        console_puts(argv[0]);
        console_puts("\r\n");
}

static void term_enqueue_dispatch(char *line)
{
        if (g_sched == 0)
        {
                PANIC("terminal scheduler not bound");
        }

        int slot = cmd_slot_alloc_copy(line);
        if (slot < 0)
        {
                console_puts("cmd queue full\r\n");
                return;
        }

        /* Strip trailing whitespace and check for background '&' operator. */
        char *s = g_cmd_slots[slot].line;
        uint32_t slen = 0u;
        while (s[slen] != '\0') { slen++; }
        while (slen > 0u && (s[slen - 1u] == ' ' || s[slen - 1u] == '\t')) { slen--; }
        uint8_t is_bg = 0u;
        if (slen > 0u && s[slen - 1u] == '&')
        {
                is_bg = 1u;
                slen--;
                /* Also strip any whitespace before '&'. */
                while (slen > 0u && (s[slen - 1u] == ' ' || s[slen - 1u] == '\t')) { slen--; }
                s[slen] = '\0';
        }

        int rc = sched_post(g_sched, AO_CMD,
                            &(event_t){ .sig = CMD_SIG_EXEC, .arg0 = (uintptr_t)slot, .arg1 = (uintptr_t)is_bg });
        if (rc != SCHED_OK)
        {
                cmd_slot_release((uint8_t)slot);
                console_puts("cmd dispatch failed\r\n");
        }
}

static void terminal_task_dispatch(ao_t *self, const event_t *e)
{
        (void)self;
        PANIC_IF(e == 0, "terminal dispatch: null event");

        if (e->sig == TERM_SIG_REPRINT_PROMPT) {
                console_puts(g_term_ctx.shell.prompt_str);
                return;
        }

        if (e->sig != TERM_SIG_UART_RX_READY)
                return;

        for (;;) {
                while (uart_rx_available()) {
                        shell_tick(&g_term_ctx.shell, term_enqueue_dispatch);
                }
                if (!uart_async_rx_event_finish()) {
                        break;
                }
        }
}

int terminal_task_register(scheduler_t *sched)
{
        if (sched == 0)
                return SCHED_ERR_PARAM;

        g_sched = sched;
        shell_state_init(&g_term_ctx.shell, "rewind> ");
        uart_async_bind_scheduler(sched, AO_TERMINAL, TERM_SIG_UART_RX_READY);

        task_spec_t spec;
        spec.id = AO_TERMINAL;
        spec.prio = 1;
        spec.dispatch = terminal_task_dispatch;
        spec.ctx = &g_term_ctx;
        spec.queue_storage = g_term_queue_storage;
        spec.queue_capacity = (uint16_t)(sizeof(g_term_queue_storage) / sizeof(g_term_queue_storage[0]));
        spec.rtc_budget_ticks = 1;
        spec.name = "terminal";

        return sched_register_task(sched, &spec);
}

static void cmd_task_dispatch(ao_t *self, const event_t *e)
{
        (void)self;
        PANIC_IF(e == 0, "cmd dispatch: null event");
        if (e->sig != CMD_SIG_EXEC)
                return;

        uint8_t idx    = (uint8_t)e->arg0;
        uint8_t is_bg  = (uint8_t)(e->arg1 & 1u);
        PANIC_IF(idx >= CMD_MAILBOX_CAP, "cmd event idx out of range");
        PANIC_IF(g_cmd_slots[idx].used == 0u, "cmd event for free slot");

        /* Snapshot the command name before term_execute may modify the slot. */
        char cmd_name[32];
        uint32_t ni = 0u;
        const char *src = g_cmd_slots[idx].line;
        /* Skip leading whitespace. */
        while (*src == ' ' || *src == '\t') { src++; }
        /* Copy first token. */
        while (*src != '\0' && *src != ' ' && *src != '\t' && ni < (uint32_t)(sizeof(cmd_name) - 1u))
        {
                cmd_name[ni++] = *src++;
        }
        cmd_name[ni] = '\0';

        g_cmd_bg_ctx   = is_bg;
        g_cmd_bg_async = 0u;
        g_cmd_fg_async = 0u;
        if (is_bg)
                console_set_sink(CONSOLE_SINK_LOG);

        term_execute(g_cmd_slots[idx].line);

        console_set_sink(CONSOLE_SINK_UART);
        if (is_bg && !g_cmd_bg_async)
        {
                console_puts("[done: ");
                console_puts(cmd_name);
                console_puts("]\r\n");
        }
        g_cmd_bg_ctx = 0u;

        cmd_slot_release(idx);

        /* Print prompt after output so it always appears last.
         * Skip if an async command was dispatched --
         * the async completion handler will post TERM_SIG_REPRINT_PROMPT. */
        if (!g_cmd_fg_async && !g_cmd_bg_async)
                console_puts(g_term_ctx.shell.prompt_str);
        g_cmd_fg_async = 0u;
}

int cmd_task_register(scheduler_t *sched)
{
        if (sched == 0)
                return SCHED_ERR_PARAM;

        task_spec_t spec;
        spec.id = AO_CMD;
        spec.prio = 0;
        spec.dispatch = cmd_task_dispatch;
        spec.ctx = g_cmd_slots;
        spec.queue_storage = g_cmd_queue_storage;
        spec.queue_capacity = (uint16_t)(sizeof(g_cmd_queue_storage) / sizeof(g_cmd_queue_storage[0]));
        spec.rtc_budget_ticks = 1;
        spec.name = "cmd";

        for (uint32_t i = 0; i < CMD_MAILBOX_CAP; i++)
                g_cmd_slots[i].used = 0u;
        g_cmd_alloc_cursor = 0u;

        return sched_register_task(sched, &spec);
}
