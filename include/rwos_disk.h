#pragma once

#include "stdint.h"
#include "sd.h"

/*
 * RewindOS unified disk partition table.
 *
 * Stored as a mirrored pair at LBA 1 and LBA 2.  LBA 0 is reserved (MBR area).
 * On first boot with a blank card, rwos_disk_init() auto-formats from card
 * capacity and SRAM size, then writes both copies.
 *
 * Disk layout after format:
 *   LBA 0          : reserved (MBR / sector 0)
 *   LBA 1          : disk table copy A  (rwos_disk_t)
 *   LBA 2          : disk table copy B  (rwos_disk_t)
 *   LBA 3..130     : journal region     (128 blocks = 64 KB)
 *   LBA 131..N     : snapshot region    (snap_slots * snap_slot_blocks)
 *   LBA N+1..end   : rwfs region        (rest of card)
 */

#define RWOS_DISK_MAGIC    0x49445752u  /* "RWDI" */
#define RWOS_DISK_VERSION  1u
#define RWOS_DISK_LBA_A    1u
#define RWOS_DISK_LBA_B    2u

/* boot_target_mode values */
#define RWOS_BOOT_TARGET_RECENT   0u   /* restore the most recent valid snapshot */
#define RWOS_BOOT_TARGET_FRESH    1u   /* cold boot, do not restore */
#define RWOS_BOOT_TARGET_SNAPSHOT 2u   /* restore a specific slot/seq */

/*
 * Exactly SD_BLOCK_SIZE (512) bytes.
 * checksum covers the first 18 uint32_t words (all fields before it).
 */
typedef struct {
    uint32_t magic;              /*  0 RWOS_DISK_MAGIC                */
    uint32_t version;            /*  1 RWOS_DISK_VERSION              */
    uint32_t generation;         /*  2 incremented on each write      */
    uint32_t total_blocks;       /*  3 SD card capacity (512-B blocks)*/

    /* Journal partition */
    uint32_t journal_start_lba;  /*  4                                */
    uint32_t journal_blocks;     /*  5                                */

    /* Snapshot partition */
    uint32_t snap_start_lba;     /*  6                                */
    uint32_t snap_blocks;        /*  7 == snap_slots * snap_slot_blocks */
    uint32_t snap_slots;         /*  8                                */
    uint32_t snap_slot_blocks;   /*  9 blocks allocated per slot      */
    uint32_t snap_interval_s;    /* 10 periodic snapshot interval     */

    /* RWFS partition */
    uint32_t rwfs_start_lba;     /* 11                                */
    uint32_t rwfs_blocks;        /* 12                                */

    /* Boot target — written by snapshot task after each commit */
    uint32_t boot_target_mode;   /* 13 RWOS_BOOT_TARGET_*             */
    uint32_t boot_target_slot;   /* 14 slot index (TARGET_SNAPSHOT)   */
    uint32_t boot_target_seq;    /* 15 seq number  (TARGET_SNAPSHOT)  */

    /* Last successful snapshot */
    uint32_t last_snap_slot;     /* 16                                */
    uint32_t last_snap_seq;      /* 17                                */

    uint32_t checksum;           /* 18 FNV-1a over words 0-17         */
    uint32_t reserved[109];      /* 19..127 pad to 512 bytes          */
} rwos_disk_t;

/*
 * Initialize disk: read from card, or auto-format if not found.
 * Must be called once after sd_async_init() and before any subsystem
 * that depends on partition layout (rwfs, snapshot).
 * Returns SD_OK on success, negative on error.
 */
int rwos_disk_init(void);

/*
 * Return a pointer to the in-memory disk table (valid after rwos_disk_init).
 * Returns NULL if not yet initialized.
 */
const rwos_disk_t *rwos_disk_get(void);

/*
 * Update boot target fields in-memory and persist to both LBAs.
 * The snapshot task calls this after selecting a restore candidate.
 */
int rwos_disk_set_boot_target(uint32_t mode, uint32_t slot, uint32_t seq);

/*
 * Update last-snapshot tracking in-memory and persist to both LBAs.
 * The snapshot task calls this after a successful snapshot commit.
 */
int rwos_disk_set_last_snap(uint32_t slot, uint32_t seq);
