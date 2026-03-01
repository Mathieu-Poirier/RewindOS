#include "../../include/lineio_async.h"
#include "../../include/parse.h"
#include "../../include/uart_async.h"
#include "../../include/uart.h"
#include "../../include/systick.h"
#include "../../include/sd.h"
#include "../../include/sd_async.h"
#include "../../include/sd_task.h"
#include "../../include/counter_task.h"
#include "../../include/snapshot_task.h"
#include "../../include/console.h"
#include "../../include/log.h"
#include "../../include/cmd_context.h"
#include "../../include/journal.h"
#include "../../include/scheduler.h"
#include "../../include/task_spec.h"
#include "../../include/task_ids.h"
#include "../../include/task_signals.h"
#include "../../include/shutdown.h"
#include "../../include/terminal.h"
#include "../../include/panic.h"

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
        (void)journal_capture_io_owner(owner_ao, 1u);
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
        (void)journal_capture_io_owner(owner_ao, 0u);
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
                console_puts("    snapnow           Trigger snapshot now\r\n");
                console_puts("    snapstat          Show snapshot task status\r\n");
                console_puts("    snapls            List snapshot slots + target\r\n");
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

                /* Quiesce the SD card so the next boot finds it in a
                 * clean state.  Disable SDMMC IRQ, stop the data path,
                 * and wait for the card to return to TRAN. */
                __asm__ volatile("cpsid i" ::: "memory");
                {
                        extern void nvic_disable_irq(uint32_t);
                        extern void nvic_clear_pending(uint32_t);
                        nvic_disable_irq(49u);   /* SDMMC1_IRQn */
                        nvic_clear_pending(49u);
                        volatile uint32_t *SDMMC_MASK  = (uint32_t *)0x40012C3Cu;
                        volatile uint32_t *SDMMC_DCTRL = (uint32_t *)0x40012C2Cu;
                        volatile uint32_t *SDMMC_ICR   = (uint32_t *)0x40012C38u;
                        *SDMMC_MASK  = 0u;
                        *SDMMC_DCTRL = 0u;
                        *SDMMC_ICR   = 0xFFFFFFFFu;
                        (void)sd_wait_card_ready();
                }

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

        if (streq(argv[0], "snapnow"))
        {
                int rc = snapshot_task_request_now();
                if (rc != SCHED_OK)
                {
                        console_puts("snapnow: err=");
                        uart_put_s32(rc);
                        console_puts("\r\n");
                }
                return;
        }

        if (streq(argv[0], "snapstat"))
        {
                snapshot_task_stats_t st;
                snapshot_task_get_stats(&st);
                console_puts("snapshot enabled=");
                console_put_u32(st.enabled);
                console_puts(" busy=");
                console_put_u32(st.busy);
                console_puts(" restore_mode=");
                console_put_u32(st.restore_mode);
                console_puts(" restore_candidate=");
                console_put_u32(st.restore_has_candidate);
                console_puts(" interval_s=");
                console_put_u32(st.interval_s);
                console_puts("\r\n");
                console_puts("restore slot=");
                console_put_u32(st.restore_slot);
                console_puts(" seq=");
                console_put_u32(st.restore_seq);
                console_puts("\r\n");
                console_puts("last err=");
                uart_put_s32(st.last_err);
                console_puts(" slot=");
                console_put_u32(st.last_slot);
                console_puts(" seq=");
                console_put_u32(st.last_seq);
                console_puts("\r\n");
                console_puts("last capture_tick=");
                console_put_u32(st.last_capture_tick);
                console_puts(" ready_bitmap=0x");
                console_put_hex32(st.last_ready_bitmap);
                console_puts("\r\n");
                console_puts("totals ok=");
                console_put_u32(st.saves_ok);
                console_puts(" err=");
                console_put_u32(st.saves_err);
                console_puts(" next_tick=");
                console_put_u32(st.next_tick);
                console_puts("\r\n");
                return;
        }

        if (streq(argv[0], "snapls"))
        {
                int rc = snapshot_task_list_slots();
                if (rc != SCHED_OK)
                {
                        console_puts("snapls: err=");
                        uart_put_s32(rc);
                        console_puts(" cmd=");
                        console_put_hex32(sd_last_cmd());
                        console_puts(" sta=");
                        console_put_hex32(sd_last_sta());
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
                console_puts("last_error="); uart_put_s32(sd_last_error()); console_puts("\r\n");
                console_puts("last_cmd=0x"); console_put_hex32(sd_last_cmd()); console_puts("\r\n");
                console_puts("last_sta=0x"); console_put_hex32(sd_last_sta()); console_puts("\r\n");
                console_puts("error_code="); uart_put_s32(g_sd_ctx.error_code); console_puts("\r\n");
                console_puts("error_detail=0x"); console_put_hex32(g_sd_ctx.error_detail); console_puts("\r\n");
                console_puts("status="); console_put_u32(g_sd_ctx.status); console_puts("\r\n");
                console_puts("operation="); console_put_u32(g_sd_ctx.operation); console_puts("\r\n");
                console_puts("wait_ready_calls="); console_put_u32(sd_dbg_wait_ready_calls()); console_puts("\r\n");
                console_puts("wait_ready_ok="); console_put_u32(sd_dbg_wait_ready_ok()); console_puts("\r\n");
                console_puts("wait_ready_timeout="); console_put_u32(sd_dbg_wait_ready_timeout()); console_puts("\r\n");
                console_puts("wait_ready_failfast="); console_put_u32(sd_dbg_wait_ready_cmd_fail_fast()); console_puts("\r\n");
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

        /* Skip empty / whitespace-only lines (noise, accidental Enter). */
        {
                const char *p = line;
                while (*p == ' ' || *p == '\t') { p++; }
                if (*p == '\0')
                {
                        console_puts(g_term_ctx.shell.prompt_str);
                        return;
                }
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
        (void)journal_capture_input_byte((uint8_t)c);

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

void terminal_replay_drain(void)
{
        while (uart_rx_available()) {
                if (!term_stdin_owner_valid())
                        shell_tick(&g_term_ctx.shell, term_enqueue_dispatch);
                else
                        term_dispatch_owned_input();
        }
}

static void terminal_task_dispatch(ao_t *self, const event_t *e)
{
        (void)self;
        PANIC_IF(e == 0, "terminal dispatch: null event");

        if (e->sig == TERM_SIG_REPRINT_PROMPT) {
                if (!term_stdin_owner_valid()) {
                        console_puts(g_term_ctx.shell.prompt_str);
                        if (g_term_ctx.shell.len > 0u)
                                console_write(g_term_ctx.shell.line,
                                              (uint16_t)g_term_ctx.shell.len);
                }
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
