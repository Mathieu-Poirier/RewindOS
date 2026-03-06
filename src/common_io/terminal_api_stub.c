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

__attribute__((weak)) int terminal_ckpt_save_sd_once(uint32_t *out_lba,
                                                     uint32_t *out_slot,
                                                     uint32_t *out_seq,
                                                     uint32_t *out_regions)
{
    if (out_lba) *out_lba = 0u;
    if (out_slot) *out_slot = 0u;
    if (out_seq) *out_seq = 0u;
    if (out_regions) *out_regions = 0u;
    return SCHED_ERR_NOT_FOUND;
}

__attribute__((weak)) int terminal_ckpt_load_latest_sd(scheduler_t *sched,
                                                       uint32_t *out_applied,
                                                       uint32_t *out_skipped,
                                                       uint32_t *out_failed,
                                                       uint32_t *out_seq)
{
    (void)sched;
    if (out_applied) *out_applied = 0u;
    if (out_skipped) *out_skipped = 0u;
    if (out_failed) *out_failed = 0u;
    if (out_seq) *out_seq = 0u;
    return SCHED_ERR_NOT_FOUND;
}
