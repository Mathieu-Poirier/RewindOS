#include "../../include/restore_loader.h"
#include "../../include/restore_registry.h"

static restore_loader_stats_t g_restore_loader_stats;

static int bitmap_test_and_set(uint32_t *bits, uint8_t task_id)
{
    uint8_t word = (uint8_t)(task_id >> 5);
    uint8_t bit = (uint8_t)(task_id & 31u);
    uint32_t mask = (uint32_t)(1u << bit);
    int was_set = (bits[word] & mask) != 0u;
    bits[word] |= mask;
    return was_set;
}

int restore_loader_apply_regions(scheduler_t *sched,
                                 const checkpoint_v2_region_t *regions,
                                 uint16_t region_count,
                                 const uint8_t *payload_base,
                                 uint32_t payload_len,
                                 uint32_t *out_applied,
                                 uint32_t *out_skipped,
                                 uint32_t *out_failed)
{
    uint32_t applied = 0u;
    uint32_t skipped = 0u;
    uint32_t failed = 0u;
    uint32_t registered[8];
    uint16_t i;
    uint16_t j;

    if (sched == 0) {
        g_restore_loader_stats.calls++;
        g_restore_loader_stats.last_rc = RESTORE_LOADER_ERR_PARAM;
        return RESTORE_LOADER_ERR_PARAM;
    }
    if (regions == 0 && region_count != 0u) {
        g_restore_loader_stats.calls++;
        g_restore_loader_stats.last_rc = RESTORE_LOADER_ERR_PARAM;
        return RESTORE_LOADER_ERR_PARAM;
    }
    if (payload_base == 0 && payload_len != 0u) {
        g_restore_loader_stats.calls++;
        g_restore_loader_stats.last_rc = RESTORE_LOADER_ERR_PARAM;
        return RESTORE_LOADER_ERR_PARAM;
    }
    for (j = 0u; j < 8u; j++) {
        registered[j] = 0u;
    }

    for (i = 0u; i < region_count; i++) {
        const checkpoint_v2_region_t *r = &regions[i];
        const restore_task_descriptor_t *desc =
            restore_registry_find((uint8_t)r->region_id);
        const uint8_t *blob = 0;
        uint32_t end_off = 0u;
        int already_registered;
        int rc;

        if (desc == 0) {
            failed++;
            continue;
        }

        already_registered = bitmap_test_and_set(registered, desc->task_id);
        if (!already_registered) {
            rc = desc->register_fn(sched, 0);
            if (rc != SCHED_OK && rc != SCHED_ERR_EXISTS) {
                failed++;
                continue;
            }
        }

        if (desc->task_class == TASK_CLASS_NON_RESTORABLE) {
            skipped++;
            continue;
        }
        if (desc->task_class == TASK_CLASS_RESTART_ONLY) {
            skipped++;
            continue;
        }

        if (desc->state_version != r->state_version) {
            failed++;
            continue;
        }
        if (r->length < desc->min_state_len || r->length > desc->max_state_len) {
            failed++;
            continue;
        }
        if (r->length > CKPT_V2_MAX_TASK_STATE_BLOB) {
            failed++;
            continue;
        }

        end_off = r->offset + r->length;
        if (end_off < r->offset || end_off > payload_len) {
            failed++;
            continue;
        }
        blob = payload_base + r->offset;

        if (desc->restore_fn == 0) {
            failed++;
            continue;
        }
        rc = desc->restore_fn(blob, r->length);
        if (rc != SCHED_OK) {
            failed++;
            continue;
        }
        applied++;
    }

    if (out_applied) *out_applied = applied;
    if (out_skipped) *out_skipped = skipped;
    if (out_failed) *out_failed = failed;
    g_restore_loader_stats.calls++;
    g_restore_loader_stats.applied += applied;
    g_restore_loader_stats.skipped += skipped;
    g_restore_loader_stats.failed += failed;

    if (failed != 0u) {
        g_restore_loader_stats.last_rc = RESTORE_LOADER_ERR_RESTORE;
        return RESTORE_LOADER_ERR_RESTORE;
    }
    g_restore_loader_stats.last_rc = RESTORE_LOADER_OK;
    return RESTORE_LOADER_OK;
}

void restore_loader_get_stats(restore_loader_stats_t *out)
{
    if (out == 0) {
        return;
    }
    *out = g_restore_loader_stats;
}
