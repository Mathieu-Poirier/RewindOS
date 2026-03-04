#pragma once

#include "stdint.h"

/* v2 checkpoint format constants */
#define CKPT_V2_MAGIC 0x32545043u /* "CPT2" */
#define CKPT_V2_FORMAT_VERSION 2u
#define CKPT_V2_MAX_REGIONS 64u
#define CKPT_V2_MAX_TASK_STATE_BLOB 4096u

enum {
    CKPT_SLOT_STATE_PENDING = 0,
    CKPT_SLOT_STATE_COMMITTED = 1
};

/* Fixed-size v2 checkpoint slot header (64 bytes target). */
typedef struct {
    uint32_t magic;
    uint16_t format_version;
    uint16_t header_size;
    uint32_t seq;
    uint32_t tick_at_checkpoint;
    uint8_t slot_id;
    uint8_t state;
    uint16_t region_count;
    uint32_t active_task_bitmap;
    uint8_t stdin_owner;
    uint8_t reserved0[3];
    uint32_t regions_crc32;
    uint32_t header_crc32;
    uint8_t reserved1[32];
} checkpoint_v2_header_t;

typedef struct {
    uint16_t region_id;
    uint16_t state_version;
    uint32_t offset;
    uint32_t length;
    uint32_t crc32;
} checkpoint_v2_region_t;

_Static_assert(sizeof(checkpoint_v2_header_t) == 64u, "checkpoint_v2_header_t size must be 64");
_Static_assert(sizeof(checkpoint_v2_region_t) == 16u, "checkpoint_v2_region_t size must be 16");
