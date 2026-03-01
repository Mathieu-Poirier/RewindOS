#pragma once

#include "stdint.h"

/* Configure journal geometry from ADMIN metadata. Passing blocks=0 disables it. */
void journal_configure(uint32_t start_lba, uint32_t blocks, uint32_t last_snapshot_seq);

/* Called by snapshot writer after a new snapshot commit succeeds. */
void journal_note_snapshot_seq(uint32_t snapshot_seq);

/* Kernel-default capture points. */
int journal_capture_input_byte(uint8_t c);
int journal_capture_io_owner(uint8_t owner_ao, uint8_t acquired);

/* Replay journal records newer than or equal to snapshot_seq baseline. */
void journal_replay_from_snapshot(uint32_t snapshot_seq, uint32_t journal_start_lba, uint32_t journal_blocks);
