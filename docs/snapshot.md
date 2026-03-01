# Snapshot + SD Architecture (v0)

## Goals

- Persist system state frequently (default every 60s) with retention of `k` snapshots.
- Make snapshots bootloader-readable and crash-safe.
- Skip corrupt or partial snapshots automatically and reuse their space.
- Keep storage layout deterministic and format-enforced.
- Avoid hardcoded SD capacities; derive all region sizes from detected card geometry.

## Scope (v0)

- Snapshot capture target: **internal SRAM state only**.
- Snapshot persistence medium: SD card block device.
- User filesystem: minimal, text-only friendly format in a dedicated region.
- Bootloader responsibility: parse metadata, select valid snapshot, restore-or-fallback.

Out of scope for v0:

- SDRAM snapshotting.
- Multi-file rich filesystem semantics.
- Delta compression/deduplication.

## RAM Capture Region (SRAM-first)

v0 snapshot captures kernel/app state from internal SRAM with explicit exclusions.

Included regions (by linker symbols):

- `.data`: `[_sdata, _edata)`
- `.bss`: `[_sbss, _ebss)`
- `.noinit`: `[_snoinit, _enoinit)` (optional policy flag)
- Heap live range: `[_end, bump_brk)`

Excluded regions:

- `.snap_exclude`: `[_ssnap_exclude, _esnap_exclude)`
- Active runtime stack window near `_estack`
- DMA/driver transient buffers that cannot be restored safely

Implementation rule:

- Build a **snapshot segment table** at runtime (base+len entries), not one blind full-SRAM copy.
- Segment table is stored in slot metadata and used by bootloader restore logic.

### RAM Region Boundary Markers (required)

Each serialized RAM region must be wrapped with explicit markers and checksums:

- region start marker: `RGS0`
- region end marker: `RGE0`

Per-region serialized shape:

```text
[RGS0][region_hdr][region_bytes...][RGE0][region_tail]
```

`region_hdr` fields:

- `region_id`
- `vaddr_start`
- `vaddr_end`
- `data_len`
- `data_crc32`

`region_tail` fields:

- `region_id`
- `data_crc32`
- `tail_crc32`

Parser rule:

- region is valid only if `RGS0` and `RGE0` are both present, ids match, and CRCs validate.

## SD Card Layout (dynamic)

Layout is computed from `total_blocks` reported by SD init/info.

```text
LBA 0..Nadmin-1        : ADMIN region (A/B superblocks + partition table + policy)
LBA Nadmin..Nsnap_end  : Snapshot ring (fixed-size slots)
LBA Nsnap_end+1..end   : RWFS region (current user filesystem)
```

Boot policy requirement:

- Bootloader defaults to booting the **last valid bootable partition** (newest valid generation/sequence).
- If that partition is invalid at boot-time verification, bootloader walks backward to older valid partitions.
- If no valid bootable partition exists, bootloader enters recovery/default shell path.

### Sizing rules

Given:

- `B = total_blocks`
- `bs = logical_block_size` (typically 512)
- `snap_payload_bytes = sum(segment lengths)`
- `slot_blocks = ceil((slot_header + payload + slot_commit) / bs)`

Compute:

- `admin_blocks = max(32, align_up(B / 4096, 32))`
- `snap_blocks_target = clamp(B * SNAP_PART_PCT, min_snap_blocks, B - admin_blocks - min_fs_blocks)`
- `slot_count = floor(snap_blocks_target / slot_blocks)`
- `retained_k = min(config_k, slot_count)`

Notes:

- `SNAP_PART_PCT` is a config knob (e.g. 25%), not a hardcoded absolute size.
- If `slot_count < 2`, snapshotting is disabled and system boots normally with warning.

## On-Disk Metadata

All control metadata lives in the ADMIN region.

## ADMIN Superblock (A/B mirrored)

Stored in two copies (A/B) with generation counter:

- magic (`RWSS`)
- version
- block size
- total blocks
- ADMIN/snapshot/fs region boundaries
- slot geometry (`slot_blocks`, `slot_count`)
- configured `k`
- active generation
- CRC32

Bootloader picks the newest valid superblock copy.

## ADMIN Partition Table

Partition table records live in ADMIN and define bootable entries:

- `part_id`
- partition type (`SNAP`, `RWFS`, reserved)
- `start_lba`, `end_lba` (explicit inclusive bounds)
- `block_count` (derived/sanity-checked from bounds)
- boot flags (`bootable`, `readonly`, `disabled`)
- `last_valid_seq` hint
- generation + CRC32

Bootloader default target is the last valid bootable partition entry.

Constraint:

- `readonly` is valid for non-ADMIN partitions only; ADMIN metadata is always writable by design.

## ADMIN Update Protocol (atomic metadata writes)

ADMIN metadata updates use A/B copy-on-write semantics:

1. Read active valid copy (`A` or `B`).
2. Build new metadata image in RAM with `generation + 1`.
3. Write inactive copy fully.
4. Read back and CRC-verify inactive copy.
5. Mark it active by generation rule (no in-place mutate of active copy).

Power loss during update keeps at least one valid ADMIN copy.

## ADMIN Writability Rule

- ADMIN region is always writable in normal operation.
- ADMIN must never be mounted/configured as `readonly`.
- If RWFS is forced read-only (degraded mode), ADMIN remains writable so boot/snapshot metadata can still advance.
- If ADMIN cannot be written, snapshot creation is disabled and system should raise a persistent health/error flag.

## Snapshot Slot Format

Each slot is fixed-size to avoid fragmentation.

```text
[slot header block][payload blocks...][slot commit block]
```

Payload bytes are also framed:

```text
[PAY0][segment_table][RGS0...RGE0 region blobs][PAYE]
```

Required payload markers:

- payload start marker: `PAY0`
- payload end marker: `PAYE`

Slot header contains:

- magic (`SLOT`)
- format version
- `seq` (monotonic snapshot number)
- timestamp/tick
- segment table hash + payload size
- expected payload CRC32
- expected region count
- header CRC32

Commit block contains (written last):

- magic (`CMIT`)
- `seq`
- payload CRC32
- commit CRC32

Validity rule:

- Slot is valid only if header + commit are both valid, `seq` matches, payload CRC verifies, and payload start/end + region markers validate.

This makes interrupted writes auto-skippable.

## Corruption + Reuse Policy

- Any slot failing validity is treated as **free/rewritable**.
- Writer uses ring order (`next_slot = (last_slot + 1) % slot_count`).
- No dynamic allocation inside snapshot region, so no fragmentation.
- Corrupt or mid-write slots are naturally overwritten on future cycles.

## Bootloader Parse + Restore Flow

1. Read ADMIN superblock A/B, choose newest valid.
2. Validate ADMIN + partition bounds against card geometry.
3. Read ADMIN partition table and select last valid bootable partition.
4. Scan snapshot slots for that partition and collect candidate valid `seq`s.
5. Try newest candidate first:
- verify `PAY0`/`PAYE`
- stream payload and verify CRC32
- verify all `RGS0`/`RGE0` pairs and region CRCs
- restore segment table into SRAM
6. If candidate fails, try next newest (and then next older valid partition if needed).
7. If all fail, boot recovery/default path.

Corrupt/in-progress snapshots are skipped without blocking boot.

## RW Filesystem Partition (naive v0)

Use a minimal filesystem in its own region (`RWFS0`) with text-focused data.

v0 constraints:

- single file type: UTF-8 text blob/log entries
- fixed metadata header + append-friendly data area
- optional single “active file” for now
- explicit file data boundaries with start/end markers

Suggested minimal objects:

- `fs_super` (magic/version/size/crc)
- `fs_manifest` (file id, offsets, lengths, crc)
- `fs_data` (append records: `[FDAT][file_hdr][bytes][FDEN][record_crc]`)

This keeps implementation small while separating filesystem from snapshot ring.

### File Data Boundary Markers (required)

For each appended file record:

- file data start marker: `FDAT`
- file data end marker: `FDEN`

`file_hdr` fields:

- `file_id`
- `record_seq`
- `payload_len`
- `payload_crc32`

Parser rule:

- record is valid only if both markers exist, `payload_len` matches, and CRCs validate.

## Format Enforcement

Add a `format` command to initialize/repair layout:

- Writes ADMIN superblock A/B
- Writes ADMIN partition table
- Initializes snapshot slot geometry
- Clears slot headers/commit markers
- Initializes empty `RWFS0`
- Writes canonical marker constants/version table used by boot parser
- Verifies geometry + CRCs

Recommended commands:

- `format` (quick metadata init)
- `format --full` (full wipe/zero for diagnostics)
- `snapls` (list snapshot slots + validity)
- `snapnow` (manual snapshot)
- `snapcfg [k] [interval_s]`

## Snapshot Task Architecture (efficient + safe)

Run as dedicated AO (`AO_SNAPSHOT`) on interval timer and manual trigger.

### Quiesce protocol (atomicity)

Before capture:

1. Request quiesce barrier from scheduler.
2. Pause event acceptance for mutating AOs (except SD + snapshot path).
3. Acquire terminal/IO foreground lock boundary (no foreground mutation during capture).
4. Wait for in-flight mutating operations to ACK idle.

Capture/write:

5. Build segment table.
6. Compute CRC while streaming segments in chunks to SD AO.
7. Write commit block last.

After commit:

8. Release quiesce barrier and IO lock.
9. Resume paused AOs.

### Efficiency rules

- Write in bounded chunks (e.g. 4-16 blocks/event) to keep RTC latency bounded.
- Avoid heap allocations in fast path.
- Reuse static SD buffers.
- Keep quiesce window as short as possible; do non-critical prep outside barrier.

## Cadence + Retention

Defaults:

- interval: 60 seconds
- retention target: `k=8` (bounded by `slot_count`)

Policy:

- Keep newest `retained_k` by ring overwrite semantics.
- If SD busy/error, skip cycle and retry next interval.

## Invariants

- Never trust on-card metadata without CRC/version checks.
- Never restore payload without validating payload/region/file boundary markers.
- Never update commit marker before payload is fully written.
- Never do in-place partial updates of active ADMIN metadata copy.
- Never put ADMIN in read-only mode.
- Never block boot on snapshot restore failure.
- Never hardcode absolute SD sizes; always derive from detected geometry.

## Open decisions (to finalize after review)

- Whether `.noinit` should be included by default.
- Exact AO set that must ACK quiesce.
- Whether bootloader should restore only SRAM segments or also selected device state.
- Whether `RWFS0` starts with one active text file or a tiny fixed directory of text files.
