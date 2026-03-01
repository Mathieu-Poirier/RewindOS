#pragma once

#include "stdint.h"

#define RWFS_NAME_MAX 32u
#define RWFS_MAX_LIST 32u

typedef struct {
    char name[RWFS_NAME_MAX];
    uint32_t size_bytes;
    uint32_t generation;
} rwfs_file_info_t;

typedef struct {
    uint8_t mounted;
    uint8_t formatted;
    uint16_t _pad;
    int32_t last_err;
    uint32_t region_start_lba;
    uint32_t region_blocks;
    uint32_t next_data_lba;
    uint32_t file_count;
    uint32_t super_generation;
} rwfs_status_t;

int rwfs_init_or_mount(void);
int rwfs_write_all_atomic(const char *name, const void *data, uint32_t len);
int rwfs_read_all(const char *name, void *buf, uint32_t cap, uint32_t *out_len);
int rwfs_list(rwfs_file_info_t *out, uint32_t cap, uint32_t *out_count);
void rwfs_get_status(rwfs_status_t *out);
