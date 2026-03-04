#include "../../include/restore_sim.h"
#include "../../include/restore_loader.h"
#include "../../include/checkpoint_v2.h"

typedef struct {
    uint32_t seq;
    uint16_t action;
    uint16_t entry_state;
    checkpoint_v2_region_t region;
    uint8_t blob[RESTORE_SIM_MAX_BLOB];
} restore_sim_item_t;

typedef struct {
    uint16_t read_idx;
    uint16_t write_idx;
    uint16_t count;
    uint16_t reserved;
    uint32_t generation;
    uint32_t next_seq;
} restore_sim_queue_t;

enum {
    RESTORE_SIM_ACTION_APPLY_REGION = 1u
};

enum {
    RESTORE_SIM_STATE_PENDING = 0u,
    RESTORE_SIM_STATE_DONE = 1u,
    RESTORE_SIM_STATE_FAILED = 2u
};

static restore_sim_item_t g_items[RESTORE_SIM_MAX_ITEMS];
static restore_sim_queue_t g_q;

int restore_sim_reset(void)
{
    g_q.read_idx = 0u;
    g_q.write_idx = 0u;
    g_q.count = 0u;
    g_q.generation++;
    g_q.next_seq = 1u;
    return SCHED_OK;
}

int restore_sim_enqueue(uint16_t region_id, uint16_t state_version, const void *blob, uint32_t len)
{
    restore_sim_item_t *it;
    uint32_t i;

    if (g_q.count >= RESTORE_SIM_MAX_ITEMS) {
        return SCHED_ERR_FULL;
    }
    if (len > RESTORE_SIM_MAX_BLOB) {
        return SCHED_ERR_PARAM;
    }
    if (len != 0u && blob == 0) {
        return SCHED_ERR_PARAM;
    }

    it = &g_items[g_q.write_idx];
    it->seq = g_q.next_seq++;
    it->action = RESTORE_SIM_ACTION_APPLY_REGION;
    it->entry_state = RESTORE_SIM_STATE_PENDING;
    it->region.region_id = region_id;
    it->region.state_version = state_version;
    it->region.offset = 0u;
    it->region.length = len;
    it->region.crc32 = 0u;

    for (i = 0u; i < len; i++) {
        it->blob[i] = ((const uint8_t *)blob)[i];
    }

    g_q.write_idx = (uint16_t)((g_q.write_idx + 1u) % RESTORE_SIM_MAX_ITEMS);
    g_q.count++;
    return SCHED_OK;
}

int restore_sim_apply(scheduler_t *sched, uint32_t *out_applied, uint32_t *out_skipped, uint32_t *out_failed)
{
    checkpoint_v2_region_t regions[RESTORE_SIM_MAX_ITEMS];
    uint8_t payload[RESTORE_SIM_MAX_ITEMS * RESTORE_SIM_MAX_BLOB];
    uint32_t payload_used = 0u;
    uint16_t idx = g_q.read_idx;
    uint16_t remain = g_q.count;
    uint32_t n = 0u;

    /* Keep analyzers happy without pulling in libc memset in freestanding mode. */
    regions[0].region_id = 0u;
    payload[0] = 0u;

    while (remain > 0u) {
        restore_sim_item_t *it = &g_items[idx];
        uint32_t j;
        if (it->action == RESTORE_SIM_ACTION_APPLY_REGION &&
            it->entry_state == RESTORE_SIM_STATE_PENDING) {
            regions[n] = it->region;
            regions[n].offset = payload_used;
            for (j = 0u; j < it->region.length; j++) {
                payload[payload_used + j] = it->blob[j];
            }
            payload_used += it->region.length;
            n++;
        }
        idx = (uint16_t)((idx + 1u) % RESTORE_SIM_MAX_ITEMS);
        remain--;
    }

    if (n == 0u) {
        if (out_applied) *out_applied = 0u;
        if (out_skipped) *out_skipped = 0u;
        if (out_failed) *out_failed = 0u;
        return RESTORE_LOADER_OK;
    }

    {
        int rc = restore_loader_apply_regions(sched,
                                              regions, (uint16_t)n,
                                              payload, payload_used,
                                              out_applied, out_skipped, out_failed);
        idx = g_q.read_idx;
        remain = g_q.count;
        while (remain > 0u) {
            g_items[idx].entry_state = (rc == RESTORE_LOADER_OK)
                                           ? RESTORE_SIM_STATE_DONE
                                           : RESTORE_SIM_STATE_FAILED;
            idx = (uint16_t)((idx + 1u) % RESTORE_SIM_MAX_ITEMS);
            remain--;
        }
        if (rc == RESTORE_LOADER_OK) {
            g_q.read_idx = g_q.write_idx;
            g_q.count = 0u;
            g_q.generation++;
        }
        return rc;
    }
}

uint32_t restore_sim_pending(void)
{
    return g_q.count;
}

uint32_t restore_sim_generation(void)
{
    return g_q.generation;
}
