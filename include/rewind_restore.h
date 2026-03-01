#pragma once

#include "stdint.h"

#define REWIND_HANDOFF_MAGIC 0x54535752u /* "RWST" */
#define REWIND_HANDOFF_VERSION 1u
#define REWIND_HANDOFF_FLAG_RESUME (1u << 0)

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t flags;
    uint32_t slot;
    uint32_t seq;
    uint32_t journal_start_lba;
    uint32_t journal_blocks;
    uint32_t reserved[9];
} rewind_handoff_t;

#define REWIND_HANDOFF_ADDR 0x2004FFC0u

void rewind_handoff_clear(void);
void rewind_handoff_mark_resume(uint32_t slot, uint32_t seq, uint32_t journal_start_lba, uint32_t journal_blocks);
int  rewind_handoff_resume_requested(void);
void rewind_handoff_read(rewind_handoff_t *out);
uint32_t rewind_restore_stack_top(void);

/* Journal replay is wired here so boot/main can share one entry point. */
void rewind_journal_replay(uint32_t snapshot_seq, uint32_t journal_start_lba, uint32_t journal_blocks);
void rewind_resume_entry(void);
