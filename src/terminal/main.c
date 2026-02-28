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
#include "../../include/panic.h"

extern void systick_init(uint32_t ticks);

static void idle_hook(void)
{
        __asm__ volatile("wfi");
}

int main(void)
{
        scheduler_t sched;

        full_clock_init();
        enable_gpio_clock();
        uart_init(108000000u, 115200u);
        systick_init(215999);

        bump_init();
        uart_async_init();
        sd_async_init();

        sched_init(&sched, idle_hook);
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

        sched_run(&sched);
        for (;;)
        {
        }
}
