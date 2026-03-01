#include "../../include/gpio.h"
#include "../../include/uart.h"
#include "../../include/uart_async.h"
#include "../../include/sd_async.h"
#include "../../include/bump.h"
#include "../../include/clock.h"
#include "../../include/stdint.h"
#include "../../include/scheduler.h"
#include "../../include/console.h"
#include "../../include/terminal.h"
#include "../../include/sd_task.h"
#include "../../include/snapshot_task.h"
#include "../../include/counter_task.h"
#include "../../include/rewind_restore.h"
#include "../../include/rwfs.h"
#include "../../include/rwos_disk.h"
#include "../../include/task_ids.h"
#include "../../include/task_signals.h"
#include "../../include/panic.h"

extern void systick_init(uint32_t ticks);
extern int uart_async_resume_after_restore(void);
extern void uart_async_resume_enable_irq(void);

void HardFault_Handler(void)
{
        __asm__ volatile("cpsid i" ::: "memory");
        uart_puts("\r\n!!! HARDFAULT !!!\r\n");
        uart_flush_tx();
        for (;;) __asm__ volatile("bkpt #0");
}

static void idle_hook(void)
{
        __asm__ volatile("wfi");
}

static scheduler_t g_sched_main;

/* Build sentinel — written on cold boot, checked on resume.
 * If the snapshot was created by a different build, the stored value
 * won't match and we fall back to cold boot to avoid dispatching
 * through stale function pointers.                                   */
extern int main(void);
static uintptr_t g_build_sentinel;
#define BUILD_SENTINEL_VALUE ((uintptr_t)main)

static uint32_t safe_apb2_hz(void)
{
        uint32_t hz = clock_apb2_hz();
        if (hz < 1000000u || hz > 120000000u) {
                hz = 16000000u;
        }
        return hz;
}

static void rwfs_mount_best_effort(void)
{
        (void)rwfs_init_or_mount();
}

static int resume_sched_valid(void)
{
        if (g_sched_main.table[AO_TERMINAL] == 0 ||
            g_sched_main.table[AO_CMD] == 0 ||
            g_sched_main.table[AO_CONSOLE] == 0) {
                return 0;
        }
        if (g_sched_main.table[AO_TERMINAL]->dispatch == 0 ||
            g_sched_main.table[AO_CMD]->dispatch == 0 ||
            g_sched_main.table[AO_CONSOLE]->dispatch == 0) {
                return 0;
        }
        return 1;
}

static void main_cold_boot(void)
{
        uart_puts("main: cold entry\r\n");
        full_clock_init();
        enable_gpio_clock();
        uart_init(safe_apb2_hz(), 115200u);
        uart_puts("main: uart ok\r\n");
        systick_init(215999);
        uart_puts("main: systick ok\r\n");

        g_build_sentinel = BUILD_SENTINEL_VALUE;
        bump_init();
        uart_puts("main: bump ok\r\n");
        uart_async_init();
        uart_puts("main: uart_async ok\r\n");
        sd_async_init();
        uart_puts("main: sd_async ok\r\n");

        sched_init(&g_sched_main, idle_hook);
        uart_puts("main: sched init ok\r\n");
        if (console_task_register(&g_sched_main) != SCHED_OK)
        {
                PANIC("console task init failed");
        }
        uart_puts("main: console reg ok\r\n");
        if (terminal_task_register(&g_sched_main) != SCHED_OK)
        {
                PANIC("terminal task init failed");
        }
        uart_puts("main: terminal reg ok\r\n");
        if (cmd_task_register(&g_sched_main) != SCHED_OK)
        {
                PANIC("cmd task init failed");
        }
        uart_puts("main: cmd reg ok\r\n");
        if (sd_task_register(&g_sched_main) != SCHED_OK)
        {
                PANIC("sd task init failed");
        }
        uart_puts("main: sd reg ok\r\n");
        if (snapshot_task_register(&g_sched_main) != SCHED_OK)
        {
                PANIC("snapshot task init failed");
        }
        uart_puts("main: snapshot reg ok\r\n");
        rwos_disk_init();
        uart_puts("main: disk init done\r\n");
        rwfs_mount_best_effort();
        uart_puts("main: rwfs mount done\r\n");
        snapshot_task_boot_init();
        uart_puts("main: boot complete\r\n");
        sched_run(&g_sched_main);
}

void rewind_resume_entry(void)
{
        rewind_handoff_t handoff;
        rewind_handoff_read(&handoff);

        uart_puts("main: resume entry\r\n");
        full_clock_init();
        enable_gpio_clock();
        uart_init(safe_apb2_hz(), 115200u);
        uart_puts("main: resume uart ok\r\n");

        /* ---- Phase 1: validate restored state, NO ISRs yet ---- */
        if (g_build_sentinel != BUILD_SENTINEL_VALUE) {
                uart_puts("main: build mismatch -> cold boot\r\n");
                rewind_handoff_clear();
                main_cold_boot();
                return;
        }
        if (!uart_async_resume_after_restore() || !resume_sched_valid()) {
                uart_puts("main: sched invalid -> cold boot\r\n");
                rewind_handoff_clear();
                main_cold_boot();
                return;
        }
        uart_puts("main: validate ok\r\n");

        /* Disarm systick hooks BEFORE starting systick — the restored
         * g_counter_sched / g_snapshot_sched may reference stale AOs.  */
        counter_task_disarm_hook();
        snapshot_task_disarm_hook();

        console_reset_after_restore();
        uart_puts("main: console reset ok\r\n");
        /* Flush stale AO event queues — validate pointers first because the
         * snapshot may come from a different build with shifted addresses.   */
        uart_puts("main: flush begin\r\n");
        for (uint32_t _fi = 0; _fi < 32u; _fi++) {
                uintptr_t p = (uintptr_t)g_sched_main.table[_fi];
                if (p == 0) continue;
                /* Validate pointer is inside SRAM [0x20000000, 0x20050000) */
                if (p < 0x20000000u || p >= 0x20050000u) {
                        uart_puts("  ao");
                        uart_put_u32(_fi);
                        uart_puts(" BAD p=");
                        uart_put_hex32((uint32_t)p);
                        uart_puts(" -> null\r\n");
                        g_sched_main.table[_fi] = 0;
                        continue;
                }
                ao_t *ao = g_sched_main.table[_fi];
                /* Also validate queue buffer pointer */
                uintptr_t qp = (uintptr_t)ao->q.buf;
                if (qp != 0 && (qp < 0x20000000u || qp >= 0x20050000u)) {
                        uart_puts("  ao");
                        uart_put_u32(_fi);
                        uart_puts(" BAD q.buf=");
                        uart_put_hex32((uint32_t)qp);
                        uart_puts(" -> null\r\n");
                        g_sched_main.table[_fi] = 0;
                        continue;
                }
                /* Validate dispatch function pointer is in flash */
                uintptr_t dp = (uintptr_t)ao->dispatch;
                if (dp < 0x08000000u || dp >= 0x08100000u) {
                        uart_puts("  ao");
                        uart_put_u32(_fi);
                        uart_puts(" BAD dispatch=");
                        uart_put_hex32((uint32_t)dp);
                        uart_puts(" -> null\r\n");
                        g_sched_main.table[_fi] = 0;
                        continue;
                }
                ao->q.head = 0;
                ao->q.tail = 0;
                ao->q.count = 0;
                uart_puts("  ao");
                uart_put_u32(_fi);
                uart_puts(" ok\r\n");
        }
        g_sched_main.ready_bitmap = 0u;
        uart_puts("main: flush done\r\n");

        /* ---- Phase 2: NOW safe to start ISRs ---- */
        systick_init(215999);
        uart_async_resume_enable_irq();
        sd_async_init();
        uart_puts("main: hw resume ok\r\n");

        /* Re-arm counter & snapshot systick hooks now that systick is running
         * and the scheduler table has been validated.                        */
        counter_task_rearm_hook(&g_sched_main);
        snapshot_task_rearm_hook(&g_sched_main);
        uart_puts("main: hooks rearmed\r\n");

        rwos_disk_init(); /* re-read disk table over snapshot-restored SRAM state */

        uart_puts("main: journal replay start\r\n");
        uart_puts("  jrn_start=");
        uart_put_hex32(handoff.journal_start_lba);
        uart_puts(" jrn_blks=");
        uart_put_hex32(handoff.journal_blocks);
        uart_puts(" seq=");
        uart_put_u32(handoff.seq);
        uart_puts("\r\n");
        rewind_journal_replay(handoff.seq,
                              handoff.journal_start_lba,
                              handoff.journal_blocks);
        uart_puts("main: journal replay done\r\n");
        rwfs_mount_best_effort();
        uart_puts("main: rwfs mount done\r\n");
        rewind_handoff_clear();
        /* Re-print the shell prompt so the user sees a cursor after resume. */
        {
                int rc = sched_post(&g_sched_main, AO_TERMINAL,
                                    &(event_t){ .sig = TERM_SIG_REPRINT_PROMPT });
                uart_puts("main: prompt post rc=");
                uart_put_u32((uint32_t)(rc < 0 ? -rc : rc));
                uart_puts("\r\n");
        }
        uart_puts("main: ready=");
        uart_put_hex32(g_sched_main.ready_bitmap);
        uart_puts("\r\n");
        uart_puts("main: sched_run\r\n");
        sched_run(&g_sched_main);
}

int main(void)
{
        if (rewind_handoff_resume_requested()) {
                rewind_resume_entry();
        } else {
                main_cold_boot();
        }
        for (;;)
        {
        }
}
