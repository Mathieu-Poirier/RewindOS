#pragma once

#include "stdint.h"

#define BOOT_HANDOFF_MAGIC 0x48444E52u /* "RNDH" */
#define BOOT_HANDOFF_VERSION 2u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t ckpt_interval_ms;
    uint32_t ckpt_interval_inv;
    uint32_t boot_restore_enabled;
    uint32_t boot_restore_enabled_inv;
} boot_handoff_blob_t;

typedef struct {
    uint32_t ckpt_interval_ms;
    uint32_t boot_restore_enabled;
} boot_handoff_cfg_t;

void boot_handoff_clear(void);
void boot_handoff_publish(const boot_handoff_cfg_t *cfg);
int  boot_handoff_consume(boot_handoff_cfg_t *out);
