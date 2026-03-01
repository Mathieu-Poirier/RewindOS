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

__attribute__((weak)) void terminal_replay_drain(void)
{
}
