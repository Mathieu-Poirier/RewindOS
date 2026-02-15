#include "../../include/lineio_async.h"
#include "../../include/parse.h"
#include "../../include/uart.h"
#include "../../include/uart_async.h"
#include "../../include/systick.h"
#include "../../include/sd.h"
#include "../../include/sd_async.h"
#include "../../include/driver_common.h"
#include "../../include/scheduler.h"
#include "../../include/task_spec.h"
#include "../../include/task_ids.h"
#include "../../include/task_signals.h"

#define MAX_ARGUMENTS 8
#define TICKS_PER_SEC 1000u
#define SD_DUMP_BYTES 64u
#define SD_READ_MAX_BLOCKS 4u

static uint32_t sd_buf_words[SD_BLOCK_SIZE / 4u];

typedef struct {
        shell_state_t shell;
} terminal_task_ctx_t;

static terminal_task_ctx_t g_term_ctx;
static event_t g_term_queue_storage[8];

static void uart_put_s32(int v)
{
        if (v < 0)
        {
                uart_putc('-');
                uart_put_u32((uint32_t)(-v));
                return;
        }
        uart_put_u32((uint32_t)v);
}

static void uart_put_hex8(uint8_t v)
{
        static const char *hx = "0123456789ABCDEF";
        uart_putc(hx[(v >> 4) & 0xF]);
        uart_putc(hx[v & 0xF]);
}

static void sd_dump_bytes(const uint8_t *buf, uint32_t count)
{
        for (uint32_t i = 0; i < count; i++)
        {
                uart_put_hex8(buf[i]);
                if ((i & 0x0Fu) == 0x0Fu)
                        uart_puts("\r\n");
                else
                        uart_putc(' ');
        }
        if ((count & 0x0Fu) != 0u)
                uart_puts("\r\n");
}

static void sd_print_info(void)
{
        const sd_info_t *info = sd_get_info();
        if (!info->initialized)
        {
                uart_puts("sd not initialized\r\n");
                return;
        }
        uart_puts("rca=");
        uart_put_hex32(info->rca);
        uart_puts(" ocr=");
        uart_put_hex32(info->ocr);
        uart_puts("\r\n");
        uart_puts("capacity=");
        uart_put_u32(info->capacity_blocks / 2048u);
        uart_puts("MB hc=");
        uart_put_u32(info->high_capacity);
        uart_puts(" bus=");
        uart_put_u32(info->bus_width);
        uart_puts("bit\r\n");
}

static void term_dispatch(char *line)
{
        char *argv[MAX_ARGUMENTS];
        int argc = tokenize(line, argv, MAX_ARGUMENTS);

        if (argc == 0)
                return;

        /* simple built-ins */
        if (streq(argv[0], "help"))
        {
                uart_puts("\r\n");
                uart_puts("  System\r\n");
                uart_puts("    reboot            Reboot system\r\n");
                uart_puts("    uptime            Show uptime\r\n");
                uart_puts("    ticks             Show tick count\r\n");
                uart_puts("\r\n");
                uart_puts("  SD Card\r\n");
                uart_puts("    sdinit            Initialize SD card\r\n");
                uart_puts("    sdinfo            Show card info\r\n");
                uart_puts("    sdtest            Init + read test\r\n");
                uart_puts("    sdread <lba> [n]  Read blocks (n<=4)\r\n");
                uart_puts("    sdaread <lba>     Async read one block\r\n");
                uart_puts("    sddetect          Check card presence\r\n");
                uart_puts("\r\n");
                uart_puts("  Debug\r\n");
                uart_puts("    md <addr> [n]     Memory dump\r\n");
                uart_puts("    echo <text>       Echo text\r\n");
                uart_puts("\r\n");
                return;
        }

        if (streq(argv[0], "echo"))
        {
                /* echo argv[1..] separated by spaces */
                for (int i = 1; i < argc; i++)
                {
                        uart_puts(argv[i]);
                        if (i + 1 < argc)
                                uart_putc(' ');
                }
                uart_puts("\r\n");
                return;
        }

        if (streq(argv[0], "reboot"))
        {
                uart_puts("rebooting...\r\n");
                uart_flush_tx();
                /* System reset: AIRCR */
                volatile uint32_t *AIRCR = (uint32_t *)0xE000ED0Cu;
                const uint32_t VECTKEY = 0x5FAu << 16;
                *AIRCR = VECTKEY | (1u << 2); /* SYSRESETREQ */

                for (;;)
                {
                }
        }

        if (streq(argv[0], "ticks"))
        {
                uint32_t t = systick_now();
                uart_puts("ticks=");
                uart_put_u32(t);
                uart_puts("\r\n");
                return;
        }

        if (streq(argv[0], "uptime"))
        {
                uint32_t t = systick_now();
                uint32_t ms = (t * 1000u) / TICKS_PER_SEC;
                uart_puts("uptime_ms=");
                uart_put_u32(ms);
                uart_puts("\r\n");
                return;
        }

        if (streq(argv[0], "md"))
        {
                if (argc < 2)
                {
                        uart_puts("usage: md <addr> [n]\r\n");
                        return;
                }

                uint32_t addr, n = 1;
                if (!parse_u32(argv[1], &addr))
                {
                        uart_puts("md: bad addr\r\n");
                        return;
                }
                if (argc >= 3 && !parse_u32(argv[2], &n))
                {
                        uart_puts("md: bad n\r\n");
                        return;
                }
                if (n == 0)
                        n = 1;
                if (n > 64)
                        n = 64; /* safety cap */

                volatile uint32_t *p = (volatile uint32_t *)addr;

                for (uint32_t i = 0; i < n; i++)
                {
                        uart_put_hex32((uint32_t)(addr + i * 4u));
                        uart_puts(": ");
                        uart_put_hex32(p[i]);
                        uart_puts("\r\n");
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
                        uart_puts("sdinit: ok\r\n");
                        return;
                }
                uart_puts("sdinit: err=");
                uart_put_s32(rc);
                uart_puts("\r\n");
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
                        uart_puts("sd: present\r\n");
                else
                        uart_puts("sd: not present\r\n");
                return;
        }

        if (streq(argv[0], "sdtest"))
        {
                uart_puts("sdtest: initializing...\r\n");
                sd_use_pll48(1);
                sd_set_data_clkdiv(SD_CLKDIV_FAST);
                int rc = sd_init();
                if (rc != SD_OK)
                {
                        uart_puts("sdtest: init failed err=");
                        uart_put_s32(rc);
                        uart_puts("\r\n");
                        return;
                }
                const sd_info_t *info = sd_get_info();
                uart_puts("sdtest: ok ");
                uart_put_u32(info->capacity_blocks / 2048u);
                uart_puts("MB\r\n");

                uart_puts("sdtest: reading block 0...\r\n");
                rc = sd_read_blocks(0, 1, sd_buf_words);
                if (rc != SD_OK)
                {
                        uart_puts("sdtest: read err=");
                        uart_put_s32(rc);
                        uart_puts("\r\n");
                        return;
                }
                const uint8_t *buf = (const uint8_t *)sd_buf_words;
                uart_puts("sdtest: sig=");
                uart_put_hex8(buf[510]);
                uart_put_hex8(buf[511]);
                if (buf[510] == 0x55 && buf[511] == 0xAA)
                        uart_puts(" (MBR)\r\n");
                else
                        uart_puts("\r\n");
                uart_puts("sdtest: PASS\r\n");
                return;
        }

        if (streq(argv[0], "sdread"))
        {
                if (argc < 2)
                {
                        uart_puts("usage: sdread <lba> [count]\r\n");
                        return;
                }
                uint32_t lba = 0;
                uint32_t count = 1;
                if (!parse_u32(argv[1], &lba))
                {
                        uart_puts("sdread: bad lba\r\n");
                        return;
                }
                if (argc >= 3 && !parse_u32(argv[2], &count))
                {
                        uart_puts("sdread: bad count\r\n");
                        return;
                }
                if (count == 0)
                        count = 1;
                if (count > SD_READ_MAX_BLOCKS)
                        count = SD_READ_MAX_BLOCKS;

                for (uint32_t i = 0; i < count; i++)
                {
                        int rc = sd_read_blocks(lba + i, 1, sd_buf_words);
                        if (rc != SD_OK)
                        {
                                uart_puts("sdread: err=");
                                uart_put_s32(rc);
                                uart_puts("\r\n");
                                return;
                        }
                        uart_puts("lba ");
                        uart_put_u32(lba + i);
                        uart_puts(":\r\n");
                        sd_dump_bytes((const uint8_t *)sd_buf_words, SD_DUMP_BYTES);
                }
                return;
        }

        if (streq(argv[0], "sdaread"))
        {
                if (argc < 2)
                {
                        uart_puts("usage: sdaread <lba>\r\n");
                        return;
                }

                uint32_t lba = 0;
                if (!parse_u32(argv[1], &lba))
                {
                        uart_puts("sdaread: bad lba\r\n");
                        return;
                }
                if (sd_async_poll() == DRV_IN_PROGRESS)
                {
                        uart_puts("sdaread: busy\r\n");
                        return;
                }

                int rc = sd_async_read_start(lba, 1, sd_buf_words);
                if (rc != SD_OK)
                {
                        uart_puts("sdaread: err=");
                        uart_put_s32(rc);
                        uart_puts("\r\n");
                        return;
                }
                uart_puts("sdaread: started\r\n");
                return;
        }

        uart_puts("unknown cmd: ");
        uart_puts(argv[0]);
        uart_puts("\r\n");
}

static void terminal_task_dispatch(ao_t *self, const event_t *e)
{
        (void)self;
        if (e == 0 || e->sig != TERM_SIG_UART_RX_READY)
                return;

        for (;;) {
                while (uart_rx_available()) {
                        shell_tick(&g_term_ctx.shell, term_dispatch);
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
