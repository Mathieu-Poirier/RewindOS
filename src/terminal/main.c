#include "../../include/gpio.h"
#include "../../include/uart.h"
#include "../../include/uart_async.h"
#include "../../include/sd_async.h"
#include "../../include/sd.h"
#include "../../include/bump.h"
#include "../../include/clock.h"
#include "../../include/stdint.h"
#include "../../include/scheduler.h"
#include "../../include/console.h"
#include "../../include/terminal.h"
#include "../../include/checkpoint_task.h"
#include "../../include/sd_task.h"
#include "../../include/panic.h"
#include "../../include/counter_task.h"
#include "../../include/restore_registry.h"
#include "../../include/restore_loader.h"
#include "../../include/checkpoint_v2.h"
#include "../../include/task_ids.h"
#include "../../include/boot_handoff.h"

extern void systick_init(uint32_t ticks);

static void idle_hook(void)
{
        __asm__ volatile("wfi");
}

#ifndef RWOS_RESTORE_SELFTEST
#define RWOS_RESTORE_SELFTEST 0
#endif

static void maybe_run_restore_loader_selftest(scheduler_t *sched)
{
#if RWOS_RESTORE_SELFTEST
        counter_task_state_t fake_counter = {
                .active = 0u,
                .bg = 0u,
                .step_pending = 0u,
                .limit = 50u,
                .value = 25u,
                .next_tick = 0u
        };
        uint8_t blob[sizeof(restorable_envelope_t)];
        uint32_t blob_len = (uint32_t)sizeof(blob);
        checkpoint_v2_region_t region;
        uint32_t applied = 0u;
        uint32_t skipped = 0u;
        uint32_t failed = 0u;
        int rc;
        rc = counter_task_encode_restore_envelope(&fake_counter, 0, 0, blob, &blob_len);
        PANIC_IF(rc != SCHED_OK, "restore selftest encode");

        region.region_id = AO_COUNTER;
        region.state_version = 2u;
        region.offset = 0u;
        region.length = blob_len;
        region.crc32 = 0u;

        rc = restore_loader_apply_regions(sched,
                                          &region, 1u,
                                          (const uint8_t *)blob,
                                          blob_len,
                                          &applied, &skipped, &failed);
        PANIC_IF(rc != RESTORE_LOADER_OK, "restore selftest rc");
        PANIC_IF(applied != 1u, "restore selftest applied");
        PANIC_IF(failed != 0u, "restore selftest failed");
        PANIC_IF(skipped != 0u, "restore selftest skipped");
#else
        (void)sched;
#endif
}

static void uart_put_s32_main(int v)
{
        if (v < 0)
        {
                uart_putc('-');
                uart_put_u32((uint32_t)(-v));
                return;
        }
        uart_put_u32((uint32_t)v);
}

static int sd_init_for_boot_restore(void)
{
        sd_detect_init();
        if (!sd_is_detected())
                return 0;
        sd_use_pll48(1);
        sd_set_data_clkdiv(SD_CLKDIV_FAST);
        return sd_init() == SD_OK;
}

int main(void)
{
        scheduler_t sched;
        boot_handoff_cfg_t boot_cfg = {0};
        uint8_t has_boot_cfg = 0u;

        full_clock_init();
        enable_gpio_clock();
        uart_init(108000000u, 115200u);
        systick_init(215999);

        bump_init();
        uart_async_init();
        sd_async_init();
        if (sd_init_for_boot_restore())
        {
                uart_puts("sdinit: auto ok\r\n");
        }
        else
        {
                uart_puts("sdinit: auto unavailable\r\n");
        }

        sched_init(&sched, idle_hook);
        has_boot_cfg = (uint8_t)boot_handoff_consume(&boot_cfg);
        restore_registry_init();
        if (counter_task_register_restore_descriptor() != SCHED_OK)
        {
                PANIC("counter restore descriptor init failed");
        }
        if (console_task_register_restore_descriptor() != SCHED_OK)
        {
                PANIC("console restore descriptor init failed");
        }
        if (terminal_task_register_restore_descriptor() != SCHED_OK)
        {
                PANIC("terminal restore descriptor init failed");
        }
        if (cmd_task_register_restore_descriptor() != SCHED_OK)
        {
                PANIC("cmd restore descriptor init failed");
        }
        if (sd_task_register_restore_descriptor() != SCHED_OK)
        {
                PANIC("sd restore descriptor init failed");
        }
        /* No-op restore path wiring: real SD-backed regions will be passed here later. */
        {
                uint32_t applied = 0u, skipped = 0u, failed = 0u;
                int rrc = restore_loader_apply_regions(&sched,
                                                       0, 0,
                                                       0, 0,
                                                       &applied, &skipped, &failed);
                PANIC_IF(rrc != RESTORE_LOADER_OK, "restore loader bootstrap failed");
        }
        /* Run loader selftest before boot restore so test payloads cannot
         * overwrite/unregister a successfully restored runtime task. */
        maybe_run_restore_loader_selftest(&sched);
        if (has_boot_cfg && boot_cfg.boot_restore_enabled)
        {
                if (sd_is_detected() && sd_get_info()->initialized)
                {
                        uint32_t applied = 0u, skipped = 0u, failed = 0u, seq = 0u;
                        int rrc = terminal_ckpt_load_latest_sd(&sched,
                                                               &applied, &skipped, &failed, &seq);
                        uart_puts("bootrestore: rc=");
                        uart_put_s32_main(rrc);
                        uart_puts(" applied=");
                        uart_put_u32(applied);
                        uart_puts(" skipped=");
                        uart_put_u32(skipped);
                        uart_puts(" failed=");
                        uart_put_u32(failed);
                        uart_puts(" seq=");
                        uart_put_u32(seq);
                        uart_puts("\r\n");
                }
                else
                {
                        uart_puts("bootrestore: sd init failed\r\n");
                }
        }
        if (console_task_register(&sched) != SCHED_OK)
        {
                PANIC("console task init failed");
        }
        if (terminal_task_register(&sched) != SCHED_OK)
        {
                PANIC("terminal task init failed");
        }
        if (cmd_task_register(&sched) != SCHED_OK)
        {
                PANIC("cmd task init failed");
        }
        if (sd_task_register(&sched) != SCHED_OK)
        {
                PANIC("sd task init failed");
        }
        if (checkpoint_task_register(&sched) != SCHED_OK)
        {
                PANIC("checkpoint task init failed");
        }
        if (has_boot_cfg)
        {
                checkpoint_task_set_interval_ms(boot_cfg.ckpt_interval_ms);
        }
        sched_run(&sched);
        for (;;)
        {
        }
}
