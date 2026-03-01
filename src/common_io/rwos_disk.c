#include "../../include/rwos_disk.h"
#include "../../include/sd.h"
#include "../../include/sd_async.h"
#include "../../include/uart.h"
#include "../../include/panic.h"
#include "../../include/log.h"
#include "../../include/snapshot_config.h"

/* Layout constants used during auto-format. */
#define DISK_ADMIN_END_LBA    3u    /* LBAs 0-2 reserved (0=MBR, 1-2=table) */
#define DISK_MIN_RWFS_BLOCKS  64u

extern uint8_t _sram_start;
extern uint8_t _sram_end;

static rwos_disk_t g_rwos_disk;
static uint8_t g_rwos_disk_valid;
static uint32_t g_rwos_blk[SD_BLOCK_SIZE / 4u];

/* ------------------------------------------------------------------ */
/* Checksum over the first 18 words (all fields before checksum itself) */
/* ------------------------------------------------------------------ */
static uint32_t rwos_disk_checksum(const rwos_disk_t *d)
{
    uint32_t h = 2166136261u;
    const uint32_t *w = (const uint32_t *)d;
    for (uint32_t i = 0u; i < 18u; i++) {
        h ^= w[i];
        h *= 16777619u;
    }
    return h ^ 0xA5A55A5Au;
}

/* ------------------------------------------------------------------ */
/* Media helpers                                                        */
/* ------------------------------------------------------------------ */
static int rwos_disk_media_ready(void)
{
    sd_detect_init();
    if (!sd_is_detected()) {
        return SD_ERR_NO_INIT;
    }
    sd_use_pll48(1);
    sd_set_data_clkdiv(SD_CLKDIV_FAST);
    if (!sd_get_info()->initialized) {
        return sd_init();
    }
    return SD_OK;
}

/* ------------------------------------------------------------------ */
/* Read / write helpers                                                 */
/* ------------------------------------------------------------------ */
/* Boot menu writes ADMIN_MAGIC ("RWSS") to LBA 1-2.  Recognise it so the
 * main firmware doesn't think "no table found" and try to auto-format. */
#define ADMIN_MAGIC_ALT   0x53535752u  /* "RWSS" */
#define ADMIN_VERSION_ALT 1u

static int rwos_disk_parse(const uint8_t *blk, rwos_disk_t *out)
{
    const rwos_disk_t *d = (const rwos_disk_t *)blk;

    /* --- native RWDI format --- */
    if (d->magic == RWOS_DISK_MAGIC && d->version == RWOS_DISK_VERSION) {
        if (d->checksum != rwos_disk_checksum(d)) {
            return 0;
        }
        if (out != 0) {
            const uint32_t *src = (const uint32_t *)d;
            uint32_t *dst = (uint32_t *)out;
            for (uint32_t i = 0u; i < (sizeof(rwos_disk_t) / sizeof(uint32_t)); i++) {
                dst[i] = src[i];
            }
        }
        return 1;
    }

    /* --- boot-menu RWSS format --- synthesise a rwos_disk_t from it */
    const uint32_t *aw = (const uint32_t *)blk;
    if (aw[0] != ADMIN_MAGIC_ALT || aw[1] != ADMIN_VERSION_ALT) {
        return 0;
    }
    if (out != 0) {
        uint32_t *w = (uint32_t *)out;
        for (uint32_t i = 0u; i < (sizeof(rwos_disk_t) / sizeof(uint32_t)); i++)
            w[i] = 0u;

        out->magic             = RWOS_DISK_MAGIC;
        out->version           = RWOS_DISK_VERSION;
        out->generation        = aw[2];   /* generation      */
        out->total_blocks      = aw[3];   /* total_blocks    */
        out->snap_slots        = aw[5];   /* snapshot_slots  */
        out->snap_interval_s   = aw[6];   /* interval_s      */
        out->snap_start_lba    = aw[7];   /* reserved[0]     */
        out->snap_blocks       = aw[8];   /* reserved[1]     */
        out->snap_slot_blocks  = aw[9];   /* reserved[2]     */
        out->boot_target_mode  = aw[10];  /* reserved[3]     */
        out->boot_target_slot  = aw[11];  /* reserved[4]     */
        out->boot_target_seq   = aw[12];  /* reserved[5]     */
        out->last_snap_slot    = aw[13];  /* reserved[6]     */
        out->last_snap_seq     = aw[14];  /* reserved[7]     */
        out->journal_start_lba = aw[15];  /* reserved[8]     */
        out->journal_blocks    = aw[16];  /* reserved[9]     */

        uint32_t rwfs_start = out->snap_start_lba + out->snap_blocks;
        out->rwfs_start_lba = rwfs_start;
        out->rwfs_blocks    = (out->total_blocks > rwfs_start)
                            ? out->total_blocks - rwfs_start : 0u;
        out->checksum = rwos_disk_checksum(out);
    }
    return 1;
}

static int rwos_disk_read_best(void)
{
    rwos_disk_t a;
    rwos_disk_t b;
    int have_a = 0;
    int have_b = 0;

    if (sd_read_blocks(RWOS_DISK_LBA_A, 1u, g_rwos_blk) == SD_OK) {
        have_a = rwos_disk_parse((const uint8_t *)g_rwos_blk, &a);
    }
    if (sd_read_blocks(RWOS_DISK_LBA_B, 1u, g_rwos_blk) == SD_OK) {
        have_b = rwos_disk_parse((const uint8_t *)g_rwos_blk, &b);
    }

    if (!have_a && !have_b) {
        return 0;
    }

    const rwos_disk_t *best;
    if (have_a && (!have_b || a.generation >= b.generation)) {
        best = &a;
    } else {
        best = &b;
    }

    const uint32_t *src = (const uint32_t *)best;
    uint32_t *dst = (uint32_t *)&g_rwos_disk;
    for (uint32_t i = 0u; i < (sizeof(rwos_disk_t) / sizeof(uint32_t)); i++) {
        dst[i] = src[i];
    }
    return 1;
}

static int rwos_disk_write_both(void)
{
    /* Copy struct into a full block (reserved[] already pads to 512). */
    int rc = sd_write_blocks(RWOS_DISK_LBA_A, 1u, &g_rwos_disk);
    if (rc != SD_OK) {
        LOG_ERR("disk-A", rc);
        return rc;
    }
    rc = sd_write_blocks(RWOS_DISK_LBA_B, 1u, &g_rwos_disk);
    if (rc != SD_OK) {
        LOG_ERR("disk-B", rc);
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* Auto-format                                                          */
/* ------------------------------------------------------------------ */
static uint32_t disk_sram_blocks(void)
{
    uintptr_t bytes = (uintptr_t)&_sram_end - (uintptr_t)&_sram_start;
    if (bytes == 0u) {
        return 640u; /* fallback: 320 KB */
    }
    return (uint32_t)((bytes + (SD_BLOCK_SIZE - 1u)) / SD_BLOCK_SIZE);
}

static int rwos_disk_auto_format(void)
{
    const sd_info_t *info = sd_get_info();
    if (!info->initialized || info->capacity_blocks == 0u) {
        return SD_ERR_NO_INIT;
    }

    uint32_t card_blocks     = info->capacity_blocks;
    uint32_t sram_blocks     = disk_sram_blocks();
    uint32_t snap_slot_blocks = sram_blocks + SNAPSHOT_HDR_BLOCKS;
    uint32_t snap_blocks     = SNAPSHOT_SLOTS_DEFAULT * snap_slot_blocks;

    uint32_t journal_start = DISK_ADMIN_END_LBA;
    uint32_t snap_start    = journal_start + JOURNAL_BLOCKS_DEFAULT;
    uint32_t rwfs_start    = snap_start + snap_blocks;

    if (rwfs_start + DISK_MIN_RWFS_BLOCKS > card_blocks) {
        uart_puts("rwos_disk: card too small for format\r\n");
        return SD_ERR_PARAM;
    }

    uint32_t *w = (uint32_t *)&g_rwos_disk;
    for (uint32_t i = 0u; i < (sizeof(g_rwos_disk) / sizeof(uint32_t)); i++) {
        w[i] = 0u;
    }

    g_rwos_disk.magic            = RWOS_DISK_MAGIC;
    g_rwos_disk.version          = RWOS_DISK_VERSION;
    g_rwos_disk.generation       = 1u;
    g_rwos_disk.total_blocks     = card_blocks;
    g_rwos_disk.journal_start_lba = journal_start;
    g_rwos_disk.journal_blocks   = JOURNAL_BLOCKS_DEFAULT;
    g_rwos_disk.snap_start_lba   = snap_start;
    g_rwos_disk.snap_blocks      = snap_blocks;
    g_rwos_disk.snap_slots       = SNAPSHOT_SLOTS_DEFAULT;
    g_rwos_disk.snap_slot_blocks = snap_slot_blocks;
    g_rwos_disk.snap_interval_s  = SNAPSHOT_INTERVAL_DEFAULT_S;
    g_rwos_disk.rwfs_start_lba   = rwfs_start;
    g_rwos_disk.rwfs_blocks      = card_blocks - rwfs_start;
    g_rwos_disk.boot_target_mode = RWOS_BOOT_TARGET_RECENT;
    g_rwos_disk.checksum         = rwos_disk_checksum(&g_rwos_disk);

    int rc = rwos_disk_write_both();
    if (rc != SD_OK) {
        uart_puts("rwos_disk: write failed err=-");
        uart_put_u32((uint32_t)(0 - rc));
        uart_puts(" cmd=");
        uart_put_hex32(sd_last_cmd());
        uart_puts(" sta=");
        uart_put_hex32(sd_last_sta());
        uart_puts("\r\n");
        return rc;
    }
    uart_puts("rwos_disk: auto-format ok\r\n");
    return SD_OK;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

int rwos_disk_init(void)
{
    PANIC_IF(sizeof(rwos_disk_t) != SD_BLOCK_SIZE, "rwos_disk_t size != 512");

    g_rwos_disk_valid = 0u;

    int rc = rwos_disk_media_ready();
    if (rc != SD_OK) {
        uart_puts("rwos_disk: media not ready\r\n");
        return rc;
    }

    if (rwos_disk_read_best()) {
        uart_puts("rwos_disk: table loaded\r\n");
        g_rwos_disk_valid = 1u;
        return SD_OK;
    }

    uart_puts("rwos_disk: no table found, formatting\r\n");
    rc = rwos_disk_auto_format();
    if (rc == SD_OK) {
        g_rwos_disk_valid = 1u;
    }
    return rc;
}

const rwos_disk_t *rwos_disk_get(void)
{
    if (!g_rwos_disk_valid) {
        return 0;
    }
    return &g_rwos_disk;
}

int rwos_disk_set_boot_target(uint32_t mode, uint32_t slot, uint32_t seq)
{
    if (!g_rwos_disk_valid) {
        return SD_ERR_NO_INIT;
    }
    if (g_rwos_disk.generation != 0xFFFFFFFFu) {
        g_rwos_disk.generation++;
    }
    g_rwos_disk.boot_target_mode = mode;
    g_rwos_disk.boot_target_slot = slot;
    g_rwos_disk.boot_target_seq  = seq;
    g_rwos_disk.checksum = rwos_disk_checksum(&g_rwos_disk);
    return rwos_disk_write_both();
}

int rwos_disk_set_last_snap(uint32_t slot, uint32_t seq)
{
    if (!g_rwos_disk_valid) {
        return SD_ERR_NO_INIT;
    }
    if (g_rwos_disk.generation != 0xFFFFFFFFu) {
        g_rwos_disk.generation++;
    }
    g_rwos_disk.last_snap_slot = slot;
    g_rwos_disk.last_snap_seq  = seq;
    g_rwos_disk.checksum = rwos_disk_checksum(&g_rwos_disk);
    return rwos_disk_write_both();
}
