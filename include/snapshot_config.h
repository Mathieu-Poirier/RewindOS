#pragma once

/*
 * Single source of truth for snapshot / journal / disk-layout defaults.
 *
 * Included by:
 *   - src/common_io/rwos_disk.c   (auto-format)
 *   - src/scheduler/snapshot_task.c (runtime clamp / register)
 *   - src/bootmenu/boot_terminal.c (setup menu / format)
 */

/* ---- Snapshot slots ---- */
#define SNAPSHOT_SLOTS_DEFAULT  4u
#define SNAPSHOT_SLOTS_MIN      1u
#define SNAPSHOT_SLOTS_MAX      64u

/* ---- Snapshot interval ---- */
#define SNAPSHOT_INTERVAL_DEFAULT_S  60u
#define SNAPSHOT_INTERVAL_MIN_S       5u
#define SNAPSHOT_INTERVAL_MAX_S    3600u

/* ---- Journal ring ---- */
#define JOURNAL_BLOCKS_DEFAULT  128u   /* 64 KB */
#define JOURNAL_BLOCKS_MAX     4096u

/* ---- Per-slot overhead (header + commit blocks) ---- */
#define SNAPSHOT_HDR_BLOCKS     16u
