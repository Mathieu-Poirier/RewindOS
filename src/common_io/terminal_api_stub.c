#include "../../include/terminal.h"

__attribute__((weak)) int terminal_stdin_acquire(uint8_t owner_ao, uint8_t mode)
{
    (void)owner_ao;
    (void)mode;
    return SCHED_ERR_NOT_FOUND;
}

__attribute__((weak)) int terminal_stdin_release(uint8_t owner_ao)
{
    (void)owner_ao;
    return SCHED_ERR_NOT_FOUND;
}

__attribute__((weak)) void terminal_task_systick_hook(void)
{
}

__attribute__((weak)) void terminal_ckpt_set_interval_ms(uint32_t interval_ms)
{
    (void)interval_ms;
}

__attribute__((weak)) uint32_t terminal_ckpt_get_interval_ms(void)
{
    return 0u;
}
