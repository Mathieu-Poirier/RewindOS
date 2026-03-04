#pragma once

#include "stdint.h"

#define RESTORE_ENV_MAGIC 0x31564E45u /* "ENV1" */
#define RESTORE_ENV_VERSION 1u
#define RESTORE_ENV_MAX_ENTRIES 8u
#define RESTORE_ENV_ENTRY_DATA_MAX 32u

enum {
    RESTORE_ENV_ENTRY_PENDING = 0,
    RESTORE_ENV_ENTRY_DONE = 1,
    RESTORE_ENV_ENTRY_FAILED = 2
};

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t entry_count;
    uint16_t read_idx;
    uint16_t write_idx;
    uint32_t generation;
    uint32_t next_seq;
    uint8_t program_id;
    uint8_t reserved[3];
} __attribute__((packed)) restorable_envelope_header_t;

typedef struct {
    uint32_t seq;
    uint16_t action;
    uint16_t entry_state;
    uint16_t data_len;
    uint16_t reserved;
    uint8_t data[RESTORE_ENV_ENTRY_DATA_MAX];
} __attribute__((packed)) restorable_envelope_entry_t;

typedef struct {
    restorable_envelope_header_t hdr;
    restorable_envelope_entry_t entries[RESTORE_ENV_MAX_ENTRIES];
} __attribute__((packed)) restorable_envelope_t;

_Static_assert(sizeof(restorable_envelope_header_t) == 24u, "restorable_envelope_header_t size");
_Static_assert(sizeof(restorable_envelope_entry_t) == 44u, "restorable_envelope_entry_t size");
