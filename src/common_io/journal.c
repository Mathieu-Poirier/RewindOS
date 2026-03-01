#include "../../include/journal.h"

#include "../../include/sd.h"
#include "../../include/sd_async.h"
#include "../../include/uart_async.h"
#include "../../include/uart.h"
#include "../../include/terminal.h"
#include "../../include/panic.h"
#include "../../include/log.h"

#define JRNL_MAGIC 0x4C4E524Au /* "JRNL" */
#define JEND_MAGIC 0x444E454Au /* "JEND" */

#define JRNL_EVT_RX_BYTES 1u
#define JRNL_EVT_IO_OWNER 2u

#define JRNL_HDR_WORDS_NO_CRC 6u
#define JRNL_TAIL_WORDS_NO_CRC 3u

#define JRNL_PENDING_RX_CAP 64u

typedef struct {
    uint32_t start_magic;
    uint32_t rec_seq;
    uint32_t snap_seq_base;
    uint32_t event_type;
    uint32_t payload_len;
    uint32_t payload_crc;
    uint32_t hdr_crc;
} journal_hdr_t;

typedef struct {
    uint32_t end_magic;
    uint32_t rec_seq;
    uint32_t payload_crc;
    uint32_t tail_crc;
} journal_tail_t;

#define JRNL_PAYLOAD_MAX (SD_BLOCK_SIZE - (uint32_t)sizeof(journal_hdr_t) - (uint32_t)sizeof(journal_tail_t))

typedef struct {
    uint8_t configured;
    uint8_t scan_ready;
    uint8_t replaying;
    uint8_t _pad0;
    uint16_t pending_len;
    uint16_t _pad1;
    uint32_t start_lba;
    uint32_t blocks;
    uint32_t next_lba;
    uint32_t next_seq;
    uint32_t last_snapshot_seq;
    uint32_t suppress_rx_bytes;
    int32_t last_err;
    uint32_t dropped;
    uint8_t pending_rx[JRNL_PENDING_RX_CAP];
} journal_ctx_t;

__attribute__((section(".snap_exclude"), aligned(8)))
static journal_ctx_t g_journal;

__attribute__((section(".snap_exclude"), aligned(8)))
static uint32_t g_journal_block[SD_BLOCK_SIZE / 4u];

static uint32_t jrnl_checksum_words(const uint32_t *words, uint32_t count)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0u; i < count; i++) {
        h ^= words[i];
        h *= 16777619u;
    }
    return h ^ 0xA5A55A5Au;
}

static uint32_t jrnl_checksum_bytes(const uint8_t *bytes, uint32_t len)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0u; i < len; i++) {
        h ^= bytes[i];
        h *= 16777619u;
    }
    return h ^ 0xA5A55A5Au;
}

static int jrnl_media_ready(void)
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

static int jrnl_block_validate(const uint32_t *blk,
                               journal_hdr_t *hdr_out,
                               const uint8_t **payload_out,
                               journal_tail_t *tail_out)
{
    const journal_hdr_t *h = (const journal_hdr_t *)blk;
    if (h->start_magic != JRNL_MAGIC) {
        return 0;
    }
    if (h->payload_len > JRNL_PAYLOAD_MAX) {
        return 0;
    }
    if (h->hdr_crc != jrnl_checksum_words((const uint32_t *)h, JRNL_HDR_WORDS_NO_CRC)) {
        return 0;
    }

    const uint8_t *payload = ((const uint8_t *)blk) + sizeof(journal_hdr_t);
    const uint8_t *tail_ptr = payload + h->payload_len;
    if ((uint32_t)(tail_ptr - (const uint8_t *)blk) > (SD_BLOCK_SIZE - (uint32_t)sizeof(journal_tail_t))) {
        return 0;
    }

    const journal_tail_t *t = (const journal_tail_t *)tail_ptr;
    if (t->end_magic != JEND_MAGIC) {
        return 0;
    }
    if (t->rec_seq != h->rec_seq) {
        return 0;
    }
    if (t->payload_crc != h->payload_crc) {
        return 0;
    }
    if (t->tail_crc != jrnl_checksum_words((const uint32_t *)t, JRNL_TAIL_WORDS_NO_CRC)) {
        return 0;
    }
    if (h->payload_crc != jrnl_checksum_bytes(payload, h->payload_len)) {
        return 0;
    }

    if (hdr_out != 0) {
        *hdr_out = *h;
    }
    if (payload_out != 0) {
        *payload_out = payload;
    }
    if (tail_out != 0) {
        *tail_out = *t;
    }
    return 1;
}

static int jrnl_seq_newer(uint32_t a, uint32_t b)
{
    return ((int32_t)(a - b)) > 0;
}

static void jrnl_scan_tail(void)
{
    g_journal.scan_ready = 0u;

    if (!g_journal.configured || g_journal.blocks == 0u) {
        return;
    }

    uart_puts("jrnl: scan media\r\n");
    int rc = jrnl_media_ready();
    if (rc != SD_OK) {
        g_journal.last_err = rc;
        /* Card not working — start from block 0 so we don't block later. */
        g_journal.next_seq = 1u;
        g_journal.next_lba = g_journal.start_lba;
        g_journal.scan_ready = 1u;
        uart_puts("jrnl: scan media fail\r\n");
        return;
    }

    uart_puts("jrnl: scan ");
    uart_put_u32(g_journal.blocks);
    uart_puts(" blks\r\n");

    uint32_t have = 0u;
    uint32_t best_seq = 0u;
    uint32_t best_idx = 0u;

    for (uint32_t i = 0u; i < g_journal.blocks; i++) {
        uint32_t lba = g_journal.start_lba + i;
        rc = sd_read_blocks(lba, 1u, g_journal_block);
        if (rc != SD_OK) {
            /* Abort scan on first read failure — don't block the whole
             * boot trying 128 reads against a flaky card.  Use whatever
             * we found so far, or start from block 0. */
            g_journal.last_err = rc;
            uart_puts("jrnl: scan read fail i=");
            uart_put_u32(i);
            uart_puts("\r\n");
            break;
        }

        journal_hdr_t h;
        if (!jrnl_block_validate(g_journal_block, &h, 0, 0)) {
            continue;
        }

        if (!have || jrnl_seq_newer(h.rec_seq, best_seq)) {
            have = 1u;
            best_seq = h.rec_seq;
            best_idx = i;
        }
    }

    if (!have) {
        g_journal.next_seq = 1u;
        g_journal.next_lba = g_journal.start_lba;
        g_journal.scan_ready = 1u;
        g_journal.last_err = SD_OK;
        return;
    }

    g_journal.next_seq = best_seq + 1u;
    g_journal.next_lba = g_journal.start_lba + ((best_idx + 1u) % g_journal.blocks);
    g_journal.scan_ready = 1u;
    g_journal.last_err = SD_OK;
    uart_puts("jrnl: scan done seq=");
    uart_put_u32(best_seq);
    uart_puts("\r\n");
}

static int jrnl_append_record(uint32_t event_type, const uint8_t *payload, uint16_t payload_len)
{
    PANIC_IF(payload_len > JRNL_PAYLOAD_MAX, "journal payload too large");

    if (!g_journal.configured || g_journal.blocks == 0u) {
        return SCHED_ERR_DISABLED;
    }

    if (g_journal.replaying) {
        return SCHED_ERR_DISABLED;
    }

    if (!g_journal.scan_ready) {
        jrnl_scan_tail();
        if (!g_journal.scan_ready) {
            return SCHED_ERR_DISABLED;
        }
    }

    if (g_sd_ctx.operation != SD_OP_NONE || g_sd_ctx.status == DRV_IN_PROGRESS) {
        g_journal.dropped++;
        return SCHED_ERR_DISABLED;
    }

    int rc = jrnl_media_ready();
    if (rc != SD_OK) {
        g_journal.last_err = rc;
        g_journal.dropped++;
        return rc;
    }

    uint8_t *blk = (uint8_t *)g_journal_block;
    for (uint32_t i = 0u; i < SD_BLOCK_SIZE; i++) {
        blk[i] = 0u;
    }

    journal_hdr_t *h = (journal_hdr_t *)blk;
    h->start_magic = JRNL_MAGIC;
    h->rec_seq = g_journal.next_seq;
    h->snap_seq_base = g_journal.last_snapshot_seq;
    h->event_type = event_type;
    h->payload_len = payload_len;
    h->payload_crc = jrnl_checksum_bytes(payload, payload_len);
    h->hdr_crc = jrnl_checksum_words((const uint32_t *)h, JRNL_HDR_WORDS_NO_CRC);

    uint8_t *p = blk + sizeof(journal_hdr_t);
    for (uint32_t i = 0u; i < payload_len; i++) {
        p[i] = payload[i];
    }

    journal_tail_t *t = (journal_tail_t *)(p + payload_len);
    t->end_magic = JEND_MAGIC;
    t->rec_seq = h->rec_seq;
    t->payload_crc = h->payload_crc;
    t->tail_crc = jrnl_checksum_words((const uint32_t *)t, JRNL_TAIL_WORDS_NO_CRC);

    rc = sd_write_blocks(g_journal.next_lba, 1u, g_journal_block);
    if (rc != SD_OK) {
        LOG_ERR("journal", rc);
        g_journal.last_err = rc;
        g_journal.dropped++;
        return rc;
    }

    if (g_journal.next_lba >= (g_journal.start_lba + g_journal.blocks - 1u)) {
        g_journal.next_lba = g_journal.start_lba;
    } else {
        g_journal.next_lba += 1u;
    }
    g_journal.next_seq += 1u;
    g_journal.last_err = SD_OK;
    return SD_OK;
}

static int jrnl_flush_pending_rx(void)
{
    if (g_journal.pending_len == 0u) {
        return SD_OK;
    }

    int rc = jrnl_append_record(JRNL_EVT_RX_BYTES, g_journal.pending_rx, g_journal.pending_len);
    if (rc == SD_OK) {
        g_journal.pending_len = 0u;
    }
    return rc;
}

#include "../../include/snapshot_config.h"

void journal_configure(uint32_t start_lba, uint32_t blocks, uint32_t last_snapshot_seq)
{
    g_journal.configured = 0u;
    g_journal.scan_ready = 0u;
    g_journal.replaying = 0u;
    g_journal.pending_len = 0u;
    g_journal.suppress_rx_bytes = 0u;
    g_journal.last_snapshot_seq = last_snapshot_seq;
    g_journal.start_lba = start_lba;
    g_journal.next_lba = start_lba;
    g_journal.next_seq = 1u;
    g_journal.last_err = SD_OK;
    g_journal.dropped = 0u;

    if (blocks == 0u || blocks > JOURNAL_BLOCKS_MAX) {
        if (blocks > JOURNAL_BLOCKS_MAX) {
            uart_puts("jrnl: blocks insane ");
            uart_put_u32(blocks);
            uart_puts(", disabled\r\n");
        }
        g_journal.blocks = 0u;
        return;
    }

    g_journal.blocks = blocks;

    PANIC_IF(start_lba == 0u, "journal start lba invalid");
    g_journal.configured = 1u;

    /* On a fresh disk (no snapshots ever written) there are no journal
     * entries to find — skip the scan entirely and start from block 0.
     * Otherwise do the scan now, but it will abort on first read failure
     * so a broken card won't stall boot forever. */
    if (last_snapshot_seq == 0u) {
        g_journal.scan_ready = 1u;
    } else {
        jrnl_scan_tail();
    }
}

void journal_note_snapshot_seq(uint32_t snapshot_seq)
{
    (void)jrnl_flush_pending_rx();
    if (snapshot_seq > g_journal.last_snapshot_seq) {
        g_journal.last_snapshot_seq = snapshot_seq;
    }
}

int journal_capture_input_byte(uint8_t c)
{
    if (!g_journal.configured || g_journal.blocks == 0u) {
        return SCHED_ERR_DISABLED;
    }
    if (g_journal.replaying) {
        return SCHED_ERR_DISABLED;
    }

    if (g_journal.suppress_rx_bytes > 0u) {
        g_journal.suppress_rx_bytes--;
        return SD_OK;
    }

    if (g_journal.pending_len >= JRNL_PENDING_RX_CAP) {
        (void)jrnl_flush_pending_rx();
        if (g_journal.pending_len >= JRNL_PENDING_RX_CAP) {
            g_journal.dropped++;
            return SCHED_ERR_FULL;
        }
    }

    g_journal.pending_rx[g_journal.pending_len++] = c;

    /* Flush on line submit so completed commands are persisted without
     * triggering a synchronous SD write on every individual keystroke. */
    if (c == '\r' || c == '\n') {
        return jrnl_flush_pending_rx();
    }
    return SD_OK;
}

int journal_capture_io_owner(uint8_t owner_ao, uint8_t acquired)
{
    if (!g_journal.configured || g_journal.blocks == 0u) {
        return SCHED_ERR_DISABLED;
    }

    (void)jrnl_flush_pending_rx();

    uint8_t payload[2];
    payload[0] = owner_ao;
    payload[1] = (uint8_t)(acquired ? 1u : 0u);
    return jrnl_append_record(JRNL_EVT_IO_OWNER, payload, (uint16_t)sizeof(payload));
}

void journal_replay_from_snapshot(uint32_t snapshot_seq, uint32_t journal_start_lba, uint32_t journal_blocks)
{
    if (journal_start_lba == 0u || journal_blocks == 0u) {
        return;
    }

    uart_puts("jrnl: configure\r\n");
    journal_configure(journal_start_lba, journal_blocks, snapshot_seq);
    if (!g_journal.configured || !g_journal.scan_ready) {
        uart_puts("jrnl: not ready\r\n");
        return;
    }

    uart_puts("jrnl: flush pending\r\n");
    /* Flush pending in-memory records before replay scan (if any). */
    (void)jrnl_flush_pending_rx();

    uint32_t rel_next = g_journal.next_lba - g_journal.start_lba;
    PANIC_IF(rel_next >= g_journal.blocks, "journal next rel out of range");

    uint32_t start_rel = 0u;
    {
        int rc = sd_read_blocks(g_journal.next_lba, 1u, g_journal_block);
        if (rc == SD_OK) {
            journal_hdr_t h;
            if (jrnl_block_validate(g_journal_block, &h, 0, 0)) {
                /* Ring appears full; oldest record starts at write head. */
                start_rel = rel_next;
            }
        }
    }

    g_journal.replaying = 1u;

    uart_puts("jrnl: replay ");
    uart_put_u32(g_journal.blocks);
    uart_puts(" blks\r\n");

    uint32_t have_prev = 0u;
    uint32_t prev_seq = 0u;
    for (uint32_t i = 0u; i < g_journal.blocks; i++) {
        uint32_t rel = (start_rel + i) % g_journal.blocks;
        uint32_t lba = g_journal.start_lba + rel;

        int rc = sd_read_blocks(lba, 1u, g_journal_block);
        if (rc != SD_OK) {
            break;
        }

        journal_hdr_t h;
        const uint8_t *payload = 0;
        if (!jrnl_block_validate(g_journal_block, &h, &payload, 0)) {
            if (i == 0u && start_rel != 0u) {
                /* Not actually full; fallback to sparse/not-full ordering. */
                start_rel = 0u;
                have_prev = 0u;
                prev_seq = 0u;
                i = (uint32_t)-1;
                continue;
            }
            break;
        }

        if (have_prev && !jrnl_seq_newer(h.rec_seq, prev_seq)) {
            break;
        }
        have_prev = 1u;
        prev_seq = h.rec_seq;

        if (h.snap_seq_base < snapshot_seq) {
            continue;
        }

        if (h.event_type == JRNL_EVT_RX_BYTES) {
            int injected = uart_async_inject_rx(payload, (uint16_t)h.payload_len);
            if (injected > 0) {
                g_journal.suppress_rx_bytes += (uint32_t)injected;
            }
            /* Drain the small RX ring buffer through the shell so the
             * next block's inject doesn't overflow it. */
            terminal_replay_drain();
        } else if (h.event_type == JRNL_EVT_IO_OWNER) {
            if (h.payload_len >= 2u) {
                uint8_t owner = payload[0];
                uint8_t acquired = payload[1] & 1u;
                if (acquired) {
                    (void)terminal_stdin_acquire(owner, TERM_STDIN_MODE_RAW);
                } else {
                    (void)terminal_stdin_release(owner);
                }
            }
        }
    }

    uart_puts("jrnl: replay done\r\n");
    g_journal.replaying = 0u;
}
