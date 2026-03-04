#include "../../include/lineio_async.h"
#include "../../include/parse.h"
#include "../../include/uart_async.h"
#include "../../include/uart.h"
#include "../../include/systick.h"
#include "../../include/sd.h"
#include "../../include/sd_async.h"
#include "../../include/sd_task.h"
#include "../../include/counter_task.h"
#include "../../include/console.h"
#include "../../include/log.h"
#include "../../include/cmd_context.h"
#include "../../include/scheduler.h"
#include "../../include/task_spec.h"
#include "../../include/task_ids.h"
#include "../../include/task_signals.h"
#include "../../include/shutdown.h"
#include "../../include/terminal.h"
#include "../../include/panic.h"
#include "../../include/restore_registry.h"
#include "../../include/restore_loader.h"
#include "../../include/restore_sim.h"

#define MAX_ARGUMENTS 8
#define TICKS_PER_SEC 1000u
#define CMD_MAILBOX_CAP 8u
#define TERM_STDIN_OWNER_NONE 0xFFu
#define TERM_SHORTCUT_CTRL_C 0x03
#define SDWRITE_TEST_LBA 2048u
#define CKPT_SD_SLOT0_LBA 2060u
#define CKPT_SD_SLOT1_LBA 2061u
#define CKPT_SD_SLOT_COUNT 2u
#define CKPT_SD_MAX_REGIONS 8u
#define CKPT_SD_MAX_BLOB 384u
#define CKPT_META_MAGIC 0x4154454Du /* "META" */
#define CKPT_META_VERSION 1u

typedef struct {
        shell_state_t shell;
        uint8_t stdin_owner;
        uint8_t stdin_mode;
        const ao_t *stdin_owner_ref;
} terminal_task_ctx_t;

static terminal_task_ctx_t g_term_ctx;
static event_t g_term_queue_storage[8];
static scheduler_t *g_sched;

typedef struct {
        char line[SHELL_LINE_MAX];
        uint8_t used;
} cmd_slot_t;

static event_t g_cmd_queue_storage[8];
static cmd_slot_t g_cmd_slots[CMD_MAILBOX_CAP];
static uint8_t g_cmd_alloc_cursor;
static uint8_t g_sdwrite_test_block[SD_BLOCK_SIZE];
static uint32_t g_ckpt_sd_seq = 1u;
static uint8_t g_ckpt_sd_seq_seeded = 0u;
static uint32_t g_ckpt_auto_interval_ms = 0u;
static uint32_t g_ckpt_auto_interval_ticks = 0u;
static uint32_t g_ckpt_auto_next_tick = 0u;
static uint32_t g_ckpt_running_prev_bitmap = 0u;
static uint16_t g_ckpt_launch_count[SCHED_MAX_AO];
static uint16_t g_ckpt_exit_count[SCHED_MAX_AO];

static void buf_zero(uint8_t *p, uint32_t n)
{
        for (uint32_t i = 0u; i < n; i++) p[i] = 0u;
}

static void buf_copy(uint8_t *dst, const uint8_t *src, uint32_t n)
{
        for (uint32_t i = 0u; i < n; i++) dst[i] = src[i];
}

static void u32_store_le(uint8_t *dst, uint32_t v)
{
        dst[0] = (uint8_t)(v & 0xFFu);
        dst[1] = (uint8_t)((v >> 8) & 0xFFu);
        dst[2] = (uint8_t)((v >> 16) & 0xFFu);
        dst[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t u32_load_le(const uint8_t *src)
{
        return ((uint32_t)src[0])
             | ((uint32_t)src[1] << 8)
             | ((uint32_t)src[2] << 16)
             | ((uint32_t)src[3] << 24);
}

static uint32_t crc32_calc(const uint8_t *data, uint32_t len)
{
        uint32_t crc = 0xFFFFFFFFu;
        for (uint32_t i = 0u; i < len; i++)
        {
                crc ^= (uint32_t)data[i];
                for (uint32_t b = 0u; b < 8u; b++)
                {
                        uint32_t mask = (uint32_t)(-(int32_t)(crc & 1u));
                        crc = (crc >> 1) ^ (0xEDB88320u & mask);
                }
        }
        return ~crc;
}

static uint32_t ckpt_sd_lba_for_slot(uint32_t slot)
{
        return (slot == 0u) ? CKPT_SD_SLOT0_LBA : CKPT_SD_SLOT1_LBA;
}

static int ckpt_sd_read_valid_slot(uint32_t slot,
                                   checkpoint_v2_header_t *out_hdr,
                                   uint8_t *out_block)
{
        uint32_t blk_words[SD_BLOCK_SIZE / 4u];
        uint8_t *blk = (uint8_t *)blk_words;
        checkpoint_v2_header_t hdr;
        uint32_t regions_bytes;
        uint32_t hdr_crc_expected;
        int rc;

        if (slot >= CKPT_SD_SLOT_COUNT || out_hdr == 0 || out_block == 0)
                return 0;

        rc = sd_read_blocks(ckpt_sd_lba_for_slot(slot), 1u, blk_words);
        if (rc != SD_OK)
                return 0;

        buf_copy((uint8_t *)&hdr, blk, (uint32_t)sizeof(hdr));
        if (hdr.magic != CKPT_V2_MAGIC ||
            hdr.format_version != CKPT_V2_FORMAT_VERSION ||
            hdr.state != CKPT_SLOT_STATE_COMMITTED ||
            hdr.region_count == 0u ||
            hdr.region_count > CKPT_V2_MAX_REGIONS ||
            hdr.header_size != sizeof(checkpoint_v2_header_t))
                return 0;

        hdr_crc_expected = hdr.header_crc32;
        hdr.header_crc32 = 0u;
        if (crc32_calc((const uint8_t *)&hdr, (uint32_t)sizeof(hdr)) != hdr_crc_expected)
                return 0;
        hdr.header_crc32 = hdr_crc_expected;

        regions_bytes = (uint32_t)hdr.region_count * (uint32_t)sizeof(checkpoint_v2_region_t);
        if ((uint32_t)sizeof(checkpoint_v2_header_t) + regions_bytes > SD_BLOCK_SIZE)
                return 0;
        if (crc32_calc(blk + sizeof(checkpoint_v2_header_t), regions_bytes) != hdr.regions_crc32)
                return 0;

        for (uint16_t i = 0u; i < hdr.region_count; i++)
        {
                checkpoint_v2_region_t reg;
                uint32_t reg_off = (uint32_t)sizeof(checkpoint_v2_header_t)
                                 + ((uint32_t)i * (uint32_t)sizeof(checkpoint_v2_region_t));
                buf_copy((uint8_t *)&reg, blk + reg_off, (uint32_t)sizeof(reg));
                if (reg.offset + reg.length > SD_BLOCK_SIZE ||
                    reg.offset < ((uint32_t)sizeof(checkpoint_v2_header_t) + regions_bytes))
                        return 0;
                if (crc32_calc(blk + reg.offset, reg.length) != reg.crc32)
                        return 0;
        }

        *out_hdr = hdr;
        buf_copy(out_block, blk, SD_BLOCK_SIZE);
        return 1;
}

static void ckpt_sd_seed_seq_if_needed(void)
{
        checkpoint_v2_header_t hdr;
        uint8_t blk[SD_BLOCK_SIZE];
        uint8_t found = 0u;
        uint32_t max_seq = 0u;

        if (g_ckpt_sd_seq_seeded)
                return;

        for (uint32_t slot = 0u; slot < CKPT_SD_SLOT_COUNT; slot++)
        {
                if (!ckpt_sd_read_valid_slot(slot, &hdr, blk))
                        continue;
                if (!found || hdr.seq > max_seq)
                {
                        found = 1u;
                        max_seq = hdr.seq;
                }
        }

        if (!found)
                return;

        g_ckpt_sd_seq = (max_seq == 0xFFFFFFFFu) ? 1u : (max_seq + 1u);
        g_ckpt_sd_seq_seeded = 1u;
}

void terminal_ckpt_set_interval_ms(uint32_t interval_ms)
{
        if (interval_ms == 0u)
        {
                g_ckpt_auto_interval_ms = 0u;
                g_ckpt_auto_interval_ticks = 0u;
                g_ckpt_auto_next_tick = 0u;
                return;
        }

        uint32_t ticks = ((interval_ms * TICKS_PER_SEC) + 999u) / 1000u;
        if (ticks == 0u)
                ticks = 1u;
        g_ckpt_auto_interval_ms = interval_ms;
        g_ckpt_auto_interval_ticks = ticks;
        g_ckpt_auto_next_tick = systick_now() + ticks;
}

uint32_t terminal_ckpt_get_interval_ms(void)
{
        return g_ckpt_auto_interval_ms;
}

void terminal_task_systick_hook(void)
{
        event_t ev;
        uint32_t now;

        if (g_sched == 0 || g_ckpt_auto_interval_ticks == 0u)
                return;

        now = systick_now();
        if ((int32_t)(now - g_ckpt_auto_next_tick) < 0)
                return;

        g_ckpt_auto_next_tick = now + g_ckpt_auto_interval_ticks;
        ev.sig = TERM_SIG_CKPT_TIMER;
        ev.src = 0u;
        ev.arg0 = 0u;
        ev.arg1 = 0u;
        (void)sched_post_isr(g_sched, AO_TERMINAL, &ev);
}

typedef struct {
        uint8_t task_id;
        uint16_t state_version;
        uint32_t len;
        uint8_t blob[CKPT_SD_MAX_BLOB];
} ckpt_region_item_t;

typedef struct {
        uint8_t task_id;
        uint8_t reserved;
        uint16_t launch_count;
        uint16_t exit_count;
        uint16_t reserved2;
} __attribute__((packed)) ckpt_lifecycle_entry_t;

typedef struct {
        uint32_t magic;
        uint16_t version;
        uint16_t entry_count;
        uint32_t running_bitmap;
        ckpt_lifecycle_entry_t entries[CKPT_SD_MAX_REGIONS];
} __attribute__((packed)) ckpt_lifecycle_meta_t;

static uint32_t ckpt_meta_size(uint16_t entry_count)
{
        return (uint32_t)sizeof(uint32_t)
             + (uint32_t)sizeof(uint16_t)
             + (uint32_t)sizeof(uint16_t)
             + (uint32_t)sizeof(uint32_t)
             + ((uint32_t)entry_count * (uint32_t)sizeof(ckpt_lifecycle_entry_t));
}

static void term_ckpt_preview(void)
{
        uint32_t running_bitmap = 0u;
        uint32_t launched;
        uint32_t exited;
        uint16_t launch_tmp[SCHED_MAX_AO];
        uint16_t exit_tmp[SCHED_MAX_AO];
        uint8_t meta_task_ids[CKPT_SD_MAX_REGIONS];
        uint16_t meta_entry_count = 0u;
        ckpt_region_item_t items[CKPT_SD_MAX_REGIONS];
        uint16_t region_count = 0u;
        uint32_t payload_bytes = 0u;
        uint32_t active_bitmap = 0u;
        uint32_t meta_len;
        uint32_t total_bytes;

        if (g_sched == 0)
        {
                console_puts("ckptpreview: scheduler not ready\r\n");
                return;
        }

        for (uint32_t i = 0u; i < SCHED_MAX_AO; i++)
        {
                launch_tmp[i] = g_ckpt_launch_count[i];
                exit_tmp[i] = g_ckpt_exit_count[i];
                if (g_sched->table[i] != 0)
                        running_bitmap |= (1u << i);
        }

        launched = running_bitmap & ~g_ckpt_running_prev_bitmap;
        exited = g_ckpt_running_prev_bitmap & ~running_bitmap;
        for (uint32_t i = 0u; i < SCHED_MAX_AO; i++)
        {
                if ((launched & (1u << i)) != 0u)
                        launch_tmp[i]++;
                if ((exited & (1u << i)) != 0u)
                        exit_tmp[i]++;
        }

        for (uint32_t id = 0u; id < SCHED_MAX_AO && meta_entry_count < CKPT_SD_MAX_REGIONS; id++)
        {
                const restore_task_descriptor_t *desc = restore_registry_find((uint8_t)id);
                if (desc == 0 || desc->task_class != TASK_CLASS_RESTORABLE_NOW)
                        continue;
                meta_task_ids[meta_entry_count++] = (uint8_t)id;
        }

        meta_len = ckpt_meta_size(meta_entry_count);
        for (uint32_t id = 0u; id < SCHED_MAX_AO && region_count < CKPT_SD_MAX_REGIONS; id++)
        {
                const restore_task_descriptor_t *desc = restore_registry_find((uint8_t)id);
                uint32_t len;
                uint32_t candidate_regions;
                uint32_t candidate_total;
                int rc;

                if (desc == 0 || desc->task_class != TASK_CLASS_RESTORABLE_NOW || desc->get_state_fn == 0)
                        continue;
                if (g_sched->table[id] == 0)
                        continue;
                if (desc->max_state_len > CKPT_SD_MAX_BLOB)
                        continue;

                len = CKPT_SD_MAX_BLOB;
                rc = desc->get_state_fn(items[region_count].blob, &len);
                if (rc != SCHED_OK)
                        continue;
                if (len < desc->min_state_len || len > desc->max_state_len || len > CKPT_SD_MAX_BLOB)
                        continue;

                candidate_regions = (uint32_t)region_count + 1u;
                candidate_total = (uint32_t)sizeof(checkpoint_v2_header_t)
                                + (candidate_regions * (uint32_t)sizeof(checkpoint_v2_region_t))
                                + payload_bytes + len + meta_len;
                if (candidate_total > SD_BLOCK_SIZE)
                        continue;

                items[region_count].task_id = (uint8_t)id;
                items[region_count].state_version = desc->state_version;
                items[region_count].len = len;
                payload_bytes += len;
                active_bitmap |= (1u << id);
                region_count++;
        }

        total_bytes = (uint32_t)sizeof(checkpoint_v2_header_t)
                    + ((uint32_t)region_count * (uint32_t)sizeof(checkpoint_v2_region_t))
                    + payload_bytes + meta_len;

        console_puts("ckptpreview: next_seq=");
        console_put_u32(g_ckpt_sd_seq);
        console_puts(" running_bitmap=0x");
        console_put_hex32(running_bitmap);
        console_puts(" active_bitmap=0x");
        console_put_hex32(active_bitmap);
        console_puts("\r\n");

        console_puts("  regions=");
        console_put_u32(region_count);
        console_puts(" payload=");
        console_put_u32(payload_bytes);
        console_puts(" meta=");
        console_put_u32(meta_len);
        console_puts(" total=");
        console_put_u32(total_bytes);
        console_puts("/");
        console_put_u32(SD_BLOCK_SIZE);
        console_puts(" bytes\r\n");

        for (uint16_t i = 0u; i < region_count; i++)
        {
                uint8_t tid = items[i].task_id;
                console_puts("  region id=");
                console_put_u32(tid);
                console_puts(" ver=");
                console_put_u32(items[i].state_version);
                console_puts(" len=");
                console_put_u32(items[i].len);
                console_puts(" launch=");
                console_put_u32(launch_tmp[tid]);
                console_puts(" exit=");
                console_put_u32(exit_tmp[tid]);
                console_puts(" restore=");
                console_puts((launch_tmp[tid] > exit_tmp[tid]) ? "yes" : "no");
                console_puts("\r\n");
        }

        for (uint16_t i = 0u; i < meta_entry_count; i++)
        {
                uint8_t tid = meta_task_ids[i];
                uint8_t already = 0u;
                for (uint16_t j = 0u; j < region_count; j++)
                {
                        if (items[j].task_id == tid)
                        {
                                already = 1u;
                                break;
                        }
                }
                if (already)
                        continue;
                console_puts("  lifecycle id=");
                console_put_u32(tid);
                console_puts(" launch=");
                console_put_u32(launch_tmp[tid]);
                console_puts(" exit=");
                console_put_u32(exit_tmp[tid]);
                console_puts(" restore=no (not active/no region)\r\n");
        }
}

static int term_ckptsave_sd_once(uint32_t *out_lba, uint32_t *out_slot, uint32_t *out_seq, uint32_t *out_regions)
{
        uint32_t blk_words[SD_BLOCK_SIZE / 4u];
        uint8_t *blk = (uint8_t *)blk_words;
        checkpoint_v2_header_t hdr;
        checkpoint_v2_region_t regs[CKPT_SD_MAX_REGIONS];
        ckpt_region_item_t items[CKPT_SD_MAX_REGIONS];
        ckpt_lifecycle_meta_t meta;
        uint8_t meta_task_ids[CKPT_SD_MAX_REGIONS];
        uint32_t slot_id;
        uint32_t target_lba;
        uint32_t payload_bytes = 0u;
        uint16_t region_count = 0u;
        uint16_t meta_entry_count = 0u;
        uint32_t active_bitmap = 0u;
        uint32_t running_bitmap = 0u;
        uint32_t meta_len = 0u;
        uint32_t meta_off = 0u;
        int rc;
        uint32_t payload_off;

        ckpt_sd_seed_seq_if_needed();
        if (g_sched == 0)
                return SCHED_ERR_PARAM;

        for (uint32_t id = 0u; id < SCHED_MAX_AO; id++)
        {
                if (g_sched->table[id] != 0)
                        running_bitmap |= (1u << id);
        }
        {
                uint32_t launched = running_bitmap & ~g_ckpt_running_prev_bitmap;
                uint32_t exited = g_ckpt_running_prev_bitmap & ~running_bitmap;
                for (uint32_t id = 0u; id < SCHED_MAX_AO; id++)
                {
                        if ((launched & (1u << id)) != 0u)
                                g_ckpt_launch_count[id]++;
                        if ((exited & (1u << id)) != 0u)
                                g_ckpt_exit_count[id]++;
                }
                g_ckpt_running_prev_bitmap = running_bitmap;
        }

        for (uint32_t id = 0u; id < SCHED_MAX_AO && meta_entry_count < CKPT_SD_MAX_REGIONS; id++)
        {
                const restore_task_descriptor_t *desc = restore_registry_find((uint8_t)id);
                if (desc == 0 || desc->task_class != TASK_CLASS_RESTORABLE_NOW)
                        continue;
                meta_task_ids[meta_entry_count++] = (uint8_t)id;
        }
        meta_len = ckpt_meta_size(meta_entry_count);

        for (uint32_t id = 0u; id < SCHED_MAX_AO && region_count < CKPT_SD_MAX_REGIONS; id++)
        {
                const restore_task_descriptor_t *desc = restore_registry_find((uint8_t)id);
                uint32_t len;
                uint32_t candidate_regions;
                uint32_t candidate_total;

                if (desc == 0 || desc->task_class != TASK_CLASS_RESTORABLE_NOW || desc->get_state_fn == 0)
                        continue;
                if (g_sched->table[id] == 0)
                        continue;
                if (desc->max_state_len > CKPT_SD_MAX_BLOB)
                        continue;

                len = CKPT_SD_MAX_BLOB;
                rc = desc->get_state_fn(items[region_count].blob, &len);
                if (rc != SCHED_OK)
                        continue;
                if (len < desc->min_state_len || len > desc->max_state_len)
                        continue;
                if (len > CKPT_SD_MAX_BLOB)
                        continue;

                candidate_regions = (uint32_t)region_count + 1u;
                candidate_total = (uint32_t)sizeof(checkpoint_v2_header_t)
                                + (candidate_regions * (uint32_t)sizeof(checkpoint_v2_region_t))
                                + payload_bytes + len + meta_len;
                if (candidate_total > SD_BLOCK_SIZE)
                        continue;

                items[region_count].task_id = (uint8_t)id;
                items[region_count].state_version = desc->state_version;
                items[region_count].len = len;
                payload_bytes += len;
                active_bitmap |= (1u << id);
                region_count++;
        }

        if ((uint32_t)sizeof(checkpoint_v2_header_t) +
            ((uint32_t)region_count * (uint32_t)sizeof(checkpoint_v2_region_t)) +
            payload_bytes + meta_len > SD_BLOCK_SIZE)
                return SCHED_ERR_FULL;

        buf_zero(blk, SD_BLOCK_SIZE);
        slot_id = (g_ckpt_sd_seq - 1u) % CKPT_SD_SLOT_COUNT;
        target_lba = ckpt_sd_lba_for_slot(slot_id);
        payload_off = (uint32_t)sizeof(checkpoint_v2_header_t)
                    + ((uint32_t)region_count * (uint32_t)sizeof(checkpoint_v2_region_t));
        meta_off = payload_off + payload_bytes;
        hdr.magic = CKPT_V2_MAGIC;
        hdr.format_version = CKPT_V2_FORMAT_VERSION;
        hdr.header_size = (uint16_t)sizeof(checkpoint_v2_header_t);
        hdr.seq = g_ckpt_sd_seq++;
        hdr.tick_at_checkpoint = systick_now();
        hdr.slot_id = (uint8_t)slot_id;
        hdr.state = CKPT_SLOT_STATE_COMMITTED;
        hdr.region_count = region_count;
        hdr.active_task_bitmap = active_bitmap;
        hdr.stdin_owner = TERM_STDIN_OWNER_NONE;
        hdr.reserved0[0] = 0u;
        hdr.reserved0[1] = 0u;
        hdr.reserved0[2] = 0u;
        hdr.regions_crc32 = 0u;
        hdr.header_crc32 = 0u;
        for (uint32_t i = 0u; i < sizeof(hdr.reserved1); i++) hdr.reserved1[i] = 0u;
        u32_store_le(&hdr.reserved1[0], meta_off);
        u32_store_le(&hdr.reserved1[4], meta_len);

        {
                uint32_t cur_off = payload_off;
                for (uint16_t i = 0u; i < region_count; i++)
                {
                        regs[i].region_id = items[i].task_id;
                        regs[i].state_version = items[i].state_version;
                        regs[i].offset = cur_off;
                        regs[i].length = items[i].len;
                        regs[i].crc32 = crc32_calc(items[i].blob, items[i].len);
                        cur_off += items[i].len;
                }
        }
        hdr.regions_crc32 = crc32_calc((const uint8_t *)&regs[0],
                                       (uint32_t)region_count * (uint32_t)sizeof(checkpoint_v2_region_t));
        hdr.header_crc32 = 0u;
        hdr.header_crc32 = crc32_calc((const uint8_t *)&hdr, (uint32_t)sizeof(hdr));

        buf_copy(blk, (const uint8_t *)&hdr, (uint32_t)sizeof(hdr));
        buf_copy(blk + sizeof(hdr), (const uint8_t *)&regs[0],
                 (uint32_t)region_count * (uint32_t)sizeof(checkpoint_v2_region_t));
        for (uint16_t i = 0u; i < region_count; i++)
        {
                buf_copy(blk + regs[i].offset, items[i].blob, items[i].len);
        }
        meta.magic = CKPT_META_MAGIC;
        meta.version = CKPT_META_VERSION;
        meta.entry_count = meta_entry_count;
        meta.running_bitmap = running_bitmap;
        for (uint16_t i = 0u; i < meta_entry_count; i++)
        {
                uint8_t tid = meta_task_ids[i];
                meta.entries[i].task_id = tid;
                meta.entries[i].reserved = 0u;
                meta.entries[i].launch_count = g_ckpt_launch_count[tid];
                meta.entries[i].exit_count = g_ckpt_exit_count[tid];
                meta.entries[i].reserved2 = 0u;
        }
        buf_copy(blk + meta_off, (const uint8_t *)&meta, meta_len);

        rc = sd_write_blocks(target_lba, 1u, blk_words);
        if (rc != SD_OK)
                return rc;

        if (out_lba) *out_lba = target_lba;
        if (out_slot) *out_slot = slot_id;
        if (out_seq) *out_seq = hdr.seq;
        if (out_regions) *out_regions = region_count;
        g_ckpt_sd_seq_seeded = 1u;
        return SCHED_OK;
}

void ui_notify_bg_done(const char *name)
{
        if (name == 0 || name[0] == '\0')
                name = "?";

        console_puts("\r\n[done: ");
        console_puts(name);
        console_puts("]\r\n");
        console_puts(g_term_ctx.shell.prompt_str);
        if (g_term_ctx.shell.len > 0u)
                console_write(g_term_ctx.shell.line, (uint16_t)g_term_ctx.shell.len);
}

static void uart_put_s32(int v)
{
        if (v < 0)
        {
                console_putc('-');
                console_put_u32((uint32_t)(-v));
                return;
        }
        console_put_u32((uint32_t)v);
}

static const char *restore_class_name(uint8_t task_class)
{
        if (task_class == TASK_CLASS_RESTORABLE_NOW) return "restorable";
        if (task_class == TASK_CLASS_RESTART_ONLY) return "restart-only";
        if (task_class == TASK_CLASS_NON_RESTORABLE) return "non-restorable";
        return "unknown";
}

static void sd_print_info(void)
{
        const sd_info_t *info = sd_get_info();
        if (!info->initialized)
        {
                console_puts("sd not initialized\r\n");
                return;
        }
        console_puts("rca=");
        console_put_hex32(info->rca);
        console_puts(" ocr=");
        console_put_hex32(info->ocr);
        console_puts("\r\n");
        console_puts("capacity=");
        console_put_u32(info->capacity_blocks / 2048u);
        console_puts("MB hc=");
        console_put_u32(info->high_capacity);
        console_puts(" bus=");
        console_put_u32(info->bus_width);
        console_puts("bit\r\n");
}

/* ---- ps helpers ---------------------------------------------------------- */

static void ps_str_pad(char *row, uint8_t *pos, uint8_t cap,
                       const char *s, uint8_t width)
{
        uint8_t n = 0;
        while (s && s[n] && *pos < cap) {
                row[(*pos)++] = s[n++];
        }
        while (n < width && *pos < cap) {
                row[(*pos)++] = ' ';
                n++;
        }
}

static void ps_u32_pad(char *row, uint8_t *pos, uint8_t cap,
                       uint32_t v, uint8_t width)
{
        char tmp[10];
        uint8_t n = 0;
        if (v == 0u) {
                tmp[n++] = '0';
        } else {
                while (v > 0u && n < (uint8_t)sizeof(tmp)) {
                        tmp[n++] = (char)('0' + (v % 10u));
                        v /= 10u;
                }
                for (uint8_t i = 0; i < n / 2u; i++) {
                        char t = tmp[i];
                        tmp[i] = tmp[n - 1u - i];
                        tmp[n - 1u - i] = t;
                }
        }
        for (uint8_t i = 0; i < n && *pos < cap; i++) {
                row[(*pos)++] = tmp[i];
        }
        while (n < width && *pos < cap) {
                row[(*pos)++] = ' ';
                n++;
        }
}

/* ---- cmd slot management ------------------------------------------------- */

static int cmd_slot_alloc_copy(const char *line)
{
        for (uint32_t i = 0; i < CMD_MAILBOX_CAP; i++)
        {
                uint8_t idx = (uint8_t)((g_cmd_alloc_cursor + i) % CMD_MAILBOX_CAP);
                if (g_cmd_slots[idx].used)
                        continue;

                uint32_t j = 0;
                while (j < (SHELL_LINE_MAX - 1u) && line[j] != '\0')
                {
                        g_cmd_slots[idx].line[j] = line[j];
                        j++;
                }
                g_cmd_slots[idx].line[j] = '\0';
                g_cmd_slots[idx].used = 1u;
                g_cmd_alloc_cursor = (uint8_t)((idx + 1u) % CMD_MAILBOX_CAP);
                return (int)idx;
        }
        return -1;
}

static void cmd_slot_release(uint8_t idx)
{
        PANIC_IF(idx >= CMD_MAILBOX_CAP, "cmd slot idx out of range");
        g_cmd_slots[idx].used = 0u;
}

static int term_is_protected_task(uint8_t ao_id)
{
        return (ao_id == AO_TERMINAL || ao_id == AO_CMD || ao_id == AO_CONSOLE);
}

static int term_stdin_owner_valid(void)
{
        if (g_sched == 0)
                return 0;
        if (g_term_ctx.stdin_owner == TERM_STDIN_OWNER_NONE)
                return 0;
        if (g_term_ctx.stdin_owner >= SCHED_MAX_AO)
                return 0;
        if (g_sched->table[g_term_ctx.stdin_owner] == 0)
                return 0;
        if (g_term_ctx.stdin_owner_ref == 0)
                return 0;
        return g_sched->table[g_term_ctx.stdin_owner] == g_term_ctx.stdin_owner_ref;
}

static void term_stdin_release_internal(void)
{
        g_term_ctx.stdin_owner = TERM_STDIN_OWNER_NONE;
        g_term_ctx.stdin_mode = 0u;
        g_term_ctx.stdin_owner_ref = 0;
}

static int term_kill_task(uint8_t ao_id, const char **out_name)
{
        if (g_sched == 0 || ao_id >= SCHED_MAX_AO)
                return SCHED_ERR_PARAM;
        if (term_is_protected_task(ao_id))
                return SCHED_ERR_DISABLED;
        if (g_sched->table[ao_id] == 0)
                return SCHED_ERR_NOT_FOUND;

        const char *name = g_sched->table[ao_id]->name;
        int rc = sched_unregister(g_sched, ao_id);
        if (rc == SCHED_OK && g_term_ctx.stdin_owner == ao_id)
        {
                term_stdin_release_internal();
        }

        if (out_name)
                *out_name = name;
        return rc;
}

static int term_find_task_id(const char *selector, uint8_t *out_id)
{
        uint32_t id = 0u;
        if (parse_u32(selector, &id))
        {
                if (id >= SCHED_MAX_AO)
                        return 0;
                if (g_sched->table[id] == 0)
                        return 0;
                *out_id = (uint8_t)id;
                return 1;
        }

        for (uint8_t i = 0u; i < SCHED_MAX_AO; i++)
        {
                const ao_t *ao = g_sched->table[i];
                if (ao == 0 || ao->name == 0)
                        continue;
                if (streq(ao->name, selector))
                {
                        *out_id = i;
                        return 1;
                }
        }

        return 0;
}

int terminal_stdin_acquire(uint8_t owner_ao, uint8_t mode)
{
        if (g_sched == 0 || owner_ao >= SCHED_MAX_AO)
                return SCHED_ERR_PARAM;
        if (mode != TERM_STDIN_MODE_RAW)
                return SCHED_ERR_PARAM;
        if (g_sched->table[owner_ao] == 0)
                return SCHED_ERR_NOT_FOUND;
        if (term_is_protected_task(owner_ao))
                return SCHED_ERR_DISABLED;
        if (g_term_ctx.stdin_owner != TERM_STDIN_OWNER_NONE)
                return SCHED_ERR_EXISTS;

        g_term_ctx.stdin_owner = owner_ao;
        g_term_ctx.stdin_mode = mode;
        g_term_ctx.stdin_owner_ref = g_sched->table[owner_ao];
        shell_rx_idle(&g_term_ctx.shell);
        return SCHED_OK;
}

int terminal_stdin_release(uint8_t owner_ao)
{
        if (g_sched == 0 || owner_ao >= SCHED_MAX_AO)
                return SCHED_ERR_PARAM;
        if (g_term_ctx.stdin_owner == TERM_STDIN_OWNER_NONE)
                return SCHED_ERR_NOT_FOUND;
        if (g_term_ctx.stdin_owner != owner_ao)
                return SCHED_ERR_PARAM;
        if (!term_stdin_owner_valid())
                return SCHED_ERR_NOT_FOUND;

        term_stdin_release_internal();
        shell_rx_idle(&g_term_ctx.shell);
        (void)sched_post(g_sched, AO_TERMINAL, &(event_t){ .sig = TERM_SIG_REPRINT_PROMPT });
        return SCHED_OK;
}

/* ---- command executor ---------------------------------------------------- */

static void term_execute(char *line)
{
        char *argv[MAX_ARGUMENTS];
        int argc = tokenize(line, argv, MAX_ARGUMENTS);

        if (argc == 0)
                return;

        if (streq(argv[0], "help"))
        {
                console_puts("\r\n");
                console_puts("  System\r\n");
                console_puts("    reboot            Reboot system\r\n");
                console_puts("    shutdown          Enter low-power standby\r\n");
                console_puts("    uptime            Show uptime\r\n");
                console_puts("    ticks             Show tick count\r\n");
                console_puts("    ps                Show active tasks\r\n");
                console_puts("    kill <task>       Remove task by id or name\r\n");
                console_puts("    counter [n]       Run simple counter program\r\n");
                console_puts("    Ctrl-C            Kill foreground input owner\r\n");
                console_puts("\r\n");
                console_puts("  SD Card\r\n");
                console_puts("    sdinit            Initialize SD card\r\n");
                console_puts("    sdinfo            Show card info\r\n");
                console_puts("    sdtest            Init + read test\r\n");
                console_puts("    sdread <lba> [n]  Read blocks (n<=4)\r\n");
                console_puts("    sdaread <lba>     Async read one block\r\n");
                console_puts("    sddetect          Check card presence\r\n");
                console_puts("    sdwrite0          Write test block to fixed LBA\r\n");
                console_puts("    sdread0cmp        Read+verify fixed test block\r\n");
                console_puts("    ckptsave_sd       Save counter checkpoint to SD A/B slot\r\n");
                console_puts("    ckptload_sd       Load latest valid counter checkpoint from SD A/B slot\r\n");
                console_puts("    ckptinspect       Inspect latest valid SD checkpoint metadata\r\n");
                console_puts("    ckptpreview       Dry-run what next SD checkpoint would contain\r\n");
                console_puts("    ckptcorrupt <slot> Corrupt SD checkpoint slot 0 or 1 (debug)\r\n");
                console_puts("    autockpt <ms|off> Configure periodic SD checkpoint save\r\n");
                console_puts("\r\n");
                console_puts("  Debug\r\n");
                console_puts("    md <addr> [n]     Memory dump\r\n");
                console_puts("    echo <text>       Echo text\r\n");
                console_puts("    logcat            Drain background log\r\n");
                console_puts("    sdmmcdump         Dump SDMMC registers\r\n");
                console_puts("    sderror           Show SD error details\r\n");
                console_puts("    sdreset           Reset SD hardware state\r\n");
                console_puts("    restorestat       Show restore loader/registry stats\r\n");
                console_puts("    ckptsave          Save counter state to in-memory ckpt queue\r\n");
                console_puts("    ckptsaveop <mul> <div>  Save counter state+ops to in-memory ckpt queue\r\n");
                console_puts("    ckptload          Load counter state from in-memory ckpt queue\r\n");
                console_puts("    ckptq             Show in-memory ckpt queue stats\r\n");
                console_puts("    restoresim [limit] [value] [bg]  Simulate in-memory restore blob\r\n");
                console_puts("    restoresimop <limit> <value> <mul> <div> [bg]  Simulate ordered ops restore\r\n");
                console_puts("    restoresimbad [ver|action|div0|len]  Simulate invalid restore blob\r\n");
                console_puts("\r\n");
                return;
        }

        if (streq(argv[0], "echo"))
        {
                for (int i = 1; i < argc; i++)
                {
                        console_puts(argv[i]);
                        if (i + 1 < argc)
                                console_putc(' ');
                }
                console_puts("\r\n");
                return;
        }

        if (streq(argv[0], "reboot"))
        {
                /* Drain async TX so all pending console output reaches the
                 * terminal, then send the final message via synchronous UART
                 * which is guaranteed to complete before the reset fires.   */
                while (!uart_tx_done()) {
                        __asm__ volatile("wfi");
                }
                uart_puts("rebooting...\r\n");
                uart_flush_tx();

                volatile uint32_t *AIRCR = (uint32_t *)0xE000ED0Cu;
                const uint32_t VECTKEY = 0x5FAu << 16;
                *AIRCR = VECTKEY | (1u << 2); /* SYSRESETREQ */

                for (;;) {}
        }

        if (streq(argv[0], "shutdown"))
        {
                /* Drain async TX so all pending console output reaches the
                 * terminal, then send the final message via synchronous UART
                 * before handing off to shutdown_now() for Standby entry.  */
                while (!uart_tx_done()) {
                        __asm__ volatile("wfi");
                }
                uart_puts("halted.\r\n");
                shutdown_now();
                /* unreachable */
                for (;;) {}
        }

        if (streq(argv[0], "ticks"))
        {
                uint32_t t = systick_now();
                console_puts("ticks=");
                console_put_u32(t);
                console_puts("\r\n");
                return;
        }

        if (streq(argv[0], "uptime"))
        {
                uint32_t t = systick_now();
                uint32_t ms = (t * 1000u) / TICKS_PER_SEC;
                console_puts("uptime_ms=");
                console_put_u32(ms);
                console_puts("\r\n");
                return;
        }

        if (streq(argv[0], "ps"))
        {
                PANIC_IF(g_sched == 0, "ps: scheduler not bound");
                console_puts("NAME        ID  PRIO  QLEN  QCAP  HANDLED  DROPPED\r\n");
                for (uint32_t i = 0; i < SCHED_MAX_AO; i++)
                {
                        const ao_t *ao = g_sched->table[i];
                        if (ao == 0)
                                continue;
                        char row[80];
                        uint8_t pos = 0u;
                        const uint8_t cap = (uint8_t)(sizeof(row) - 3u);
                        ps_str_pad(row, &pos, cap, ao->name ? ao->name : "?", 12u);
                        ps_u32_pad(row, &pos, cap, ao->id, 4u);
                        ps_u32_pad(row, &pos, cap, ao->prio, 6u);
                        ps_u32_pad(row, &pos, cap, ao->q.count, 6u);
                        ps_u32_pad(row, &pos, cap, ao->q.cap, 6u);
                        ps_u32_pad(row, &pos, cap, ao->events_handled, 9u);
                        ps_u32_pad(row, &pos, cap, ao->q.dropped, 8u);
                        row[pos++] = '\r';
                        row[pos++] = '\n';
                        row[pos]   = '\0';
                        console_puts(row);
                }
                return;
        }

        if (streq(argv[0], "kill"))
        {
                PANIC_IF(g_sched == 0, "kill: scheduler not bound");
                if (argc < 2)
                {
                        console_puts("usage: kill <task-id|name>\r\n");
                        return;
                }

                uint8_t ao_id = 0u;
                if (!term_find_task_id(argv[1], &ao_id))
                {
                        console_puts("kill: task not found\r\n");
                        return;
                }

                const char *name = 0;
                int rc = term_kill_task(ao_id, &name);
                if (rc != SCHED_OK)
                {
                        if (rc == SCHED_ERR_DISABLED)
                        {
                                console_puts("kill: protected task\r\n");
                                return;
                        }
                        console_puts("kill: err=");
                        uart_put_s32(rc);
                        console_puts("\r\n");
                        return;
                }

                console_puts("killed ");
                if (name && name[0] != '\0')
                        console_puts(name);
                else
                        console_puts("(unnamed)");
                console_puts(" id=");
                console_put_u32((uint32_t)ao_id);
                console_puts("\r\n");
                return;
        }

        if (streq(argv[0], "counter"))
        {
                uint32_t limit = 10u;
                if (argc >= 2 && !parse_u32(argv[1], &limit))
                {
                        console_puts("counter: bad n\r\n");
                        return;
                }

                int rc = counter_task_register(g_sched);
                if (rc != SCHED_OK)
                {
                        console_puts("counter: start err=");
                        uart_put_s32(rc);
                        console_puts("\r\n");
                        return;
                }

                rc = counter_task_request_start(limit);
                if (rc != SCHED_OK)
                {
                        console_puts("counter: queue err=");
                        uart_put_s32(rc);
                        console_puts("\r\n");
                }
                return;
        }

        if (streq(argv[0], "md"))
        {
                if (argc < 2)
                {
                        console_puts("usage: md <addr> [n]\r\n");
                        return;
                }

                uint32_t addr, n = 1;
                if (!parse_u32(argv[1], &addr))
                {
                        console_puts("md: bad addr\r\n");
                        return;
                }
                if (argc >= 3 && !parse_u32(argv[2], &n))
                {
                        console_puts("md: bad n\r\n");
                        return;
                }
                if (n == 0)
                        n = 1;
                if (n > 64)
                        n = 64;

                volatile uint32_t *p = (volatile uint32_t *)addr;
                for (uint32_t i = 0; i < n; i++)
                {
                        console_put_hex32((uint32_t)(addr + i * 4u));
                        console_puts(": ");
                        console_put_hex32(p[i]);
                        console_puts("\r\n");
                }
                return;
        }

        if (streq(argv[0], "sdinit"))
        {
                sd_detect_init();
                if (!sd_is_detected())
                {
                        console_puts("sd: not present\r\n");
                        return;
                }
                sd_use_pll48(1);
                sd_set_data_clkdiv(SD_CLKDIV_FAST);
                int rc = sd_init();
                if (rc == SD_OK)
                {
                        sd_async_init();
                        g_ckpt_sd_seq_seeded = 0u;
                        console_puts("sdinit: ok\r\n");
                        return;
                }
                sd_async_init();
                console_puts("sdinit: err=");
                uart_put_s32(rc);
                console_puts("\r\n");
                return;
        }

        if (streq(argv[0], "sdinfo"))
        {
                sd_print_info();
                return;
        }

        if (streq(argv[0], "sddetect"))
        {
                sd_detect_init();
                if (sd_is_detected())
                        console_puts("sd: present\r\n");
                else
                        console_puts("sd: not present\r\n");
                return;
        }

        if (streq(argv[0], "sdtest"))
        {
                int rc = sd_task_request_test();
                if (rc != SCHED_OK)
                {
                        console_puts("sdtest: queue err=");
                        uart_put_s32(rc);
                        console_puts("\r\n");
                }
                return;
        }

        if (streq(argv[0], "sdread"))
        {
                if (argc < 2)
                {
                        console_puts("usage: sdread <lba> [count]\r\n");
                        return;
                }
                uint32_t lba = 0;
                uint32_t count = 1;
                if (!parse_u32(argv[1], &lba))
                {
                        console_puts("sdread: bad lba\r\n");
                        return;
                }
                if (argc >= 3 && !parse_u32(argv[2], &count))
                {
                        console_puts("sdread: bad count\r\n");
                        return;
                }
                if (count == 0)
                        count = 1;
                int rc = sd_task_request_read_dump(lba, count);
                if (rc != SCHED_OK)
                {
                        console_puts("sdread: queue err=");
                        uart_put_s32(rc);
                        console_puts("\r\n");
                }
                return;
        }

        if (streq(argv[0], "sdaread"))
        {
                if (argc < 2)
                {
                        console_puts("usage: sdaread <lba>\r\n");
                        return;
                }
                uint32_t lba = 0;
                if (!parse_u32(argv[1], &lba))
                {
                        console_puts("sdaread: bad lba\r\n");
                        return;
                }
                int rc = sd_task_request_read_dump(lba, 1);
                if (rc != SCHED_OK)
                {
                        console_puts("sdaread: queue err=");
                        uart_put_s32(rc);
                        console_puts("\r\n");
                }
                return;
        }

        if (streq(argv[0], "sdwrite0"))
        {
                int rc;
                for (uint32_t i = 0u; i < SD_BLOCK_SIZE; i++)
                        g_sdwrite_test_block[i] = (uint8_t)((i * 13u + 7u) & 0xFFu);
                g_sdwrite_test_block[0] = 'R';
                g_sdwrite_test_block[1] = 'W';
                g_sdwrite_test_block[2] = 'O';
                g_sdwrite_test_block[3] = 'S';
                g_sdwrite_test_block[4] = 'T';
                g_sdwrite_test_block[5] = 'E';
                g_sdwrite_test_block[6] = 'S';
                g_sdwrite_test_block[7] = 'T';

                rc = sd_write_blocks(SDWRITE_TEST_LBA, 1u, g_sdwrite_test_block);
                if (rc != SD_OK)
                {
                        console_puts("sdwrite0: err=");
                        uart_put_s32(rc);
                        console_puts(" stage=");
                        console_put_u32(sd_dbg_write_stage());
                        console_puts(" sta=0x");
                        console_put_hex32(sd_dbg_write_last_sta());
                        console_puts("\r\n");
                        return;
                }
                console_puts("sdwrite0: ok lba=");
                console_put_u32(SDWRITE_TEST_LBA);
                console_puts("\r\n");
                return;
        }

        if (streq(argv[0], "sdread0cmp"))
        {
                uint8_t rd[SD_BLOCK_SIZE];
                int rc = sd_read_blocks(SDWRITE_TEST_LBA, 1u, rd);
                if (rc != SD_OK)
                {
                        console_puts("sdread0cmp: read err=");
                        uart_put_s32(rc);
                        console_puts("\r\n");
                        return;
                }
                for (uint32_t i = 0u; i < SD_BLOCK_SIZE; i++)
                {
                        uint8_t exp = (uint8_t)((i * 13u + 7u) & 0xFFu);
                        if (i == 0u) exp = 'R';
                        if (i == 1u) exp = 'W';
                        if (i == 2u) exp = 'O';
                        if (i == 3u) exp = 'S';
                        if (i == 4u) exp = 'T';
                        if (i == 5u) exp = 'E';
                        if (i == 6u) exp = 'S';
                        if (i == 7u) exp = 'T';
                        if (rd[i] != exp)
                        {
                                console_puts("sdread0cmp: mismatch at ");
                                console_put_u32(i);
                                console_puts(" got=0x");
                                console_put_hex8(rd[i]);
                                console_puts(" exp=0x");
                                console_put_hex8(exp);
                                console_puts("\r\n");
                                return;
                        }
                }
                console_puts("sdread0cmp: ok lba=");
                console_put_u32(SDWRITE_TEST_LBA);
                console_puts("\r\n");
                return;
        }

        if (streq(argv[0], "ckptsave_sd"))
        {
                uint32_t lba = 0u, slot = 0u, seq = 0u, regions = 0u;
                int rc = term_ckptsave_sd_once(&lba, &slot, &seq, &regions);
                if (rc != SCHED_OK)
                {
                        console_puts("ckptsave_sd: err=");
                        uart_put_s32(rc);
                        console_puts("\r\n");
                        return;
                }
                console_puts("ckptsave_sd: ok lba=");
                console_put_u32(lba);
                console_puts(" slot=");
                console_put_u32(slot);
                console_puts(" seq=");
                console_put_u32(seq);
                console_puts(" regions=");
                console_put_u32(regions);
                console_puts("\r\n");
                return;
        }

        if (streq(argv[0], "autockpt"))
        {
                uint32_t ms = 0u;
                if (argc < 2)
                {
                        console_puts("autockpt: ");
                        console_put_u32(terminal_ckpt_get_interval_ms());
                        console_puts(" ms\r\n");
                        return;
                }
                if (streq(argv[1], "off") || streq(argv[1], "0"))
                {
                        terminal_ckpt_set_interval_ms(0u);
                        console_puts("autockpt: off\r\n");
                        return;
                }
                if (!parse_u32(argv[1], &ms) || ms == 0u)
                {
                        console_puts("usage: autockpt <ms|off>\r\n");
                        return;
                }
                terminal_ckpt_set_interval_ms(ms);
                console_puts("autockpt: on every ");
                console_put_u32(terminal_ckpt_get_interval_ms());
                console_puts(" ms\r\n");
                return;
        }

        if (streq(argv[0], "ckptload_sd"))
        {
                uint32_t blk_words[SD_BLOCK_SIZE / 4u];
                uint8_t *blk = (uint8_t *)blk_words;
                uint8_t selected_blk[SD_BLOCK_SIZE];
                checkpoint_v2_header_t best_hdr;
                checkpoint_v2_region_t filtered_regions[CKPT_V2_MAX_REGIONS];
                uint8_t best_found = 0u;
                uint32_t applied = 0u, skipped = 0u, failed = 0u;
                uint32_t selected_lba = 0u;
                int rc = RESTORE_LOADER_ERR_RESTORE;

                ckpt_sd_seed_seq_if_needed();

                for (uint32_t slot = 0u; slot < CKPT_SD_SLOT_COUNT; slot++)
                {
                        checkpoint_v2_header_t hdr;
                        if (!ckpt_sd_read_valid_slot(slot, &hdr, blk))
                                continue;

                        if (!best_found || hdr.seq > best_hdr.seq)
                        {
                                best_found = 1u;
                                best_hdr = hdr;
                                selected_lba = ckpt_sd_lba_for_slot(slot);
                                buf_copy(selected_blk, blk, SD_BLOCK_SIZE);
                        }
                }

                if (!best_found)
                {
                        console_puts("ckptload_sd: no valid slot\r\n");
                        return;
                }

                {
                        const checkpoint_v2_region_t *all_regions =
                                (const checkpoint_v2_region_t *)(selected_blk + sizeof(checkpoint_v2_header_t));
                        uint16_t filtered_count = 0u;
                        uint32_t meta_off = u32_load_le(&best_hdr.reserved1[0]);
                        uint32_t meta_len = u32_load_le(&best_hdr.reserved1[4]);
                        const ckpt_lifecycle_meta_t *meta = 0;

                        if (meta_len >= ckpt_meta_size(0u) && meta_off + meta_len <= SD_BLOCK_SIZE)
                        {
                                meta = (const ckpt_lifecycle_meta_t *)(selected_blk + meta_off);
                                if (meta->magic != CKPT_META_MAGIC || meta->version != CKPT_META_VERSION)
                                        meta = 0;
                                else if (meta->entry_count > best_hdr.region_count)
                                        meta = 0;
                        }

                        for (uint16_t i = 0u; i < best_hdr.region_count; i++)
                        {
                                const checkpoint_v2_region_t *r = &all_regions[i];
                                uint8_t allow = 1u;
                                if (meta != 0)
                                {
                                        for (uint16_t j = 0u; j < meta->entry_count; j++)
                                        {
                                                if (meta->entries[j].task_id != (uint8_t)r->region_id)
                                                        continue;
                                                if (meta->entries[j].launch_count <= meta->entries[j].exit_count)
                                                        allow = 0u;
                                                break;
                                        }
                                }
                                if (!allow)
                                        continue;
                                filtered_regions[filtered_count++] = *r;
                        }

                rc = restore_loader_apply_regions(g_sched,
                                                  filtered_regions,
                                                  filtered_count,
                                                  selected_blk, SD_BLOCK_SIZE,
                                                  &applied, &skipped, &failed);
                }
                console_puts("ckptload_sd: rc=");
                uart_put_s32(rc);
                console_puts(" applied=");
                console_put_u32(applied);
                console_puts(" skipped=");
                console_put_u32(skipped);
                console_puts(" failed=");
                console_put_u32(failed);
                console_puts(" lba=");
                console_put_u32(selected_lba);
                console_puts(" slot=");
                console_put_u32((uint32_t)best_hdr.slot_id);
                console_puts(" seq=");
                console_put_u32(best_hdr.seq);
                console_puts("\r\n");
                g_ckpt_sd_seq = (best_hdr.seq == 0xFFFFFFFFu) ? 1u : (best_hdr.seq + 1u);
                g_ckpt_sd_seq_seeded = 1u;
                return;
        }

        if (streq(argv[0], "ckptinspect"))
        {
                uint32_t blk_words[SD_BLOCK_SIZE / 4u];
                uint8_t *blk = (uint8_t *)blk_words;
                uint8_t selected_blk[SD_BLOCK_SIZE];
                checkpoint_v2_header_t best_hdr;
                uint8_t best_found = 0u;
                uint32_t selected_lba = 0u;

                for (uint32_t slot = 0u; slot < CKPT_SD_SLOT_COUNT; slot++)
                {
                        checkpoint_v2_header_t hdr;
                        if (!ckpt_sd_read_valid_slot(slot, &hdr, blk))
                                continue;
                        if (!best_found || hdr.seq > best_hdr.seq)
                        {
                                best_found = 1u;
                                best_hdr = hdr;
                                selected_lba = ckpt_sd_lba_for_slot(slot);
                                buf_copy(selected_blk, blk, SD_BLOCK_SIZE);
                        }
                }

                if (!best_found)
                {
                        console_puts("ckptinspect: no valid slot\r\n");
                        return;
                }

                console_puts("ckptinspect: lba=");
                console_put_u32(selected_lba);
                console_puts(" slot=");
                console_put_u32((uint32_t)best_hdr.slot_id);
                console_puts(" seq=");
                console_put_u32(best_hdr.seq);
                console_puts(" regions=");
                console_put_u32(best_hdr.region_count);
                console_puts(" active_bitmap=0x");
                console_put_hex32(best_hdr.active_task_bitmap);
                console_puts("\r\n");

                {
                        const checkpoint_v2_region_t *regions =
                                (const checkpoint_v2_region_t *)(selected_blk + sizeof(checkpoint_v2_header_t));
                        uint32_t meta_off = u32_load_le(&best_hdr.reserved1[0]);
                        uint32_t meta_len = u32_load_le(&best_hdr.reserved1[4]);
                        const ckpt_lifecycle_meta_t *meta = 0;

                        if (meta_len >= ckpt_meta_size(0u) && meta_off + meta_len <= SD_BLOCK_SIZE)
                        {
                                meta = (const ckpt_lifecycle_meta_t *)(selected_blk + meta_off);
                                if (meta->magic != CKPT_META_MAGIC || meta->version != CKPT_META_VERSION)
                                        meta = 0;
                                else if (meta->entry_count > best_hdr.region_count)
                                        meta = 0;
                        }

                        if (meta == 0)
                        {
                                console_puts("  meta: none\r\n");
                        }
                        else
                        {
                                console_puts("  meta: running_bitmap=0x");
                                console_put_hex32(meta->running_bitmap);
                                console_puts(" entries=");
                                console_put_u32(meta->entry_count);
                                console_puts("\r\n");
                        }

                        for (uint16_t i = 0u; i < best_hdr.region_count; i++)
                        {
                                const checkpoint_v2_region_t *r = &regions[i];
                                uint32_t launch = 0u;
                                uint32_t exit = 0u;
                                uint8_t has_life = 0u;
                                uint8_t would_restore = 1u;

                                if (meta != 0)
                                {
                                        for (uint16_t j = 0u; j < meta->entry_count; j++)
                                        {
                                                if (meta->entries[j].task_id != (uint8_t)r->region_id)
                                                        continue;
                                                launch = meta->entries[j].launch_count;
                                                exit = meta->entries[j].exit_count;
                                                has_life = 1u;
                                                if (launch <= exit)
                                                        would_restore = 0u;
                                                break;
                                        }
                                }

                                console_puts("  region id=");
                                console_put_u32(r->region_id);
                                console_puts(" ver=");
                                console_put_u32(r->state_version);
                                console_puts(" len=");
                                console_put_u32(r->length);
                                if (has_life)
                                {
                                        console_puts(" launch=");
                                        console_put_u32(launch);
                                        console_puts(" exit=");
                                        console_put_u32(exit);
                                        console_puts(" restore=");
                                        console_puts(would_restore ? "yes" : "no");
                                }
                                console_puts("\r\n");
                        }
                }
                return;
        }

        if (streq(argv[0], "ckptpreview"))
        {
                term_ckpt_preview();
                return;
        }

        if (streq(argv[0], "ckptcorrupt"))
        {
                uint32_t slot = 0u;
                uint32_t lba;
                uint32_t blk_words[SD_BLOCK_SIZE / 4u];
                uint8_t *blk = (uint8_t *)blk_words;
                uint32_t off = (uint32_t)(sizeof(checkpoint_v2_header_t) + sizeof(checkpoint_v2_region_t));
                int rc;

                if (argc < 2 || !parse_u32(argv[1], &slot) || slot >= CKPT_SD_SLOT_COUNT)
                {
                        console_puts("usage: ckptcorrupt <slot:0|1>\r\n");
                        return;
                }

                lba = ckpt_sd_lba_for_slot(slot);
                rc = sd_read_blocks(lba, 1u, blk_words);
                if (rc != SD_OK)
                {
                        console_puts("ckptcorrupt: read err=");
                        uart_put_s32(rc);
                        console_puts(" lba=");
                        console_put_u32(lba);
                        console_puts("\r\n");
                        return;
                }

                if (off >= SD_BLOCK_SIZE)
                {
                        console_puts("ckptcorrupt: bad offset\r\n");
                        return;
                }
                blk[off] ^= 0xA5u;

                rc = sd_write_blocks(lba, 1u, blk_words);
                if (rc != SD_OK)
                {
                        console_puts("ckptcorrupt: write err=");
                        uart_put_s32(rc);
                        console_puts(" stage=");
                        console_put_u32(sd_dbg_write_stage());
                        console_puts(" sta=0x");
                        console_put_hex32(sd_dbg_write_last_sta());
                        console_puts("\r\n");
                        return;
                }

                console_puts("ckptcorrupt: ok slot=");
                console_put_u32(slot);
                console_puts(" lba=");
                console_put_u32(lba);
                console_puts(" off=");
                console_put_u32(off);
                console_puts("\r\n");
                return;
        }

        if (streq(argv[0], "logcat"))
        {
                char buf[64];
                uint16_t n;
                uint8_t any = 0u;
                while ((n = log_read(buf, (uint16_t)sizeof(buf))) > 0u)
                {
                        console_write(buf, n);
                        any = 1u;
                }
                if (!any)
                        console_puts("(log empty)\r\n");
                return;
        }

        if (streq(argv[0], "sdmmcdump"))
        {
                volatile uint32_t *sdmmc = (volatile uint32_t *)0x40012C00;
                console_puts("SDMMC Registers:\r\n");
                console_puts("POWER=0x"); console_put_hex32(sdmmc[0x00/4]); console_puts("\r\n");
                console_puts("CLKCR=0x"); console_put_hex32(sdmmc[0x04/4]); console_puts("\r\n");
                console_puts("DCTRL=0x"); console_put_hex32(sdmmc[0x2C/4]); console_puts("\r\n");
                console_puts("DLEN=0x");  console_put_hex32(sdmmc[0x28/4]); console_puts("\r\n");
                console_puts("DTIMER=0x"); console_put_hex32(sdmmc[0x24/4]); console_puts("\r\n");
                console_puts("STA=0x");   console_put_hex32(sdmmc[0x34/4]); console_puts("\r\n");
                console_puts("MASK=0x");  console_put_hex32(sdmmc[0x3C/4]); console_puts("\r\n");
                console_puts("FIFOCNT=0x"); console_put_hex32(sdmmc[0x48/4]); console_puts("\r\n");
                return;
        }

        if (streq(argv[0], "sderror"))
        {
                extern sd_context_t g_sd_ctx;
                console_puts("SD Context:\r\n");
                console_puts("error_code="); uart_put_s32(g_sd_ctx.error_code); console_puts("\r\n");
                console_puts("error_detail=0x"); console_put_hex32(g_sd_ctx.error_detail); console_puts("\r\n");
                console_puts("status="); console_put_u32(g_sd_ctx.status); console_puts("\r\n");
                console_puts("operation="); console_put_u32(g_sd_ctx.operation); console_puts("\r\n");
                if (g_sd_ctx.error_detail) {
                        console_puts("Flags: ");
                        if (g_sd_ctx.error_detail & (1<<1)) console_puts("DCRCFAIL ");
                        if (g_sd_ctx.error_detail & (1<<3)) console_puts("DTIMEOUT ");
                        if (g_sd_ctx.error_detail & (1<<5)) console_puts("RXOVERR ");
                        if (g_sd_ctx.error_detail & (1<<8)) console_puts("DATAEND ");
                        if (g_sd_ctx.error_detail & (1<<21)) console_puts("RXDAVL ");
                        console_puts("\r\n");
                }
                return;
        }

        if (streq(argv[0], "sdreset"))
        {
                extern void sd_async_init(void);
                sd_async_init();
                console_puts("SD hardware state reset\r\n");
                return;
        }

        if (streq(argv[0], "restorestat"))
        {
                restore_loader_stats_t st;
                restore_loader_get_stats(&st);
                console_puts("restore: calls=");
                console_put_u32(st.calls);
                console_puts(" applied=");
                console_put_u32(st.applied);
                console_puts(" skipped=");
                console_put_u32(st.skipped);
                console_puts(" failed=");
                console_put_u32(st.failed);
                console_puts(" last_rc=");
                uart_put_s32((int)st.last_rc);
                console_puts("\r\n");

                console_puts("registry:\r\n");
                for (uint8_t id = 0u; id < SCHED_MAX_AO; id++)
                {
                        const restore_task_descriptor_t *d = restore_registry_find(id);
                        if (d == 0)
                                continue;
                        console_puts("  id=");
                        console_put_u32(id);
                        console_puts(" class=");
                        console_puts(restore_class_name(d->task_class));
                        console_puts(" ver=");
                        console_put_u32(d->state_version);
                        console_puts("\r\n");
                }
                return;
        }

        if (streq(argv[0], "ckptq"))
        {
                console_puts("ckptq: pending=");
                console_put_u32(restore_sim_pending());
                console_puts(" gen=");
                console_put_u32(restore_sim_generation());
                console_puts("\r\n");
                return;
        }

        if (streq(argv[0], "ckptsave"))
        {
                counter_task_state_t st;
                uint8_t blob[sizeof(restorable_envelope_t)];
                uint32_t blob_len = (uint32_t)sizeof(blob);
                int rc = counter_task_get_state(&st);
                if (rc != SCHED_OK)
                {
                        console_puts("ckptsave: counter state err=");
                        uart_put_s32(rc);
                        console_puts("\r\n");
                        return;
                }
                (void)restore_sim_reset();
                rc = counter_task_encode_restore_envelope(&st, 0, 0, blob, &blob_len);
                if (rc != SCHED_OK)
                {
                        console_puts("ckptsave: encode err=");
                        uart_put_s32(rc);
                        console_puts("\r\n");
                        return;
                }
                rc = restore_sim_enqueue((uint16_t)AO_COUNTER, 2u, blob, blob_len);
                if (rc != SCHED_OK)
                {
                        console_puts("ckptsave: enqueue err=");
                        uart_put_s32(rc);
                        console_puts("\r\n");
                        return;
                }
                console_puts("ckptsave: ok value=");
                console_put_u32(st.value);
                console_puts(" limit=");
                console_put_u32(st.limit);
                console_puts(" pending=");
                console_put_u32(restore_sim_pending());
                console_puts("\r\n");
                return;
        }

        if (streq(argv[0], "ckptsaveop"))
        {
                uint32_t mul = 1u;
                uint32_t div = 1u;
                counter_task_state_t st;
                counter_restore_op_t ops[2];
                uint16_t op_count = 0u;
                uint8_t blob[sizeof(restorable_envelope_t)];
                uint32_t blob_len = (uint32_t)sizeof(blob);
                int rc;

                if (argc < 3)
                {
                        console_puts("usage: ckptsaveop <mul> <div>\r\n");
                        return;
                }
                if (!parse_u32(argv[1], &mul) || !parse_u32(argv[2], &div))
                {
                        console_puts("ckptsaveop: bad args\r\n");
                        return;
                }
                if (counter_task_get_state(&st) != SCHED_OK)
                {
                        console_puts("ckptsaveop: counter state err\r\n");
                        return;
                }
                if (mul > 1u)
                {
                        ops[op_count].op = COUNTER_RESTORE_OP_MUL;
                        ops[op_count].reserved[0] = 0u;
                        ops[op_count].reserved[1] = 0u;
                        ops[op_count].reserved[2] = 0u;
                        ops[op_count].operand = mul;
                        op_count++;
                }
                if (div > 1u)
                {
                        ops[op_count].op = COUNTER_RESTORE_OP_DIV;
                        ops[op_count].reserved[0] = 0u;
                        ops[op_count].reserved[1] = 0u;
                        ops[op_count].reserved[2] = 0u;
                        ops[op_count].operand = div;
                        op_count++;
                }
                rc = counter_task_encode_restore_envelope(&st, ops, op_count, blob, &blob_len);
                if (rc != SCHED_OK)
                {
                        console_puts("ckptsaveop: encode err=");
                        uart_put_s32(rc);
                        console_puts("\r\n");
                        return;
                }

                (void)restore_sim_reset();
                rc = restore_sim_enqueue((uint16_t)AO_COUNTER, 2u, blob, blob_len);
                if (rc != SCHED_OK)
                {
                        console_puts("ckptsaveop: enqueue err=");
                        uart_put_s32(rc);
                        console_puts("\r\n");
                        return;
                }
                console_puts("ckptsaveop: ok ops=");
                console_put_u32(op_count);
                console_puts(" pending=");
                console_put_u32(restore_sim_pending());
                console_puts("\r\n");
                return;
        }

        if (streq(argv[0], "ckptload"))
        {
                uint32_t applied = 0u, skipped = 0u, failed = 0u;
                counter_task_state_t st;
                int rc = restore_sim_apply(g_sched, &applied, &skipped, &failed);
                console_puts("ckptload: rc=");
                uart_put_s32(rc);
                console_puts(" applied=");
                console_put_u32(applied);
                console_puts(" skipped=");
                console_put_u32(skipped);
                console_puts(" failed=");
                console_put_u32(failed);
                console_puts("\r\n");

                if (counter_task_get_state(&st) == SCHED_OK)
                {
                        console_puts("counter state: active=");
                        console_put_u32(st.active);
                        console_puts(" value=");
                        console_put_u32(st.value);
                        console_puts(" limit=");
                        console_put_u32(st.limit);
                        console_puts(" bg=");
                        console_put_u32(st.bg);
                        console_puts("\r\n");
                }
                return;
        }

        if (streq(argv[0], "restoresim"))
        {
                uint32_t limit = 50u;
                uint32_t value = 25u;
                uint32_t bg = 0u;
                counter_task_state_t st;
                uint8_t blob[sizeof(restorable_envelope_t)];
                uint32_t blob_len = (uint32_t)sizeof(blob);
                uint32_t applied = 0u, skipped = 0u, failed = 0u;
                int rc;

                if (argc >= 2 && !parse_u32(argv[1], &limit))
                {
                        console_puts("restoresim: bad limit\r\n");
                        return;
                }
                if (argc >= 3 && !parse_u32(argv[2], &value))
                {
                        console_puts("restoresim: bad value\r\n");
                        return;
                }
                if (argc >= 4 && !parse_u32(argv[3], &bg))
                {
                        console_puts("restoresim: bad bg\r\n");
                        return;
                }
                if (limit == 0u)
                        limit = 1u;
                if (value == 0u)
                        value = 1u;
                if (value > limit)
                        value = limit;

                st.active = 1u;
                st.bg = (uint8_t)(bg & 1u);
                st.step_pending = 0u;
                st.limit = limit;
                st.value = value;
                st.next_tick = systick_now() + 10u;

                (void)restore_sim_reset();
                rc = counter_task_encode_restore_envelope(&st, 0, 0, blob, &blob_len);
                if (rc != SCHED_OK)
                {
                        console_puts("restoresim: encode err=");
                        uart_put_s32(rc);
                        console_puts("\r\n");
                        return;
                }
                rc = restore_sim_enqueue((uint16_t)AO_COUNTER, 2u, blob, blob_len);
                if (rc != SCHED_OK)
                {
                        console_puts("restoresim: enqueue err=");
                        uart_put_s32(rc);
                        console_puts("\r\n");
                        return;
                }
                /* Also enqueue one restart-only task entry to validate skip accounting. */
                rc = restore_sim_enqueue((uint16_t)AO_TERMINAL, 0u, 0, 0u);
                if (rc != SCHED_OK)
                {
                        console_puts("restoresim: enqueue2 err=");
                        uart_put_s32(rc);
                        console_puts("\r\n");
                        return;
                }

                rc = restore_sim_apply(g_sched, &applied, &skipped, &failed);
                console_puts("restoresim: rc=");
                uart_put_s32(rc);
                console_puts(" applied=");
                console_put_u32(applied);
                console_puts(" skipped=");
                console_put_u32(skipped);
                console_puts(" failed=");
                console_put_u32(failed);
                console_puts("\r\n");

                if (counter_task_get_state(&st) == SCHED_OK)
                {
                        console_puts("counter state: active=");
                        console_put_u32(st.active);
                        console_puts(" value=");
                        console_put_u32(st.value);
                        console_puts(" limit=");
                        console_put_u32(st.limit);
                        console_puts(" bg=");
                        console_put_u32(st.bg);
                        console_puts("\r\n");
                }
                return;
        }

        if (streq(argv[0], "restoresimop"))
        {
                uint32_t limit = 100u;
                uint32_t value = 10u;
                uint32_t mul = 2u;
                uint32_t div = 1u;
                uint32_t bg = 0u;
                counter_task_state_t st;
                counter_restore_op_t ops[2];
                uint16_t op_count = 0u;
                uint8_t blob[sizeof(restorable_envelope_t)];
                uint32_t blob_len = (uint32_t)sizeof(blob);
                uint32_t applied = 0u, skipped = 0u, failed = 0u;
                int rc;

                if (argc < 5)
                {
                        console_puts("usage: restoresimop <limit> <value> <mul> <div> [bg]\r\n");
                        return;
                }
                if (!parse_u32(argv[1], &limit) ||
                    !parse_u32(argv[2], &value) ||
                    !parse_u32(argv[3], &mul) ||
                    !parse_u32(argv[4], &div))
                {
                        console_puts("restoresimop: bad args\r\n");
                        return;
                }
                if (argc >= 6 && !parse_u32(argv[5], &bg))
                {
                        console_puts("restoresimop: bad bg\r\n");
                        return;
                }
                if (limit == 0u) limit = 1u;
                if (value == 0u) value = 1u;
                if (value > limit) value = limit;
                if (mul > 1u) {
                        ops[op_count].op = COUNTER_RESTORE_OP_MUL;
                        ops[op_count].reserved[0] = 0u;
                        ops[op_count].reserved[1] = 0u;
                        ops[op_count].reserved[2] = 0u;
                        ops[op_count].operand = mul;
                        op_count++;
                }
                if (div > 1u) {
                        ops[op_count].op = COUNTER_RESTORE_OP_DIV;
                        ops[op_count].reserved[0] = 0u;
                        ops[op_count].reserved[1] = 0u;
                        ops[op_count].reserved[2] = 0u;
                        ops[op_count].operand = div;
                        op_count++;
                }

                st.active = 1u;
                st.bg = (uint8_t)(bg & 1u);
                st.step_pending = 0u;
                st.limit = limit;
                st.value = value;
                st.next_tick = systick_now() + 10u;

                rc = counter_task_encode_restore_envelope(&st, ops, op_count, blob, &blob_len);
                if (rc != SCHED_OK)
                {
                        console_puts("restoresimop: encode err=");
                        uart_put_s32(rc);
                        console_puts("\r\n");
                        return;
                }
                (void)restore_sim_reset();
                rc = restore_sim_enqueue((uint16_t)AO_COUNTER, 2u, blob, blob_len);
                if (rc != SCHED_OK)
                {
                        console_puts("restoresimop: enqueue err=");
                        uart_put_s32(rc);
                        console_puts("\r\n");
                        return;
                }
                rc = restore_sim_apply(g_sched, &applied, &skipped, &failed);
                console_puts("restoresimop: rc=");
                uart_put_s32(rc);
                console_puts(" applied=");
                console_put_u32(applied);
                console_puts(" skipped=");
                console_put_u32(skipped);
                console_puts(" failed=");
                console_put_u32(failed);
                console_puts("\r\n");

                if (counter_task_get_state(&st) == SCHED_OK)
                {
                        console_puts("counter state: value=");
                        console_put_u32(st.value);
                        console_puts(" limit=");
                        console_put_u32(st.limit);
                        console_puts(" bg=");
                        console_put_u32(st.bg);
                        console_puts("\r\n");
                }
                return;
        }

        if (streq(argv[0], "restoresimbad"))
        {
                const char *mode = (argc >= 2) ? argv[1] : "ver";
                counter_task_state_t st;
                counter_restore_op_t op;
                uint8_t blob[sizeof(restorable_envelope_t)];
                uint32_t blob_len = (uint32_t)sizeof(blob);
                uint32_t applied = 0u, skipped = 0u, failed = 0u;
                int rc;

                st.active = 1u;
                st.bg = 0u;
                st.step_pending = 0u;
                st.limit = 100u;
                st.value = 10u;
                st.next_tick = systick_now() + 10u;
                op.op = COUNTER_RESTORE_OP_MUL;
                op.reserved[0] = 0u;
                op.reserved[1] = 0u;
                op.reserved[2] = 0u;
                op.operand = 3u;

                rc = counter_task_encode_restore_envelope(&st, &op, 1u, blob, &blob_len);
                if (rc != SCHED_OK)
                {
                        console_puts("restoresimbad: encode err=");
                        uart_put_s32(rc);
                        console_puts("\r\n");
                        return;
                }

                {
                        restorable_envelope_t *env = (restorable_envelope_t *)blob;
                        if (streq(mode, "ver")) {
                                env->hdr.version = (uint16_t)(env->hdr.version + 1u);
                        } else if (streq(mode, "action")) {
                                env->entries[1].action = 0xFFFFu;
                        } else if (streq(mode, "div0")) {
                                counter_restore_op_t *bad_op = (counter_restore_op_t *)env->entries[1].data;
                                bad_op->op = COUNTER_RESTORE_OP_DIV;
                                bad_op->operand = 0u;
                        } else if (streq(mode, "len")) {
                                env->entries[1].data_len = (uint16_t)(sizeof(counter_restore_op_t) - 1u);
                        } else {
                                console_puts("restoresimbad: mode must be ver|action|div0|len\r\n");
                                return;
                        }
                }

                (void)restore_sim_reset();
                rc = restore_sim_enqueue((uint16_t)AO_COUNTER, 2u, blob, blob_len);
                if (rc != SCHED_OK)
                {
                        console_puts("restoresimbad: enqueue err=");
                        uart_put_s32(rc);
                        console_puts("\r\n");
                        return;
                }
                rc = restore_sim_apply(g_sched, &applied, &skipped, &failed);
                console_puts("restoresimbad: mode=");
                console_puts(mode);
                console_puts(" rc=");
                uart_put_s32(rc);
                console_puts(" applied=");
                console_put_u32(applied);
                console_puts(" skipped=");
                console_put_u32(skipped);
                console_puts(" failed=");
                console_put_u32(failed);
                console_puts("\r\n");
                return;
        }

        console_puts("unknown cmd: ");
        console_puts(argv[0]);
        console_puts("\r\n");
}

static void term_enqueue_dispatch(char *line)
{
        if (g_sched == 0)
        {
                PANIC("terminal scheduler not bound");
        }

        int slot = cmd_slot_alloc_copy(line);
        if (slot < 0)
        {
                console_puts("cmd queue full\r\n");
                return;
        }

        /* Strip trailing whitespace and check for background '&' operator. */
        char *s = g_cmd_slots[slot].line;
        uint32_t slen = 0u;
        while (s[slen] != '\0') { slen++; }
        while (slen > 0u && (s[slen - 1u] == ' ' || s[slen - 1u] == '\t')) { slen--; }
        uint8_t is_bg = 0u;
        if (slen > 0u && s[slen - 1u] == '&')
        {
                is_bg = 1u;
                slen--;
                /* Also strip any whitespace before '&'. */
                while (slen > 0u && (s[slen - 1u] == ' ' || s[slen - 1u] == '\t')) { slen--; }
                s[slen] = '\0';
        }

        int rc = sched_post(g_sched, AO_CMD,
                            &(event_t){ .sig = CMD_SIG_EXEC, .arg0 = (uintptr_t)slot, .arg1 = (uintptr_t)is_bg });
        if (rc != SCHED_OK)
        {
                cmd_slot_release((uint8_t)slot);
                console_puts("cmd dispatch failed\r\n");
        }
}

static void term_on_fg_shortcut_interrupt(void)
{
        if (!term_stdin_owner_valid())
        {
                term_stdin_release_internal();
                console_puts("\r\nstdin owner stale; released\r\n");
                console_puts(g_term_ctx.shell.prompt_str);
                return;
        }

        uint8_t owner = g_term_ctx.stdin_owner;
        const char *name = 0;
        int rc = term_kill_task(owner, &name);

        console_puts("^C\r\n");
        if (rc == SCHED_OK)
        {
                console_puts("killed ");
                if (name && name[0] != '\0')
                        console_puts(name);
                else
                        console_puts("(unnamed)");
                console_puts("\r\n");
        }
        else
        {
                console_puts("interrupt: kill err=");
                uart_put_s32(rc);
                console_puts("\r\n");
        }
        console_puts(g_term_ctx.shell.prompt_str);
}

static void term_dispatch_owned_input(void)
{
        if (!term_stdin_owner_valid())
        {
                term_stdin_release_internal();
                console_puts("\r\nstdin owner stale; released\r\n");
                console_puts(g_term_ctx.shell.prompt_str);
                return;
        }

        int c = uart_async_getc();
        if (c < 0)
                return;

        if (g_term_ctx.stdin_mode == TERM_STDIN_MODE_RAW)
        {
                if ((uint8_t)c == TERM_SHORTCUT_CTRL_C)
                {
                        term_on_fg_shortcut_interrupt();
                        return;
                }

                int rc = sched_post(g_sched, g_term_ctx.stdin_owner,
                                    &(event_t){ .sig = TERM_SIG_STDIN_RAW, .arg0 = (uintptr_t)((uint8_t)c) });
                if (rc != SCHED_OK)
                {
                        if (rc == SCHED_ERR_NOT_FOUND || rc == SCHED_ERR_DISABLED)
                                term_stdin_release_internal();
                        console_puts("\r\nstdin dispatch err=");
                        uart_put_s32(rc);
                        console_puts("\r\n");
                        console_puts(g_term_ctx.shell.prompt_str);
                }
                return;
        }

        /* Unknown mode: fail closed by releasing lock back to shell. */
        term_stdin_release_internal();
        console_puts("\r\nstdin mode invalid; released\r\n");
        console_puts(g_term_ctx.shell.prompt_str);
}

static void terminal_task_dispatch(ao_t *self, const event_t *e)
{
        (void)self;
        PANIC_IF(e == 0, "terminal dispatch: null event");

        if (e->sig == TERM_SIG_CKPT_TIMER) {
                uint32_t lba = 0u, slot = 0u, seq = 0u, regions = 0u;
                if (term_ckptsave_sd_once(&lba, &slot, &seq, &regions) == SCHED_OK) {
                        log_puts("ckpt:auto seq=");
                        log_put_u32(seq);
                        log_puts(" slot=");
                        log_put_u32(slot);
                        log_puts(" regions=");
                        log_put_u32(regions);
                        log_puts("\r\n");
                }
                return;
        }

        if (e->sig == TERM_SIG_REPRINT_PROMPT) {
                if (!term_stdin_owner_valid())
                        console_puts(g_term_ctx.shell.prompt_str);
                return;
        }

        if (e->sig != TERM_SIG_UART_RX_READY)
                return;

        for (;;) {
                while (uart_rx_available()) {
                        if (!term_stdin_owner_valid())
                                shell_tick(&g_term_ctx.shell, term_enqueue_dispatch);
                        else
                                term_dispatch_owned_input();
                }
                if (!uart_async_rx_event_finish()) {
                        if (!term_stdin_owner_valid())
                                shell_rx_idle(&g_term_ctx.shell);
                        break;
                }
        }
}

int terminal_task_register(scheduler_t *sched)
{
        if (sched == 0)
                return SCHED_ERR_PARAM;

        g_sched = sched;
        shell_state_init(&g_term_ctx.shell, "rewind> ");
        term_stdin_release_internal();
        uart_async_bind_scheduler(sched, AO_TERMINAL, TERM_SIG_UART_RX_READY);

        task_spec_t spec;
        spec.id = AO_TERMINAL;
        spec.prio = 1;
        spec.dispatch = terminal_task_dispatch;
        spec.ctx = &g_term_ctx;
        spec.queue_storage = g_term_queue_storage;
        spec.queue_capacity = (uint16_t)(sizeof(g_term_queue_storage) / sizeof(g_term_queue_storage[0]));
        spec.rtc_budget_ticks = 1;
        spec.name = "terminal";

        return sched_register_task(sched, &spec);
}

static void cmd_task_dispatch(ao_t *self, const event_t *e)
{
        (void)self;
        PANIC_IF(e == 0, "cmd dispatch: null event");
        if (e->sig != CMD_SIG_EXEC)
                return;

        uint8_t idx    = (uint8_t)e->arg0;
        uint8_t is_bg  = (uint8_t)(e->arg1 & 1u);
        PANIC_IF(idx >= CMD_MAILBOX_CAP, "cmd event idx out of range");
        PANIC_IF(g_cmd_slots[idx].used == 0u, "cmd event for free slot");

        /* Snapshot the command name before term_execute may modify the slot. */
        char cmd_name[32];
        uint32_t ni = 0u;
        const char *src = g_cmd_slots[idx].line;
        /* Skip leading whitespace. */
        while (*src == ' ' || *src == '\t') { src++; }
        /* Copy first token. */
        while (*src != '\0' && *src != ' ' && *src != '\t' && ni < (uint32_t)(sizeof(cmd_name) - 1u))
        {
                cmd_name[ni++] = *src++;
        }
        cmd_name[ni] = '\0';

        g_cmd_bg_ctx   = is_bg;
        g_cmd_bg_async = 0u;
        g_cmd_fg_async = 0u;
        if (is_bg)
                console_set_sink(CONSOLE_SINK_LOG);

        term_execute(g_cmd_slots[idx].line);

        console_set_sink(CONSOLE_SINK_UART);
        if (is_bg && !g_cmd_bg_async)
        {
                console_puts("[done: ");
                console_puts(cmd_name);
                console_puts("]\r\n");
        }
        g_cmd_bg_ctx = 0u;

        cmd_slot_release(idx);

        /* Print prompt after output so it always appears last.
         * Foreground async commands defer prompt until completion.
         * Background async commands return prompt immediately. */
        if (!g_cmd_fg_async)
                console_puts(g_term_ctx.shell.prompt_str);
        g_cmd_fg_async = 0u;
}

int cmd_task_register(scheduler_t *sched)
{
        if (sched == 0)
                return SCHED_ERR_PARAM;

        task_spec_t spec;
        spec.id = AO_CMD;
        spec.prio = 0;
        spec.dispatch = cmd_task_dispatch;
        spec.ctx = g_cmd_slots;
        spec.queue_storage = g_cmd_queue_storage;
        spec.queue_capacity = (uint16_t)(sizeof(g_cmd_queue_storage) / sizeof(g_cmd_queue_storage[0]));
        spec.rtc_budget_ticks = 1;
        spec.name = "cmd";

        for (uint32_t i = 0; i < CMD_MAILBOX_CAP; i++)
                g_cmd_slots[i].used = 0u;
        g_cmd_alloc_cursor = 0u;

        return sched_register_task(sched, &spec);
}

static int terminal_restore_register_fn(scheduler_t *sched, const launch_intent_t *intent)
{
        (void)intent;
        return terminal_task_register(sched);
}

int terminal_task_register_restore_descriptor(void)
{
        static const restore_task_descriptor_t desc = {
                .task_id = AO_TERMINAL,
                .task_class = TASK_CLASS_RESTART_ONLY,
                .state_version = 0u,
                .min_state_len = 0u,
                .max_state_len = 0u,
                .register_fn = terminal_restore_register_fn,
                .get_state_fn = 0,
                .restore_fn = 0,
                .ui_rehydrate_fn = 0
        };
        return restore_registry_register_descriptor(&desc);
}

static int cmd_restore_register_fn(scheduler_t *sched, const launch_intent_t *intent)
{
        (void)intent;
        return cmd_task_register(sched);
}

int cmd_task_register_restore_descriptor(void)
{
        static const restore_task_descriptor_t desc = {
                .task_id = AO_CMD,
                .task_class = TASK_CLASS_RESTART_ONLY,
                .state_version = 0u,
                .min_state_len = 0u,
                .max_state_len = 0u,
                .register_fn = cmd_restore_register_fn,
                .get_state_fn = 0,
                .restore_fn = 0,
                .ui_rehydrate_fn = 0
        };
        return restore_registry_register_descriptor(&desc);
}
