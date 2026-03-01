# Userspace FS + `lineed` Architecture (v0)

## Goals

- Make snapshot slots real boot targets in boot menu and restore flow.
- Add a minimal userspace-writable filesystem (`RWFS0`) for text files.
- Add a line editor program (`lineed`) with deterministic command grammar.
- Preserve in-progress editing state across reboot via snapshot + journal replay.
- Keep implementation small, deterministic, and corruption-tolerant.

## Scope (v0)

- Flat namespace (no directories): `name.txt` style file ids only.
- Text-only file content (UTF-8 bytes + `\n` line separators).
- One editor process at a time (`lineed <file>`).
- Atomic save semantics.
- Recovery-first behavior when metadata/data is corrupt.

Out of scope:

- Multi-user permissions.
- Nested folders.
- Binary file support.
- CoW block trees/dedup.

## Bootmenu Integration

Boot menu must treat snapshots as first-class boot targets:

- `Boot recent`: boot latest valid runtime target from ADMIN metadata.
- `Boot fresh`: boot app ignoring snapshot restore.
- `Snapshot 0..N`: explicit snapshot boot targets (newest to older).

Selection behavior:

1. On selection, persist boot target (`mode`, `slot`, `seq`) in ADMIN.
2. Reboot/jump.
3. Early restore path reads target and attempts restore.
4. If selected snapshot fails validation/restore, walk to next older valid snapshot.
5. If no snapshot works, apply `fallback_mode` (`recovery` preferred default).

## RWFS0 Layout (Minimal, Fixed-Shape)

RWFS0 is independent from snapshot ring and journal ring.

```text
[FS_SUPER][FS_INDEX blocks][FS_DATA blocks...]
```

### `FS_SUPER`

- magic/version
- generation
- total blocks in RWFS0
- index region bounds
- data region bounds
- active index generation
- CRC

### `FS_INDEX` (fixed-size table)

Each entry:

- file name (fixed cap, e.g. 32 bytes)
- state (`free`, `live`, `tombstone`)
- data start LBA
- data byte length
- file generation
- content CRC
- metadata CRC

Rules:

- Flat file list only.
- Duplicate names not allowed.
- Highest generation wins if duplicate appears due to crash window.

### `FS_DATA`

File payload uses explicit boundaries:

```text
[FDAT][file_hdr][raw bytes...][FDEN][tail]
```

Where:

- `file_hdr`: name hash, generation, payload len, payload CRC
- `tail`: generation, payload CRC, tail CRC

Validity:

- Require `FDAT` and `FDEN`.
- Require len + CRC match.
- Require index entry generation to match selected data record.

## `lineed` Program UX + Grammar

Launch:

- `lineed <file>`
- If file is missing: create in-memory empty buffer (saved on `save`).
- Foreground program acquires terminal stdin lock until `exit`.

Editor prompt:

- `lineed:<file> > `

Commands (v0 required):

- `<n> <text>`
  - Replace line `n` (1-based) with `<text>`.
  - If `n == line_count + 1`, append.
  - If `n > line_count + 1`, reject (no sparse holes in v0).
- `print <n>`
  - Print line `n`.
- `print <a>-<b>`
  - Print inclusive line range.
- `save`
  - Atomically persist current buffer to RWFS0.
- `exit`
  - Release stdin lock and return to shell.

Optional (nice-to-have later):

- `delete <n>`
- `help`
- `q!` (exit without save)

## In-Memory Model (`lineed`)

- `file_name`
- `dirty` flag
- line table (offset + len per line)
- contiguous text buffer
- cursor line/column (for future interactive mode)
- command input buffer state

Storage format in memory should be deterministic for snapshot restore.

## Save Protocol (Atomic)

`save` is append-then-switch, never in-place overwrite:

1. Build new data record in free RWFS0 space.
2. Write full `FDAT ... FDEN` record.
3. Verify CRC/readback (optional in v0, recommended).
4. Write updated index entry with generation+1.
5. Advance superblock generation (A/B mirrored).

Crash handling:

- Partial data write without index switch: ignored.
- Partial index update: older generation remains valid.
- Orphan records are reclaimable by GC pass.

## Snapshot + Journal Contract for Editor Continuity

To restore "mid-typed, mid-edit" behavior:

- Snapshot captures baseline SRAM state:
  - scheduler queues/cursors
  - terminal line-edit state
  - `lineed` program state (open file, buffer, dirty, current command text)
- Journal replays interactive deltas after snapshot:
  - keystrokes
  - line mutations
  - stdin ownership transitions
  - command submit/cancel events

Restore ordering:

1. Restore snapshot memory image.
2. Sanity-check scheduler/terminal/editor structures.
3. Replay journal records newer than snapshot seq.
4. Re-check invariants.
5. Resume scheduler.

## Required Kernel APIs

Filesystem API (kernel-owned):

- `fs_open_or_create(name)`
- `fs_read_all(file_id, buf, cap)`
- `fs_write_all_atomic(file_id, data, len)`
- `fs_list(...)`

Editor control API:

- `lineed_start(name, bg=0)`
- `lineed_stdin_raw(byte)`
- `lineed_save()`
- `lineed_exit()`

Journal API:

- `jrnl_append(event_type, payload...)`
- `jrnl_replay_from(snap_seq)`

## Failure Policy

- If RWFS0 is invalid but snapshots are valid:
  - boot with restore, mount FS read-only/degraded, surface warning.
- If snapshots invalid but RWFS0 valid:
  - boot fresh or recovery (per policy), keep user files.
- If journal invalid:
  - restore snapshot baseline only, skip bad records, continue boot.

## Milestones

1. `snapls` in main shell + boot restore uses explicit snapshot target.
2. RWFS0 superblock/index/data parsing + `fs_write_all_atomic`.
3. `lineed <file>` with `n text`, `print`, `save`, `exit`.
4. Journal event set for editor continuity.
5. End-to-end reboot test: partially typed command in `lineed` resumes correctly.
