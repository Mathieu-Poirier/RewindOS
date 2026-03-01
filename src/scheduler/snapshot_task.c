#include "../../include/snapshot_task.h"
#include "../../include/uart.h"
#include "../../include/sd.h"
#include "../../include/sd_async.h"
#include "../../include/nvic.h"
#include "../../include/task_ids.h"
#include "../../include/task_signals.h"
#include "../../include/task_spec.h"
#include "../../include/cmd_context.h"
#include "../../include/console.h"
#include "../../include/log.h"
#include "../../include/systick.h"
#include "../../include/journal.h"
#include "../../include/rwos_disk.h"
#include "../../include/panic.h"
#include "../../include/log.h"
#include "../../include/snapshot_config.h"

#define SNAPSHOT_Q_CAP 8u

#define SNAP_HDR_MAGIC 0x50414E53u   /* "SNAP" */
#define SNAP_CMIT_MAGIC 0x54494D43u  /* "CMIT" */
#define SNAP_FMT_VERSION 1u
#define SNAP_HDR_WORDS 7u
#define SNAP_CMIT_WORDS 4u

#define SNAPSHOT_MIN_SLOT_BLOCKS 2u
#define SNAPSHOT_REASON_PERIODIC 0u
#define SNAPSHOT_REASON_MANUAL 1u
/* SNAP_TARGET_* aliases map to RWOS_BOOT_TARGET_* */
#define SNAP_TARGET_RECENT   RWOS_BOOT_TARGET_RECENT
#define SNAP_TARGET_FRESH    RWOS_BOOT_TARGET_FRESH
#define SNAP_TARGET_SNAPSHOT RWOS_BOOT_TARGET_SNAPSHOT

#define NVIC_ISER_BASE 0xE000E100u
#define SNAP_PAY0_MAGIC 0x30594150u /* "PAY0" */
#define SNAP_PAYE_MAGIC 0x45594150u /* "PAYE" */
#define SNAP_RGS0_MAGIC 0x30534752u /* "RGS0" */
#define SNAP_RGE0_MAGIC 0x30454752u /* "RGE0" */
#define SNAP_PAY0_WORDS 7u
#define SNAP_PAYE_WORDS 4u
#define SNAP_RGS0_WORDS 4u
#define SNAP_RGE0_WORDS 5u
#define SNAPSHOT_STACK_EXCLUDE_BYTES 8192u

extern uint8_t _sram_start;
extern uint8_t _sram_end;
extern uint8_t _ssnap_exclude;
extern uint8_t _esnap_exclude;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t seq;
    uint32_t slot_index;
    uint32_t payload_lba;
    uint32_t payload_blocks;
    uint32_t flags;
    uint32_t checksum;
    uint32_t reserved[(SD_BLOCK_SIZE / 4u) - 8u];
} snapshot_slot_hdr_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t seq;
    uint32_t slot_index;
    uint32_t checksum;
    uint32_t reserved[(SD_BLOCK_SIZE / 4u) - 5u];
} snapshot_slot_commit_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t reason;
    uint32_t captured_tick;
    uint32_t ready_bitmap;
    uint32_t region_count;
    uint32_t total_region_bytes;
    uint32_t checksum;
    uint32_t reserved[(SD_BLOCK_SIZE / 4u) - 8u];
} snapshot_payload_start_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t region_count;
    uint32_t total_region_bytes;
    uint32_t checksum;
    uint32_t reserved[(SD_BLOCK_SIZE / 4u) - 5u];
} snapshot_payload_end_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t region_addr;
    uint32_t region_len;
    uint32_t checksum;
    uint32_t reserved[(SD_BLOCK_SIZE / 4u) - 5u];
} snapshot_region_start_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t region_addr;
    uint32_t region_len;
    uint32_t region_crc;
    uint32_t checksum;
    uint32_t reserved[(SD_BLOCK_SIZE / 4u) - 6u];
} snapshot_region_end_t;

typedef struct {
    const uint8_t *start;
    uint32_t len;
} snapshot_region_span_t;

typedef struct {
    uint32_t primask;
    uint8_t usart6_enabled;
    uint8_t sdmmc1_enabled;
} snapshot_irq_gate_t;

typedef struct {
    uint8_t enabled;
    uint8_t busy;
    uint8_t step_pending;
    uint8_t restore_mode;
    uint8_t restore_has_candidate;
    uint8_t _pad0;
    uint8_t _pad1;
    uint32_t interval_ticks;
    uint32_t next_tick;
    uint32_t saves_ok;
    uint32_t saves_err;
    int32_t last_err;
    uint32_t last_slot;
    uint32_t last_seq;
    uint32_t last_capture_tick;
    uint32_t last_ready_bitmap;
    uint32_t restore_slot;
    uint32_t restore_seq;
} snapshot_task_ctx_t;

static snapshot_task_ctx_t g_snapshot_ctx;
static event_t g_snapshot_queue[SNAPSHOT_Q_CAP];
static scheduler_t *g_snapshot_sched;
static uint32_t g_snap_block_buf[SD_BLOCK_SIZE / 4u];

void snapshot_task_disarm_hook(void)
{
    g_snapshot_sched = 0;
    g_snapshot_ctx.enabled = 0u;
}

void snapshot_task_rearm_hook(scheduler_t *sched)
{
    if (sched == 0) return;
    if (sched->table[AO_SNAPSHOT] == 0) return;
    g_snapshot_sched = sched;
    g_snapshot_ctx.busy = 0u;
    g_snapshot_ctx.step_pending = 0u;
    g_snapshot_ctx.enabled = 1u;
    g_snapshot_ctx.next_tick = systick_now() + g_snapshot_ctx.interval_ticks;
}

static uint32_t irq_save_all(void)
{
    uint32_t primask;
    __asm__ volatile("mrs %0, primask" : "=r"(primask));
    __asm__ volatile("cpsid i" ::: "memory");
    return primask;
}

static void irq_restore_all(uint32_t primask)
{
    __asm__ volatile("msr primask, %0" : : "r"(primask) : "memory");
}

static uint8_t irq_enabled_bit(uint32_t irqn)
{
    volatile const uint32_t *iser = (volatile const uint32_t *)(NVIC_ISER_BASE + ((irqn / 32u) * 4u));
    return (uint8_t)((*iser >> (irqn % 32u)) & 1u);
}

static void snapshot_irq_gate_enter(snapshot_irq_gate_t *g)
{
    g->primask = irq_save_all();
    g->usart6_enabled = irq_enabled_bit(USART6_IRQn);
    g->sdmmc1_enabled = irq_enabled_bit(SDMMC1_IRQn);
    if (g->usart6_enabled) {
        nvic_disable_irq(USART6_IRQn);
    }
    if (g->sdmmc1_enabled) {
        nvic_disable_irq(SDMMC1_IRQn);
    }
}

static void snapshot_irq_gate_exit(const snapshot_irq_gate_t *g)
{
    if (g->sdmmc1_enabled) {
        nvic_enable_irq(SDMMC1_IRQn);
    }
    if (g->usart6_enabled) {
        nvic_enable_irq(USART6_IRQn);
    }
    irq_restore_all(g->primask);
}

static uint32_t checksum_words(const uint32_t *words, uint32_t count)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0u; i < count; i++) {
        h ^= words[i];
        h *= 16777619u;
    }
    return h ^ 0xA5A55A5Au;
}

static void snapshot_copy_words(uint32_t *dst, const uint32_t *src, uint32_t words)
{
    for (uint32_t i = 0u; i < words; i++) {
        dst[i] = src[i];
    }
}

static uint32_t checksum_bytes_update(uint32_t h, const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0u; i < len; i++) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}

static uint32_t checksum_bytes_finalize(uint32_t h)
{
    return h ^ 0xA5A55A5Au;
}

static uint32_t div_round_up_u32(uint32_t n, uint32_t d)
{
    if (d == 0u) {
        return 0u;
    }
    return (n + d - 1u) / d;
}

static uint32_t clamp_interval_s(uint32_t v)
{
    if (v < SNAPSHOT_INTERVAL_MIN_S) {
        return SNAPSHOT_INTERVAL_DEFAULT_S;
    }
    if (v > SNAPSHOT_INTERVAL_MAX_S) {
        return SNAPSHOT_INTERVAL_DEFAULT_S;
    }
    return v;
}

static void snapshot_out_puts(uint8_t bg, const char *s)
{
    if (bg) {
        log_puts(s);
    } else {
        console_puts(s);
    }
}

static void snapshot_out_put_u32(uint8_t bg, uint32_t v)
{
    if (bg) {
        log_put_u32(v);
    } else {
        console_put_u32(v);
    }
}

static void snapshot_out_put_s32(uint8_t bg, int32_t v)
{
    if (v < 0) {
        snapshot_out_puts(bg, "-");
        snapshot_out_put_u32(bg, (uint32_t)(-v));
        return;
    }
    snapshot_out_put_u32(bg, (uint32_t)v);
}

static uint32_t snapshot_line_append_lit(char *buf, uint32_t cap, uint32_t off, const char *s)
{
    if (buf == 0 || cap == 0u || s == 0) {
        return off;
    }
    while (*s != '\0' && off + 1u < cap) {
        buf[off++] = *s++;
    }
    return off;
}

static uint32_t snapshot_line_append_u32(char *buf, uint32_t cap, uint32_t off, uint32_t v)
{
    char tmp[10];
    uint32_t n = 0u;
    if (v == 0u) {
        return snapshot_line_append_lit(buf, cap, off, "0");
    }

    while (v > 0u && n < (uint32_t)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n > 0u && off + 1u < cap) {
        buf[off++] = tmp[--n];
    }
    return off;
}

static void snapshot_line_emit(uint8_t bg, char *buf, uint32_t cap, uint32_t off)
{
    if (buf == 0 || cap == 0u) {
        return;
    }
    if (off >= cap) {
        off = cap - 1u;
    }
    buf[off] = '\0';
    snapshot_out_puts(bg, buf);
}

static int snapshot_layout_valid(const rwos_disk_t *disk)
{
    uint32_t slots       = disk->snap_slots;
    uint32_t start_lba   = disk->snap_start_lba;
    uint32_t snap_blocks = disk->snap_blocks;
    uint32_t slot_blocks = disk->snap_slot_blocks;

    if (slots < SNAPSHOT_SLOTS_MIN || slots > SNAPSHOT_SLOTS_MAX) {
        return 0;
    }
    if (slot_blocks < SNAPSHOT_MIN_SLOT_BLOCKS) {
        return 0;
    }
    if (snap_blocks < (slots * slot_blocks)) {
        return 0;
    }
    if (start_lba == 0u || (start_lba + snap_blocks) > disk->total_blocks) {
        return 0;
    }
    return 1;
}

static uint32_t snapshot_build_regions(snapshot_region_span_t *regions, uint32_t max_regions, uint32_t *total_bytes)
{
    if (regions == 0 || max_regions == 0u) {
        if (total_bytes != 0) {
            *total_bytes = 0u;
        }
        return 0u;
    }

    const uintptr_t sram_start = (uintptr_t)&_sram_start;
    const uintptr_t sram_end = (uintptr_t)&_sram_end;
    if (sram_end <= sram_start) {
        if (total_bytes != 0) {
            *total_bytes = 0u;
        }
        return 0u;
    }

    typedef struct {
        uintptr_t start;
        uintptr_t end;
    } span_t;

    span_t ex[2];
    uint32_t ex_count = 0u;

    /* Exclude linker-declared scratch/state that must not be captured/restored. */
    uintptr_t ex0_start = (uintptr_t)&_ssnap_exclude;
    uintptr_t ex0_end = (uintptr_t)&_esnap_exclude;
    if (ex0_start < sram_start) {
        ex0_start = sram_start;
    }
    if (ex0_end > sram_end) {
        ex0_end = sram_end;
    }
    if (ex0_end > ex0_start) {
        ex[ex_count].start = ex0_start;
        ex[ex_count].end = ex0_end;
        ex_count++;
    }

    /* Exclude a top-of-SRAM runtime stack window so restore does not clobber itself. */
    uintptr_t ex1_end = sram_end;
    uintptr_t ex1_start = (ex1_end > SNAPSHOT_STACK_EXCLUDE_BYTES)
                        ? (ex1_end - SNAPSHOT_STACK_EXCLUDE_BYTES)
                        : sram_start;
    if (ex1_start < sram_start) {
        ex1_start = sram_start;
    }
    if (ex1_end > ex1_start) {
        ex[ex_count].start = ex1_start;
        ex[ex_count].end = ex1_end;
        ex_count++;
    }

    if (ex_count == 2u && ex[1].start < ex[0].start) {
        span_t t = ex[0];
        ex[0] = ex[1];
        ex[1] = t;
    }
    if (ex_count == 2u && ex[1].start <= ex[0].end) {
        if (ex[1].end > ex[0].end) {
            ex[0].end = ex[1].end;
        }
        ex_count = 1u;
    }

    uint32_t count = 0u;
    uint32_t bytes = 0u;
    uintptr_t cursor = sram_start;
    for (uint32_t i = 0u; i < ex_count; i++) {
        if (cursor < ex[i].start) {
            if (count >= max_regions) {
                if (total_bytes != 0) {
                    *total_bytes = 0u;
                }
                return 0u;
            }
            regions[count].start = (const uint8_t *)cursor;
            regions[count].len = (uint32_t)(ex[i].start - cursor);
            bytes += regions[count].len;
            count++;
        }
        if (cursor < ex[i].end) {
            cursor = ex[i].end;
        }
    }
    if (cursor < sram_end) {
        if (count >= max_regions) {
            if (total_bytes != 0) {
                *total_bytes = 0u;
            }
            return 0u;
        }
        regions[count].start = (const uint8_t *)cursor;
        regions[count].len = (uint32_t)(sram_end - cursor);
        bytes += regions[count].len;
        count++;
    }

    if (count == 0u && sram_end > sram_start) {
        regions[0].start = (const uint8_t *)sram_start;
        regions[0].len = (uint32_t)(sram_end - sram_start);
        bytes = regions[0].len;
        count = 1u;
    }

    if (total_bytes != 0) {
        *total_bytes = bytes;
    }
    return count;
}

static void snapshot_copy_region_chunk_atomic(uint8_t *dst, const uint8_t *src, uint32_t len)
{
    uint32_t key = irq_save_all();
    for (uint32_t i = 0u; i < len; i++) {
        dst[i] = src[i];
    }
    irq_restore_all(key);
}

static int snapshot_write_payload(uint32_t slot_lba,
                                  uint32_t payload_blocks,
                                  uint32_t reason,
                                  uint32_t captured_tick,
                                  uint32_t ready_bitmap)
{
    snapshot_region_span_t regions[4];
    uint32_t total_bytes = 0u;
    uint32_t region_count = snapshot_build_regions(regions, 4u, &total_bytes);
    if (region_count == 0u) {
        return SD_ERR_PARAM;
    }

    uint32_t required_blocks = 2u; /* PAY0 + PAYE */
    for (uint32_t i = 0u; i < region_count; i++) {
        required_blocks += 2u; /* RGS0 + RGE0 */
        required_blocks += div_round_up_u32(regions[i].len, SD_BLOCK_SIZE);
    }
    if (required_blocks > payload_blocks) {
        return SD_ERR_PARAM;
    }

    snapshot_payload_start_t pay0;
    for (uint32_t i = 0u; i < (sizeof(pay0) / sizeof(uint32_t)); i++) {
        ((uint32_t *)&pay0)[i] = 0u;
    }
    pay0.magic = SNAP_PAY0_MAGIC;
    pay0.version = SNAP_FMT_VERSION;
    pay0.reason = reason;
    pay0.captured_tick = captured_tick;
    pay0.ready_bitmap = ready_bitmap;
    pay0.region_count = region_count;
    pay0.total_region_bytes = total_bytes;
    pay0.checksum = checksum_words((const uint32_t *)&pay0, SNAP_PAY0_WORDS);

    int rc = sd_write_blocks(slot_lba + 1u, 1u, &pay0);
    if (rc != SD_OK) {
        LOG_ERR("snap-pay0", rc);
        return rc;
    }

    uint32_t lba = slot_lba + 2u;
    for (uint32_t r = 0u; r < region_count; r++) {
        const uint8_t *src = regions[r].start;
        uint32_t rem = regions[r].len;

        snapshot_region_start_t rs;
        for (uint32_t i = 0u; i < (sizeof(rs) / sizeof(uint32_t)); i++) {
            ((uint32_t *)&rs)[i] = 0u;
        }
        rs.magic = SNAP_RGS0_MAGIC;
        rs.version = SNAP_FMT_VERSION;
        rs.region_addr = (uint32_t)(uintptr_t)regions[r].start;
        rs.region_len = regions[r].len;
        rs.checksum = checksum_words((const uint32_t *)&rs, SNAP_RGS0_WORDS);

        rc = sd_write_blocks(lba, 1u, &rs);
        if (rc != SD_OK) {
            LOG_ERR("snap-rgs", rc);
            return rc;
        }
        lba++;

        uint32_t crc = 2166136261u;
        while (rem > 0u) {
            uint8_t *blk = (uint8_t *)g_snap_block_buf;
            uint32_t n = (rem > SD_BLOCK_SIZE) ? SD_BLOCK_SIZE : rem;
            for (uint32_t i = 0u; i < SD_BLOCK_SIZE; i++) {
                blk[i] = 0u;
            }
            snapshot_copy_region_chunk_atomic(blk, src, n);
            crc = checksum_bytes_update(crc, blk, n);

            rc = sd_write_blocks(lba, 1u, g_snap_block_buf);
            if (rc != SD_OK) {
                LOG_ERR("snap-data", rc);
                return rc;
            }
            lba++;
            src += n;
            rem -= n;
        }

        snapshot_region_end_t re;
        for (uint32_t i = 0u; i < (sizeof(re) / sizeof(uint32_t)); i++) {
            ((uint32_t *)&re)[i] = 0u;
        }
        re.magic = SNAP_RGE0_MAGIC;
        re.version = SNAP_FMT_VERSION;
        re.region_addr = (uint32_t)(uintptr_t)regions[r].start;
        re.region_len = regions[r].len;
        re.region_crc = checksum_bytes_finalize(crc);
        re.checksum = checksum_words((const uint32_t *)&re, SNAP_RGE0_WORDS);

        rc = sd_write_blocks(lba, 1u, &re);
        if (rc != SD_OK) {
            LOG_ERR("snap-rge", rc);
            return rc;
        }
        lba++;
    }

    snapshot_payload_end_t paye;
    for (uint32_t i = 0u; i < (sizeof(paye) / sizeof(uint32_t)); i++) {
        ((uint32_t *)&paye)[i] = 0u;
    }
    paye.magic = SNAP_PAYE_MAGIC;
    paye.version = SNAP_FMT_VERSION;
    paye.region_count = region_count;
    paye.total_region_bytes = total_bytes;
    paye.checksum = checksum_words((const uint32_t *)&paye, SNAP_PAYE_WORDS);

    rc = sd_write_blocks(lba, 1u, &paye);
    if (rc != SD_OK) {
        LOG_ERR("snap-paye", rc);
        return rc;
    }
    return SD_OK;
}

static int snapshot_read_block_retry(uint32_t lba, void *buf);

static int snapshot_slot_valid(uint32_t slot_lba, uint32_t slot_blocks, uint32_t slot_idx, uint32_t *seq_out)
{
    int rc = snapshot_read_block_retry(slot_lba, g_snap_block_buf);
    if (rc != SD_OK) {
        log_puts("slotval: read hdr rc=");
        log_put_s32(rc);
        log_puts("\n");
        return 0;
    }

    const snapshot_slot_hdr_t *hdr = (const snapshot_slot_hdr_t *)g_snap_block_buf;
    if (hdr->magic != SNAP_HDR_MAGIC || hdr->version != SNAP_FMT_VERSION) {
        log_puts("slotval: bad magic/ver m=");
        log_put_hex32(hdr->magic);
        log_puts(" v=");
        log_put_u32(hdr->version);
        log_puts("\n");
        return 0;
    }
    if (hdr->slot_index != slot_idx) {
        log_puts("slotval: bad idx\n");
        return 0;
    }
    if (hdr->payload_lba != (slot_lba + 1u) || hdr->payload_blocks != (slot_blocks - 2u)) {
        log_puts("slotval: bad layout plba=");
        log_put_u32(hdr->payload_lba);
        log_puts(" pb=");
        log_put_u32(hdr->payload_blocks);
        log_puts("\n");
        return 0;
    }
    if (hdr->checksum != checksum_words((const uint32_t *)hdr, SNAP_HDR_WORDS)) {
        log_puts("slotval: bad hdr cksum\n");
        return 0;
    }

    /* Save seq before buffer is reused for next read */
    uint32_t hdr_seq = hdr->seq;

    rc = snapshot_read_block_retry(slot_lba + 1u, g_snap_block_buf);
    if (rc != SD_OK) {
        log_puts("slotval: read pay0 rc=");
        log_put_s32(rc);
        log_puts("\n");
        return 0;
    }
    const snapshot_payload_start_t *pay0 = (const snapshot_payload_start_t *)g_snap_block_buf;
    if (pay0->magic != SNAP_PAY0_MAGIC || pay0->version != SNAP_FMT_VERSION) {
        log_puts("slotval: bad pay0 m=");
        log_put_hex32(pay0->magic);
        log_puts("\n");
        return 0;
    }
    if (pay0->checksum != checksum_words((const uint32_t *)pay0, SNAP_PAY0_WORDS)) {
        log_puts("slotval: bad pay0 cksum\n");
        return 0;
    }

    rc = snapshot_read_block_retry(slot_lba + slot_blocks - 1u, g_snap_block_buf);
    if (rc != SD_OK) {
        log_puts("slotval: read cmit rc=");
        log_put_s32(rc);
        log_puts("\n");
        return 0;
    }
    const snapshot_slot_commit_t *c = (const snapshot_slot_commit_t *)g_snap_block_buf;
    if (c->magic != SNAP_CMIT_MAGIC || c->version != SNAP_FMT_VERSION) {
        log_puts("slotval: bad cmit m=");
        log_put_hex32(c->magic);
        log_puts("\n");
        return 0;
    }
    if (c->slot_index != slot_idx || c->seq != hdr_seq) {
        log_puts("slotval: bad cmit idx/seq\n");
        return 0;
    }
    if (c->checksum != checksum_words((const uint32_t *)c, SNAP_CMIT_WORDS)) {
        log_puts("slotval: bad cmit cksum\n");
        return 0;
    }

    if (seq_out != 0) {
        *seq_out = hdr_seq;
    }
    return 1;
}

static void snapshot_scan_latest(const rwos_disk_t *disk,
                                 uint32_t *have_latest,
                                 uint32_t *latest_slot,
                                 uint32_t *latest_seq)
{
    uint32_t start_lba   = disk->snap_start_lba;
    uint32_t slot_blocks = disk->snap_slot_blocks;
    uint32_t slots       = disk->snap_slots;

    uint32_t found = 0u;
    uint32_t best_slot = 0u;
    uint32_t best_seq = 0u;
    for (uint32_t i = 0u; i < slots; i++) {
        uint32_t seq = 0u;
        if (!snapshot_slot_valid(start_lba + (i * slot_blocks), slot_blocks, i, &seq)) {
            continue;
        }
        if (!found || seq > best_seq) {
            found = 1u;
            best_seq = seq;
            best_slot = i;
        }
    }

    if (have_latest != 0) {
        *have_latest = found;
    }
    if (latest_slot != 0) {
        *latest_slot = best_slot;
    }
    if (latest_seq != 0) {
        *latest_seq = best_seq;
    }
}

static void snapshot_consume_boot_target(const rwos_disk_t *disk)
{
    g_snapshot_ctx.restore_mode = (uint8_t)(disk->boot_target_mode & 0xFFu);
    g_snapshot_ctx.restore_has_candidate = 0u;
    g_snapshot_ctx.restore_slot = 0u;
    g_snapshot_ctx.restore_seq = 0u;

    if (g_snapshot_ctx.restore_mode == SNAP_TARGET_FRESH) {
        return;
    }

    uint32_t slot_blocks = disk->snap_slot_blocks;
    uint32_t start_lba   = disk->snap_start_lba;

    if (g_snapshot_ctx.restore_mode == SNAP_TARGET_SNAPSHOT) {
        uint32_t req_slot = disk->boot_target_slot;
        uint32_t req_seq  = disk->boot_target_seq;
        if (req_slot < disk->snap_slots) {
            uint32_t slot_lba = start_lba + (req_slot * slot_blocks);
            uint32_t found_seq = 0u;
            if (snapshot_slot_valid(slot_lba, slot_blocks, req_slot, &found_seq)) {
                if (req_seq == 0u || req_seq == found_seq) {
                    g_snapshot_ctx.restore_has_candidate = 1u;
                    g_snapshot_ctx.restore_slot = req_slot;
                    g_snapshot_ctx.restore_seq = found_seq;
                    return;
                }
            }
        }
    }

    /* recent target, or explicit snapshot target that is no longer valid:
     * fall back to the latest valid slot. */
    uint32_t have_latest = 0u;
    uint32_t latest_slot = 0u;
    uint32_t latest_seq = 0u;
    snapshot_scan_latest(disk, &have_latest, &latest_slot, &latest_seq);
    if (have_latest) {
        g_snapshot_ctx.restore_has_candidate = 1u;
        g_snapshot_ctx.restore_slot = latest_slot;
        g_snapshot_ctx.restore_seq = latest_seq;
    }
}

static void snapshot_pause_mutators(void)
{
    if (g_snapshot_sched == 0) {
        return;
    }
    (void)sched_pause_accept(g_snapshot_sched, AO_TERMINAL);
    (void)sched_pause_accept(g_snapshot_sched, AO_CMD);
    (void)sched_pause_accept(g_snapshot_sched, AO_CONSOLE);
    (void)sched_pause_accept(g_snapshot_sched, AO_COUNTER);
}

static void snapshot_resume_mutators(void)
{
    if (g_snapshot_sched == 0) {
        return;
    }
    (void)sched_resume_accept(g_snapshot_sched, AO_COUNTER);
    (void)sched_resume_accept(g_snapshot_sched, AO_CONSOLE);
    (void)sched_resume_accept(g_snapshot_sched, AO_CMD);
    (void)sched_resume_accept(g_snapshot_sched, AO_TERMINAL);
}

static void snapshot_capture_quiesced(uint32_t *captured_tick, uint32_t *ready_bitmap)
{
    snapshot_irq_gate_t gate;
    snapshot_irq_gate_enter(&gate);
    snapshot_pause_mutators();
    if (captured_tick != 0) {
        *captured_tick = systick_now();
    }
    if (ready_bitmap != 0) {
        *ready_bitmap = (g_snapshot_sched != 0) ? g_snapshot_sched->ready_bitmap : 0u;
    }
    snapshot_resume_mutators();
    snapshot_irq_gate_exit(&gate);
}

static int snapshot_read_block_retry(uint32_t lba, void *buf)
{
    int rc = sd_read_blocks(lba, 1u, buf);
    if (rc == SD_OK) {
        return SD_OK;
    }

    /* On read failure, do one retry without full reinit.  sd_init() can
     * spin ACMD41 for ~6 s per call, which makes slot scanning very slow
     * (4 slots × 3 reads × retry = 72 s worst case).  A single retry is
     * enough to recover from transient bus glitches. */
    return sd_read_blocks(lba, 1u, buf);
}

static int snapshot_try_load_policy(uint32_t *interval_ticks_out)
{
    const rwos_disk_t *disk = rwos_disk_get();
    if (disk == 0) {
        return SD_ERR_NO_INIT;
    }
    if (!snapshot_layout_valid(disk)) {
        return SD_ERR_PARAM;
    }
    uint32_t interval_s = clamp_interval_s(disk->snap_interval_s);
    if (interval_ticks_out != 0) {
        *interval_ticks_out = interval_s * 1000u;
    }
    return SD_OK;
}

static int snapshot_write_one(uint32_t reason, uint32_t *slot_out, uint32_t *seq_out)
{
    if (g_sd_ctx.operation != SD_OP_NONE || g_sd_ctx.status == DRV_IN_PROGRESS) {
        return SCHED_ERR_DISABLED;
    }

    const rwos_disk_t *disk = rwos_disk_get();
    if (disk == 0) {
        return SD_ERR_NO_INIT;
    }
    if (!snapshot_layout_valid(disk)) {
        return SD_ERR_PARAM;
    }

    uint32_t have_latest = 0u;
    uint32_t latest_slot = 0u;
    uint32_t latest_seq = 0u;
    snapshot_scan_latest(disk, &have_latest, &latest_slot, &latest_seq);

    uint32_t slot_idx    = have_latest ? ((latest_slot + 1u) % disk->snap_slots) : 0u;
    uint32_t seq         = have_latest ? (latest_seq + 1u) : 1u;
    uint32_t slot_blocks = disk->snap_slot_blocks;
    uint32_t slot_lba    = disk->snap_start_lba + (slot_idx * slot_blocks);
    int rc;

    uint32_t captured_tick = 0u;
    uint32_t ready_bitmap = 0u;
    snapshot_capture_quiesced(&captured_tick, &ready_bitmap);

    snapshot_slot_hdr_t hdr;
    for (uint32_t i = 0u; i < (sizeof(hdr) / sizeof(uint32_t)); i++) {
        ((uint32_t *)&hdr)[i] = 0u;
    }
    hdr.magic = SNAP_HDR_MAGIC;
    hdr.version = SNAP_FMT_VERSION;
    hdr.seq = seq;
    hdr.slot_index = slot_idx;
    hdr.payload_lba = slot_lba + 1u;
    hdr.payload_blocks = slot_blocks - 2u;
    hdr.flags = 1u;
    hdr.checksum = checksum_words((const uint32_t *)&hdr, SNAP_HDR_WORDS);

    rc = sd_write_blocks(slot_lba, 1u, &hdr);
    if (rc != SD_OK) {
        LOG_ERR("snap-hdr", rc);
        return rc;
    }

    rc = snapshot_write_payload(slot_lba, slot_blocks - 2u, reason, captured_tick, ready_bitmap);
    if (rc != SD_OK) {
        return rc;
    }

    snapshot_slot_commit_t c;
    for (uint32_t i = 0u; i < (sizeof(c) / sizeof(uint32_t)); i++) {
        ((uint32_t *)&c)[i] = 0u;
    }
    c.magic = SNAP_CMIT_MAGIC;
    c.version = SNAP_FMT_VERSION;
    c.seq = seq;
    c.slot_index = slot_idx;
    c.checksum = checksum_words((const uint32_t *)&c, SNAP_CMIT_WORDS);

    rc = sd_write_blocks(slot_lba + slot_blocks - 1u, 1u, &c);
    if (rc != SD_OK) {
        LOG_ERR("snap-cmit", rc);
        return rc;
    }

    (void)rwos_disk_set_last_snap(slot_idx, seq);

    g_snapshot_ctx.last_slot = slot_idx;
    g_snapshot_ctx.last_seq = seq;
    g_snapshot_ctx.last_capture_tick = captured_tick;
    g_snapshot_ctx.last_ready_bitmap = ready_bitmap;
    journal_note_snapshot_seq(seq);

    if (slot_out != 0) {
        *slot_out = slot_idx;
    }
    if (seq_out != 0) {
        *seq_out = seq;
    }
    return SD_OK;
}

static void snapshot_task_dispatch(ao_t *self, const event_t *e)
{
    (void)self;
    if (e == 0 || e->sig != SNAP_SIG_TRIGGER) {
        return;
    }

    if (!g_snapshot_ctx.enabled || g_snapshot_ctx.busy) {
        g_snapshot_ctx.step_pending = 0u;
        return;
    }

    g_snapshot_ctx.busy = 1u;

    uint8_t manual = (uint8_t)(e->arg0 & 1u);
    (void)e->src;
    int rc = snapshot_write_one(manual ? SNAPSHOT_REASON_MANUAL : SNAPSHOT_REASON_PERIODIC,
                                0, 0);
    g_snapshot_ctx.last_err = rc;
    if (rc == SD_OK) {
        g_snapshot_ctx.saves_ok++;
        if (!manual) {
            log_puts("snapshot: ok slot=");
            log_put_u32(g_snapshot_ctx.last_slot);
            log_puts(" seq=");
            log_put_u32(g_snapshot_ctx.last_seq);
            log_puts("\r\n");
        }
    } else {
        g_snapshot_ctx.saves_err++;
        /* Reset SDMMC peripheral so the card doesn't stay wedged for the
         * next attempt.  sd_async_init() clears controller state and
         * sd_init() re-enumerates the card. */
        extern void sd_async_init(void);
        sd_async_init();
        (void)sd_init();
    }

    if (manual) {
        snapshot_out_puts(1u, "snapshot: ");
        if (rc == SD_OK) {
            snapshot_out_puts(1u, "ok slot=");
            snapshot_out_put_u32(1u, g_snapshot_ctx.last_slot);
            snapshot_out_puts(1u, " seq=");
            snapshot_out_put_u32(1u, g_snapshot_ctx.last_seq);
            snapshot_out_puts(1u, "\r\n");
        } else {
            snapshot_out_puts(1u, "err=");
            snapshot_out_put_s32(1u, rc);
            snapshot_out_puts(1u, " cmd=");
            log_put_hex32(sd_last_cmd());
            snapshot_out_puts(1u, " sta=");
            log_put_hex32(sd_last_sta());
            snapshot_out_puts(1u, "\r\n");
        }
    } else if (rc != SD_OK) {
        LOG_ERR("snapshot", rc);
        log_puts("  wstg=");
        log_put_u32(sd_dbg_write_stage());
        log_puts(" wsta=");
        log_put_hex32(sd_dbg_write_last_sta());
        log_puts("\r\n");
    }

    g_snapshot_ctx.step_pending = 0u;
    g_snapshot_ctx.busy = 0u;
    g_snapshot_ctx.next_tick = systick_now() + g_snapshot_ctx.interval_ticks;
}

int snapshot_task_register(scheduler_t *sched)
{
    if (sched == 0) {
        return SCHED_ERR_PARAM;
    }

    PANIC_IF(sizeof(snapshot_slot_hdr_t) > SD_BLOCK_SIZE, "snapshot hdr struct > block");
    PANIC_IF(sizeof(snapshot_slot_commit_t) > SD_BLOCK_SIZE, "snapshot cmit struct > block");
    PANIC_IF(sizeof(snapshot_payload_start_t) > SD_BLOCK_SIZE, "snapshot pay0 struct > block");
    PANIC_IF(sizeof(snapshot_payload_end_t) > SD_BLOCK_SIZE, "snapshot paye struct > block");
    PANIC_IF(sizeof(snapshot_region_start_t) > SD_BLOCK_SIZE, "snapshot rgs0 struct > block");
    PANIC_IF(sizeof(snapshot_region_end_t) > SD_BLOCK_SIZE, "snapshot rge0 struct > block");

    g_snapshot_sched = sched;
    g_snapshot_ctx.enabled = 0u;
    g_snapshot_ctx.busy = 0u;
    g_snapshot_ctx.step_pending = 0u;
    g_snapshot_ctx.restore_mode = SNAP_TARGET_RECENT;
    g_snapshot_ctx.restore_has_candidate = 0u;
    g_snapshot_ctx._pad0 = 0u;
    g_snapshot_ctx._pad1 = 0u;
    g_snapshot_ctx.interval_ticks = SNAPSHOT_INTERVAL_DEFAULT_S * 1000u;
    g_snapshot_ctx.next_tick = systick_now() + g_snapshot_ctx.interval_ticks;
    g_snapshot_ctx.saves_ok = 0u;
    g_snapshot_ctx.saves_err = 0u;
    g_snapshot_ctx.last_err = SD_OK;
    g_snapshot_ctx.last_slot = 0u;
    g_snapshot_ctx.last_seq = 0u;
    g_snapshot_ctx.last_capture_tick = 0u;
    g_snapshot_ctx.last_ready_bitmap = 0u;
    g_snapshot_ctx.restore_slot = 0u;
    g_snapshot_ctx.restore_seq = 0u;

    task_spec_t spec;
    spec.id = AO_SNAPSHOT;
    spec.prio = 2;
    spec.dispatch = snapshot_task_dispatch;
    spec.ctx = &g_snapshot_ctx;
    spec.queue_storage = g_snapshot_queue;
    spec.queue_capacity = (uint16_t)(sizeof(g_snapshot_queue) / sizeof(g_snapshot_queue[0]));
    spec.rtc_budget_ticks = 1;
    spec.name = "snapshot";

    journal_configure(0u, 0u, 0u);
    return sched_register_task(sched, &spec);
}

int snapshot_task_boot_init(void)
{
    uart_puts("snapshot: boot init\r\n");

    const rwos_disk_t *disk = rwos_disk_get();
    if (disk == 0 || !sd_get_info()->initialized) {
        uart_puts("snapshot: disk not ready, disabled\r\n");
        g_snapshot_ctx.enabled = 0u;
        g_snapshot_ctx.last_err = SD_ERR_NO_INIT;
        return SD_ERR_NO_INIT;
    }

    if (!snapshot_layout_valid(disk)) {
        uart_puts("snapshot: layout invalid, disabled\r\n");
        g_snapshot_ctx.enabled = 0u;
        g_snapshot_ctx.last_err = SD_ERR_PARAM;
        return SD_ERR_PARAM;
    }

    uint32_t interval_s = clamp_interval_s(disk->snap_interval_s);
    g_snapshot_ctx.enabled = 1u;
    g_snapshot_ctx.interval_ticks = interval_s * 1000u;
    g_snapshot_ctx.next_tick = systick_now() + g_snapshot_ctx.interval_ticks;
    g_snapshot_ctx.last_err = SD_OK;
    g_snapshot_ctx.last_seq = disk->last_snap_seq;

    /* Only scan slots when there are snapshots to find.  A freshly formatted
     * disk has last_snap_seq==0 — scanning empty slots wastes seconds on SD
     * reads that all return garbage headers. */
    if (disk->last_snap_seq != 0u) {
        uart_puts("snapshot: scanning boot target\r\n");
        snapshot_consume_boot_target(disk);
    } else {
        uart_puts("snapshot: fresh disk, skip scan\r\n");
    }

    journal_configure(disk->journal_start_lba,
                      disk->journal_blocks,
                      disk->last_snap_seq);

    uart_puts("snapshot: boot init ok\r\n");
    return SD_OK;
}

int snapshot_task_request_now(void)
{
    if (g_snapshot_sched == 0) {
        return SCHED_ERR_PARAM;
    }

    uint32_t interval_ticks = 0u;
    int cfg_rc = snapshot_try_load_policy(&interval_ticks);
    if (cfg_rc == SD_OK) {
        g_snapshot_ctx.enabled = 1u;
        g_snapshot_ctx.interval_ticks = interval_ticks;
    } else {
        g_snapshot_ctx.enabled = 0u;
        g_snapshot_ctx.last_err = cfg_rc;
        return SCHED_ERR_DISABLED;
    }

    uint8_t bg = (uint8_t)(g_cmd_bg_ctx & 1u);
    if (bg) {
        g_cmd_bg_async = 1u;
    }

    return sched_post(g_snapshot_sched, AO_SNAPSHOT,
                      &(event_t){ .sig = SNAP_SIG_TRIGGER,
                                  .src = (uint16_t)bg,
                                  .arg0 = (uintptr_t)1u });
}

void snapshot_task_systick_hook(void)
{
    if (g_snapshot_sched == 0 || !g_snapshot_ctx.enabled) {
        return;
    }
    if (g_snapshot_sched->table[AO_SNAPSHOT] == 0) {
        g_snapshot_ctx.enabled = 0u;
        g_snapshot_ctx.step_pending = 0u;
        return;
    }
    if (g_snapshot_ctx.busy || g_snapshot_ctx.step_pending) {
        return;
    }
    if ((int32_t)(g_ticks - g_snapshot_ctx.next_tick) < 0) {
        return;
    }

    int rc = sched_post_isr(g_snapshot_sched, AO_SNAPSHOT,
                            &(event_t){ .sig = SNAP_SIG_TRIGGER,
                                        .arg0 = (uintptr_t)0u });
    if (rc == SCHED_OK) {
        g_snapshot_ctx.step_pending = 1u;
    }
}

void snapshot_task_get_stats(snapshot_task_stats_t *out)
{
    if (out == 0) {
        return;
    }
    out->enabled = g_snapshot_ctx.enabled;
    out->busy = g_snapshot_ctx.busy;
    out->restore_mode = g_snapshot_ctx.restore_mode;
    out->restore_has_candidate = g_snapshot_ctx.restore_has_candidate;
    out->interval_s = g_snapshot_ctx.interval_ticks / 1000u;
    out->next_tick = g_snapshot_ctx.next_tick;
    out->saves_ok = g_snapshot_ctx.saves_ok;
    out->saves_err = g_snapshot_ctx.saves_err;
    out->last_err = g_snapshot_ctx.last_err;
    out->last_slot = g_snapshot_ctx.last_slot;
    out->last_seq = g_snapshot_ctx.last_seq;
    out->last_capture_tick = g_snapshot_ctx.last_capture_tick;
    out->last_ready_bitmap = g_snapshot_ctx.last_ready_bitmap;
    out->restore_slot = g_snapshot_ctx.restore_slot;
    out->restore_seq = g_snapshot_ctx.restore_seq;
}

int snapshot_task_list_slots(void)
{
    uint8_t bg = (uint8_t)(g_cmd_bg_ctx & 1u);
    char line[128];
    uint8_t slot_valid[SNAPSHOT_SLOTS_MAX];
    uint32_t slot_seq[SNAPSHOT_SLOTS_MAX];
    if (g_sd_ctx.operation != SD_OP_NONE || g_sd_ctx.status == DRV_IN_PROGRESS) {
        return SCHED_ERR_DISABLED;
    }

    const rwos_disk_t *disk = rwos_disk_get();
    if (disk == 0) {
        return SD_ERR_NO_INIT;
    }

    {
        uint32_t off = 0u;
        off = snapshot_line_append_lit(line, sizeof(line), off, "snapls: target=");
        if (disk->boot_target_mode == SNAP_TARGET_FRESH) {
            off = snapshot_line_append_lit(line, sizeof(line), off, "fresh");
        } else if (disk->boot_target_mode == SNAP_TARGET_SNAPSHOT) {
            off = snapshot_line_append_lit(line, sizeof(line), off, "snapshot");
        } else {
            off = snapshot_line_append_lit(line, sizeof(line), off, "recent");
        }
        off = snapshot_line_append_lit(line, sizeof(line), off, " slot=");
        off = snapshot_line_append_u32(line, sizeof(line), off, disk->boot_target_slot);
        off = snapshot_line_append_lit(line, sizeof(line), off, " seq=");
        off = snapshot_line_append_u32(line, sizeof(line), off, disk->boot_target_seq);
        off = snapshot_line_append_lit(line, sizeof(line), off, "\r\n");
        snapshot_line_emit(bg, line, sizeof(line), off);
    }

    if (!snapshot_layout_valid(disk)) {
        return SD_ERR_PARAM;
    }

    snapshot_consume_boot_target(disk);
    {
        uint32_t off = 0u;
        off = snapshot_line_append_lit(line, sizeof(line), off, "snapls: restore candidate=");
        off = snapshot_line_append_u32(line, sizeof(line), off, g_snapshot_ctx.restore_has_candidate);
        off = snapshot_line_append_lit(line, sizeof(line), off, " slot=");
        off = snapshot_line_append_u32(line, sizeof(line), off, g_snapshot_ctx.restore_slot);
        off = snapshot_line_append_lit(line, sizeof(line), off, " seq=");
        off = snapshot_line_append_u32(line, sizeof(line), off, g_snapshot_ctx.restore_seq);
        off = snapshot_line_append_lit(line, sizeof(line), off, "\r\n");
        snapshot_line_emit(bg, line, sizeof(line), off);
    }

    {
        uint32_t off = 0u;
        off = snapshot_line_append_lit(line, sizeof(line), off, "snapls: slots=");
        off = snapshot_line_append_u32(line, sizeof(line), off, disk->snap_slots);
        off = snapshot_line_append_lit(line, sizeof(line), off, " start_lba=");
        off = snapshot_line_append_u32(line, sizeof(line), off, disk->snap_start_lba);
        off = snapshot_line_append_lit(line, sizeof(line), off, " slot_blocks=");
        off = snapshot_line_append_u32(line, sizeof(line), off, disk->snap_slot_blocks);
        off = snapshot_line_append_lit(line, sizeof(line), off, "\r\n");
        snapshot_line_emit(bg, line, sizeof(line), off);
    }

    for (uint32_t i = 0u; i < SNAPSHOT_SLOTS_MAX; i++) {
        slot_valid[i] = 0u;
        slot_seq[i] = 0u;
    }

    uint32_t latest_slot = 0u;
    uint32_t latest_seq = 0u;
    uint32_t have_latest = 0u;
    uint32_t start_lba   = disk->snap_start_lba;
    uint32_t slot_blocks = disk->snap_slot_blocks;
    for (uint32_t i = 0u; i < disk->snap_slots && i < SNAPSHOT_SLOTS_MAX; i++) {
        uint32_t seq = 0u;
        uint32_t slot_lba = start_lba + (i * slot_blocks);
        if (snapshot_slot_valid(slot_lba, slot_blocks, i, &seq)) {
            slot_valid[i] = 1u;
            slot_seq[i] = seq;
            if (!have_latest || seq > latest_seq) {
                have_latest = 1u;
                latest_slot = i;
                latest_seq = seq;
            }
        }
    }

    if (have_latest) {
        uint32_t off = 0u;
        off = snapshot_line_append_lit(line, sizeof(line), off, "snapls: latest slot=");
        off = snapshot_line_append_u32(line, sizeof(line), off, latest_slot);
        off = snapshot_line_append_lit(line, sizeof(line), off, " seq=");
        off = snapshot_line_append_u32(line, sizeof(line), off, latest_seq);
        off = snapshot_line_append_lit(line, sizeof(line), off, "\r\n");
        snapshot_line_emit(bg, line, sizeof(line), off);
    } else {
        snapshot_line_emit(bg, line, sizeof(line),
                           snapshot_line_append_lit(line, sizeof(line), 0u, "snapls: latest none\r\n"));
    }

    for (uint32_t i = 0u; i < disk->snap_slots; i++) {
        uint32_t slot_lba = start_lba + (i * slot_blocks);
        uint32_t off = 0u;
        off = snapshot_line_append_lit(line, sizeof(line), off, "  slot ");
        off = snapshot_line_append_u32(line, sizeof(line), off, i);
        off = snapshot_line_append_lit(line, sizeof(line), off, " lba=");
        off = snapshot_line_append_u32(line, sizeof(line), off, slot_lba);
        off = snapshot_line_append_lit(line, sizeof(line), off, " ");
        if (i < SNAPSHOT_SLOTS_MAX && slot_valid[i]) {
            off = snapshot_line_append_lit(line, sizeof(line), off, "valid seq=");
            off = snapshot_line_append_u32(line, sizeof(line), off, slot_seq[i]);
        } else {
            off = snapshot_line_append_lit(line, sizeof(line), off, "empty/invalid");
        }
        off = snapshot_line_append_lit(line, sizeof(line), off, "\r\n");
        snapshot_line_emit(bg, line, sizeof(line), off);
    }

    snapshot_out_puts(bg, "snapls: done\r\n");
    return SCHED_OK;
}
