#include "../../include/boot_handoff.h"

__attribute__((section(".boot_handoff")))
volatile boot_handoff_blob_t g_boot_handoff_blob;

void boot_handoff_clear(void)
{
    g_boot_handoff_blob.magic = 0u;
    g_boot_handoff_blob.version = 0u;
    g_boot_handoff_blob.ckpt_interval_ms = 0u;
    g_boot_handoff_blob.ckpt_interval_inv = 0u;
    g_boot_handoff_blob.boot_restore_enabled = 0u;
    g_boot_handoff_blob.boot_restore_enabled_inv = 0u;
}

void boot_handoff_publish(const boot_handoff_cfg_t *cfg)
{
    uint32_t interval = 0u;
    uint32_t boot_restore_enabled = 0u;
    if (cfg != 0) {
        interval = cfg->ckpt_interval_ms;
        boot_restore_enabled = cfg->boot_restore_enabled ? 1u : 0u;
    }
    g_boot_handoff_blob.magic = BOOT_HANDOFF_MAGIC;
    g_boot_handoff_blob.version = BOOT_HANDOFF_VERSION;
    g_boot_handoff_blob.ckpt_interval_ms = interval;
    g_boot_handoff_blob.ckpt_interval_inv = ~interval;
    g_boot_handoff_blob.boot_restore_enabled = boot_restore_enabled;
    g_boot_handoff_blob.boot_restore_enabled_inv = ~boot_restore_enabled;
}

int boot_handoff_consume(boot_handoff_cfg_t *out)
{
    uint32_t interval;
    uint32_t boot_restore_enabled;
    if (out == 0) {
        return 0;
    }
    if (g_boot_handoff_blob.magic != BOOT_HANDOFF_MAGIC ||
        g_boot_handoff_blob.version != BOOT_HANDOFF_VERSION) {
        return 0;
    }
    interval = g_boot_handoff_blob.ckpt_interval_ms;
    if (g_boot_handoff_blob.ckpt_interval_inv != ~interval) {
        return 0;
    }
    boot_restore_enabled = g_boot_handoff_blob.boot_restore_enabled;
    if (g_boot_handoff_blob.boot_restore_enabled_inv != ~boot_restore_enabled) {
        return 0;
    }
    out->ckpt_interval_ms = interval;
    out->boot_restore_enabled = boot_restore_enabled ? 1u : 0u;
    boot_handoff_clear();
    return 1;
}
