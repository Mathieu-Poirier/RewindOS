#include "../../include/lineio_async.h"
#include "../../include/parse.h"
#include "../../include/uart_async.h"
#include "../../include/uart.h"
#include "../../include/systick.h"
#include "../../include/sd.h"
#include "../../include/sd_async.h"
#include "../../include/sd_task.h"
#include "../../include/counter_task.h"
#include "../../include/console.h"
#include "../../include/log.h"
#include "../../include/cmd_context.h"
#include "../../include/scheduler.h"
#include "../../include/task_spec.h"
#include "../../include/task_ids.h"
#include "../../include/task_signals.h"
#include "../../include/shutdown.h"
#include "../../include/terminal.h"
#include "../../include/panic.h"
#include "../../include/restore_registry.h"
#include "../../include/restore_loader.h"
#include "../../include/restore_sim.h"

#define MAX_ARGUMENTS 8
#define TICKS_PER_SEC 1000u
#define CMD_MAILBOX_CAP 8u
#define TERM_STDIN_OWNER_NONE 0xFFu
#define TERM_SHORTCUT_CTRL_C 0x03

typedef struct {
        shell_state_t shell;
        uint8_t stdin_owner;
        uint8_t stdin_mode;
        const ao_t *stdin_owner_ref;
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

void ui_notify_bg_done(const char *name)
{
        if (name == 0 || name[0] == '\0')
                name = "?";

        console_puts("\r\n[done: ");
        console_puts(name);
        console_puts("]\r\n");
        console_puts(g_term_ctx.shell.prompt_str);
        if (g_term_ctx.shell.len > 0u)
                console_write(g_term_ctx.shell.line, (uint16_t)g_term_ctx.shell.len);
}

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

static const char *restore_class_name(uint8_t task_class)
{
        if (task_class == TASK_CLASS_RESTORABLE_NOW) return "restorable";
        if (task_class == TASK_CLASS_RESTART_ONLY) return "restart-only";
        if (task_class == TASK_CLASS_NON_RESTORABLE) return "non-restorable";
        return "unknown";
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

static int term_is_protected_task(uint8_t ao_id)
{
        return (ao_id == AO_TERMINAL || ao_id == AO_CMD || ao_id == AO_CONSOLE);
}

static int term_stdin_owner_valid(void)
{
        if (g_sched == 0)
                return 0;
        if (g_term_ctx.stdin_owner == TERM_STDIN_OWNER_NONE)
                return 0;
        if (g_term_ctx.stdin_owner >= SCHED_MAX_AO)
                return 0;
        if (g_sched->table[g_term_ctx.stdin_owner] == 0)
                return 0;
        if (g_term_ctx.stdin_owner_ref == 0)
                return 0;
        return g_sched->table[g_term_ctx.stdin_owner] == g_term_ctx.stdin_owner_ref;
}

static void term_stdin_release_internal(void)
{
        g_term_ctx.stdin_owner = TERM_STDIN_OWNER_NONE;
        g_term_ctx.stdin_mode = 0u;
        g_term_ctx.stdin_owner_ref = 0;
}

static int term_kill_task(uint8_t ao_id, const char **out_name)
{
        if (g_sched == 0 || ao_id >= SCHED_MAX_AO)
                return SCHED_ERR_PARAM;
        if (term_is_protected_task(ao_id))
                return SCHED_ERR_DISABLED;
        if (g_sched->table[ao_id] == 0)
                return SCHED_ERR_NOT_FOUND;

        const char *name = g_sched->table[ao_id]->name;
        int rc = sched_unregister(g_sched, ao_id);
        if (rc == SCHED_OK && g_term_ctx.stdin_owner == ao_id)
        {
                term_stdin_release_internal();
        }

        if (out_name)
                *out_name = name;
        return rc;
}

static int term_find_task_id(const char *selector, uint8_t *out_id)
{
        uint32_t id = 0u;
        if (parse_u32(selector, &id))
        {
                if (id >= SCHED_MAX_AO)
                        return 0;
                if (g_sched->table[id] == 0)
                        return 0;
                *out_id = (uint8_t)id;
                return 1;
        }

        for (uint8_t i = 0u; i < SCHED_MAX_AO; i++)
        {
                const ao_t *ao = g_sched->table[i];
                if (ao == 0 || ao->name == 0)
                        continue;
                if (streq(ao->name, selector))
                {
                        *out_id = i;
                        return 1;
                }
        }

        return 0;
}

int terminal_stdin_acquire(uint8_t owner_ao, uint8_t mode)
{
        if (g_sched == 0 || owner_ao >= SCHED_MAX_AO)
                return SCHED_ERR_PARAM;
        if (mode != TERM_STDIN_MODE_RAW)
                return SCHED_ERR_PARAM;
        if (g_sched->table[owner_ao] == 0)
                return SCHED_ERR_NOT_FOUND;
        if (term_is_protected_task(owner_ao))
                return SCHED_ERR_DISABLED;
        if (g_term_ctx.stdin_owner != TERM_STDIN_OWNER_NONE)
                return SCHED_ERR_EXISTS;

        g_term_ctx.stdin_owner = owner_ao;
        g_term_ctx.stdin_mode = mode;
        g_term_ctx.stdin_owner_ref = g_sched->table[owner_ao];
        shell_rx_idle(&g_term_ctx.shell);
        return SCHED_OK;
}

int terminal_stdin_release(uint8_t owner_ao)
{
        if (g_sched == 0 || owner_ao >= SCHED_MAX_AO)
                return SCHED_ERR_PARAM;
        if (g_term_ctx.stdin_owner == TERM_STDIN_OWNER_NONE)
                return SCHED_ERR_NOT_FOUND;
        if (g_term_ctx.stdin_owner != owner_ao)
                return SCHED_ERR_PARAM;
        if (!term_stdin_owner_valid())
                return SCHED_ERR_NOT_FOUND;

        term_stdin_release_internal();
        shell_rx_idle(&g_term_ctx.shell);
        (void)sched_post(g_sched, AO_TERMINAL, &(event_t){ .sig = TERM_SIG_REPRINT_PROMPT });
        return SCHED_OK;
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
                console_puts("    kill <task>       Remove task by id or name\r\n");
                console_puts("    counter [n]       Run simple counter program\r\n");
                console_puts("    Ctrl-C            Kill foreground input owner\r\n");
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
                console_puts("    restorestat       Show restore loader/registry stats\r\n");
                console_puts("    ckptsave          Save counter state to in-memory ckpt queue\r\n");
                console_puts("    ckptload          Load counter state from in-memory ckpt queue\r\n");
                console_puts("    ckptq             Show in-memory ckpt queue stats\r\n");
                console_puts("    restoresim [limit] [value] [bg]  Simulate in-memory restore blob\r\n");
                console_puts("    restoresimop <limit> <value> <mul> <div> [bg]  Simulate ordered ops restore\r\n");
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

        if (streq(argv[0], "kill"))
        {
                PANIC_IF(g_sched == 0, "kill: scheduler not bound");
                if (argc < 2)
                {
                        console_puts("usage: kill <task-id|name>\r\n");
                        return;
                }

                uint8_t ao_id = 0u;
                if (!term_find_task_id(argv[1], &ao_id))
                {
                        console_puts("kill: task not found\r\n");
                        return;
                }

                const char *name = 0;
                int rc = term_kill_task(ao_id, &name);
                if (rc != SCHED_OK)
                {
                        if (rc == SCHED_ERR_DISABLED)
                        {
                                console_puts("kill: protected task\r\n");
                                return;
                        }
                        console_puts("kill: err=");
                        uart_put_s32(rc);
                        console_puts("\r\n");
                        return;
                }

                console_puts("killed ");
                if (name && name[0] != '\0')
                        console_puts(name);
                else
                        console_puts("(unnamed)");
                console_puts(" id=");
                console_put_u32((uint32_t)ao_id);
                console_puts("\r\n");
                return;
        }

        if (streq(argv[0], "counter"))
        {
                uint32_t limit = 10u;
                if (argc >= 2 && !parse_u32(argv[1], &limit))
                {
                        console_puts("counter: bad n\r\n");
                        return;
                }

                int rc = counter_task_register(g_sched);
                if (rc != SCHED_OK)
                {
                        console_puts("counter: start err=");
                        uart_put_s32(rc);
                        console_puts("\r\n");
                        return;
                }

                rc = counter_task_request_start(limit);
                if (rc != SCHED_OK)
                {
                        console_puts("counter: queue err=");
                        uart_put_s32(rc);
                        console_puts("\r\n");
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
                sd_detect_init();
                if (!sd_is_detected())
                {
                        console_puts("sd: not present\r\n");
                        return;
                }
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

        if (streq(argv[0], "restorestat"))
        {
                restore_loader_stats_t st;
                restore_loader_get_stats(&st);
                console_puts("restore: calls=");
                console_put_u32(st.calls);
                console_puts(" applied=");
                console_put_u32(st.applied);
                console_puts(" skipped=");
                console_put_u32(st.skipped);
                console_puts(" failed=");
                console_put_u32(st.failed);
                console_puts(" last_rc=");
                uart_put_s32((int)st.last_rc);
                console_puts("\r\n");

                console_puts("registry:\r\n");
                for (uint8_t id = 0u; id < SCHED_MAX_AO; id++)
                {
                        const restore_task_descriptor_t *d = restore_registry_find(id);
                        if (d == 0)
                                continue;
                        console_puts("  id=");
                        console_put_u32(id);
                        console_puts(" class=");
                        console_puts(restore_class_name(d->task_class));
                        console_puts(" ver=");
                        console_put_u32(d->state_version);
                        console_puts("\r\n");
                }
                return;
        }

        if (streq(argv[0], "ckptq"))
        {
                console_puts("ckptq: pending=");
                console_put_u32(restore_sim_pending());
                console_puts(" gen=");
                console_put_u32(restore_sim_generation());
                console_puts("\r\n");
                return;
        }

        if (streq(argv[0], "ckptsave"))
        {
                counter_task_state_t st;
                uint8_t blob[sizeof(restorable_envelope_t)];
                uint32_t blob_len = (uint32_t)sizeof(blob);
                int rc = counter_task_get_state(&st);
                if (rc != SCHED_OK)
                {
                        console_puts("ckptsave: counter state err=");
                        uart_put_s32(rc);
                        console_puts("\r\n");
                        return;
                }
                (void)restore_sim_reset();
                rc = counter_task_encode_restore_envelope(&st, 0, 0, blob, &blob_len);
                if (rc != SCHED_OK)
                {
                        console_puts("ckptsave: encode err=");
                        uart_put_s32(rc);
                        console_puts("\r\n");
                        return;
                }
                rc = restore_sim_enqueue((uint16_t)AO_COUNTER, 2u, blob, blob_len);
                if (rc != SCHED_OK)
                {
                        console_puts("ckptsave: enqueue err=");
                        uart_put_s32(rc);
                        console_puts("\r\n");
                        return;
                }
                console_puts("ckptsave: ok value=");
                console_put_u32(st.value);
                console_puts(" limit=");
                console_put_u32(st.limit);
                console_puts(" pending=");
                console_put_u32(restore_sim_pending());
                console_puts("\r\n");
                return;
        }

        if (streq(argv[0], "ckptload"))
        {
                uint32_t applied = 0u, skipped = 0u, failed = 0u;
                counter_task_state_t st;
                int rc = restore_sim_apply(g_sched, &applied, &skipped, &failed);
                console_puts("ckptload: rc=");
                uart_put_s32(rc);
                console_puts(" applied=");
                console_put_u32(applied);
                console_puts(" skipped=");
                console_put_u32(skipped);
                console_puts(" failed=");
                console_put_u32(failed);
                console_puts("\r\n");

                if (counter_task_get_state(&st) == SCHED_OK)
                {
                        console_puts("counter state: active=");
                        console_put_u32(st.active);
                        console_puts(" value=");
                        console_put_u32(st.value);
                        console_puts(" limit=");
                        console_put_u32(st.limit);
                        console_puts(" bg=");
                        console_put_u32(st.bg);
                        console_puts("\r\n");
                }
                return;
        }

        if (streq(argv[0], "restoresim"))
        {
                uint32_t limit = 50u;
                uint32_t value = 25u;
                uint32_t bg = 0u;
                counter_task_state_t st;
                uint8_t blob[sizeof(restorable_envelope_t)];
                uint32_t blob_len = (uint32_t)sizeof(blob);
                uint32_t applied = 0u, skipped = 0u, failed = 0u;
                int rc;

                if (argc >= 2 && !parse_u32(argv[1], &limit))
                {
                        console_puts("restoresim: bad limit\r\n");
                        return;
                }
                if (argc >= 3 && !parse_u32(argv[2], &value))
                {
                        console_puts("restoresim: bad value\r\n");
                        return;
                }
                if (argc >= 4 && !parse_u32(argv[3], &bg))
                {
                        console_puts("restoresim: bad bg\r\n");
                        return;
                }
                if (limit == 0u)
                        limit = 1u;
                if (value == 0u)
                        value = 1u;
                if (value > limit)
                        value = limit;

                st.active = 1u;
                st.bg = (uint8_t)(bg & 1u);
                st.step_pending = 0u;
                st.limit = limit;
                st.value = value;
                st.next_tick = systick_now() + 10u;

                (void)restore_sim_reset();
                rc = counter_task_encode_restore_envelope(&st, 0, 0, blob, &blob_len);
                if (rc != SCHED_OK)
                {
                        console_puts("restoresim: encode err=");
                        uart_put_s32(rc);
                        console_puts("\r\n");
                        return;
                }
                rc = restore_sim_enqueue((uint16_t)AO_COUNTER, 2u, blob, blob_len);
                if (rc != SCHED_OK)
                {
                        console_puts("restoresim: enqueue err=");
                        uart_put_s32(rc);
                        console_puts("\r\n");
                        return;
                }
                /* Also enqueue one restart-only task entry to validate skip accounting. */
                rc = restore_sim_enqueue((uint16_t)AO_TERMINAL, 0u, 0, 0u);
                if (rc != SCHED_OK)
                {
                        console_puts("restoresim: enqueue2 err=");
                        uart_put_s32(rc);
                        console_puts("\r\n");
                        return;
                }

                rc = restore_sim_apply(g_sched, &applied, &skipped, &failed);
                console_puts("restoresim: rc=");
                uart_put_s32(rc);
                console_puts(" applied=");
                console_put_u32(applied);
                console_puts(" skipped=");
                console_put_u32(skipped);
                console_puts(" failed=");
                console_put_u32(failed);
                console_puts("\r\n");

                if (counter_task_get_state(&st) == SCHED_OK)
                {
                        console_puts("counter state: active=");
                        console_put_u32(st.active);
                        console_puts(" value=");
                        console_put_u32(st.value);
                        console_puts(" limit=");
                        console_put_u32(st.limit);
                        console_puts(" bg=");
                        console_put_u32(st.bg);
                        console_puts("\r\n");
                }
                return;
        }

        if (streq(argv[0], "restoresimop"))
        {
                uint32_t limit = 100u;
                uint32_t value = 10u;
                uint32_t mul = 2u;
                uint32_t div = 1u;
                uint32_t bg = 0u;
                counter_task_state_t st;
                counter_restore_op_t ops[2];
                uint16_t op_count = 0u;
                uint8_t blob[sizeof(restorable_envelope_t)];
                uint32_t blob_len = (uint32_t)sizeof(blob);
                uint32_t applied = 0u, skipped = 0u, failed = 0u;
                int rc;

                if (argc < 5)
                {
                        console_puts("usage: restoresimop <limit> <value> <mul> <div> [bg]\r\n");
                        return;
                }
                if (!parse_u32(argv[1], &limit) ||
                    !parse_u32(argv[2], &value) ||
                    !parse_u32(argv[3], &mul) ||
                    !parse_u32(argv[4], &div))
                {
                        console_puts("restoresimop: bad args\r\n");
                        return;
                }
                if (argc >= 6 && !parse_u32(argv[5], &bg))
                {
                        console_puts("restoresimop: bad bg\r\n");
                        return;
                }
                if (limit == 0u) limit = 1u;
                if (value == 0u) value = 1u;
                if (value > limit) value = limit;
                if (mul > 1u) {
                        ops[op_count].op = COUNTER_RESTORE_OP_MUL;
                        ops[op_count].reserved[0] = 0u;
                        ops[op_count].reserved[1] = 0u;
                        ops[op_count].reserved[2] = 0u;
                        ops[op_count].operand = mul;
                        op_count++;
                }
                if (div > 1u) {
                        ops[op_count].op = COUNTER_RESTORE_OP_DIV;
                        ops[op_count].reserved[0] = 0u;
                        ops[op_count].reserved[1] = 0u;
                        ops[op_count].reserved[2] = 0u;
                        ops[op_count].operand = div;
                        op_count++;
                }

                st.active = 1u;
                st.bg = (uint8_t)(bg & 1u);
                st.step_pending = 0u;
                st.limit = limit;
                st.value = value;
                st.next_tick = systick_now() + 10u;

                rc = counter_task_encode_restore_envelope(&st, ops, op_count, blob, &blob_len);
                if (rc != SCHED_OK)
                {
                        console_puts("restoresimop: encode err=");
                        uart_put_s32(rc);
                        console_puts("\r\n");
                        return;
                }
                (void)restore_sim_reset();
                rc = restore_sim_enqueue((uint16_t)AO_COUNTER, 2u, blob, blob_len);
                if (rc != SCHED_OK)
                {
                        console_puts("restoresimop: enqueue err=");
                        uart_put_s32(rc);
                        console_puts("\r\n");
                        return;
                }
                rc = restore_sim_apply(g_sched, &applied, &skipped, &failed);
                console_puts("restoresimop: rc=");
                uart_put_s32(rc);
                console_puts(" applied=");
                console_put_u32(applied);
                console_puts(" skipped=");
                console_put_u32(skipped);
                console_puts(" failed=");
                console_put_u32(failed);
                console_puts("\r\n");

                if (counter_task_get_state(&st) == SCHED_OK)
                {
                        console_puts("counter state: value=");
                        console_put_u32(st.value);
                        console_puts(" limit=");
                        console_put_u32(st.limit);
                        console_puts(" bg=");
                        console_put_u32(st.bg);
                        console_puts("\r\n");
                }
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

static void term_on_fg_shortcut_interrupt(void)
{
        if (!term_stdin_owner_valid())
        {
                term_stdin_release_internal();
                console_puts("\r\nstdin owner stale; released\r\n");
                console_puts(g_term_ctx.shell.prompt_str);
                return;
        }

        uint8_t owner = g_term_ctx.stdin_owner;
        const char *name = 0;
        int rc = term_kill_task(owner, &name);

        console_puts("^C\r\n");
        if (rc == SCHED_OK)
        {
                console_puts("killed ");
                if (name && name[0] != '\0')
                        console_puts(name);
                else
                        console_puts("(unnamed)");
                console_puts("\r\n");
        }
        else
        {
                console_puts("interrupt: kill err=");
                uart_put_s32(rc);
                console_puts("\r\n");
        }
        console_puts(g_term_ctx.shell.prompt_str);
}

static void term_dispatch_owned_input(void)
{
        if (!term_stdin_owner_valid())
        {
                term_stdin_release_internal();
                console_puts("\r\nstdin owner stale; released\r\n");
                console_puts(g_term_ctx.shell.prompt_str);
                return;
        }

        int c = uart_async_getc();
        if (c < 0)
                return;

        if (g_term_ctx.stdin_mode == TERM_STDIN_MODE_RAW)
        {
                if ((uint8_t)c == TERM_SHORTCUT_CTRL_C)
                {
                        term_on_fg_shortcut_interrupt();
                        return;
                }

                int rc = sched_post(g_sched, g_term_ctx.stdin_owner,
                                    &(event_t){ .sig = TERM_SIG_STDIN_RAW, .arg0 = (uintptr_t)((uint8_t)c) });
                if (rc != SCHED_OK)
                {
                        if (rc == SCHED_ERR_NOT_FOUND || rc == SCHED_ERR_DISABLED)
                                term_stdin_release_internal();
                        console_puts("\r\nstdin dispatch err=");
                        uart_put_s32(rc);
                        console_puts("\r\n");
                        console_puts(g_term_ctx.shell.prompt_str);
                }
                return;
        }

        /* Unknown mode: fail closed by releasing lock back to shell. */
        term_stdin_release_internal();
        console_puts("\r\nstdin mode invalid; released\r\n");
        console_puts(g_term_ctx.shell.prompt_str);
}

static void terminal_task_dispatch(ao_t *self, const event_t *e)
{
        (void)self;
        PANIC_IF(e == 0, "terminal dispatch: null event");

        if (e->sig == TERM_SIG_REPRINT_PROMPT) {
                if (!term_stdin_owner_valid())
                        console_puts(g_term_ctx.shell.prompt_str);
                return;
        }

        if (e->sig != TERM_SIG_UART_RX_READY)
                return;

        for (;;) {
                while (uart_rx_available()) {
                        if (!term_stdin_owner_valid())
                                shell_tick(&g_term_ctx.shell, term_enqueue_dispatch);
                        else
                                term_dispatch_owned_input();
                }
                if (!uart_async_rx_event_finish()) {
                        if (!term_stdin_owner_valid())
                                shell_rx_idle(&g_term_ctx.shell);
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
        term_stdin_release_internal();
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
         * Foreground async commands defer prompt until completion.
         * Background async commands return prompt immediately. */
        if (!g_cmd_fg_async)
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

static int terminal_restore_register_fn(scheduler_t *sched, const launch_intent_t *intent)
{
        (void)intent;
        return terminal_task_register(sched);
}

int terminal_task_register_restore_descriptor(void)
{
        static const restore_task_descriptor_t desc = {
                .task_id = AO_TERMINAL,
                .task_class = TASK_CLASS_RESTART_ONLY,
                .state_version = 0u,
                .min_state_len = 0u,
                .max_state_len = 0u,
                .register_fn = terminal_restore_register_fn,
                .get_state_fn = 0,
                .restore_fn = 0,
                .ui_rehydrate_fn = 0
        };
        return restore_registry_register_descriptor(&desc);
}

static int cmd_restore_register_fn(scheduler_t *sched, const launch_intent_t *intent)
{
        (void)intent;
        return cmd_task_register(sched);
}

int cmd_task_register_restore_descriptor(void)
{
        static const restore_task_descriptor_t desc = {
                .task_id = AO_CMD,
                .task_class = TASK_CLASS_RESTART_ONLY,
                .state_version = 0u,
                .min_state_len = 0u,
                .max_state_len = 0u,
                .register_fn = cmd_restore_register_fn,
                .get_state_fn = 0,
                .restore_fn = 0,
                .ui_rehydrate_fn = 0
        };
        return restore_registry_register_descriptor(&desc);
}
