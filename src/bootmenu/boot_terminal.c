#include "../include/lineio.h"
#include "../include/parse.h"
#include "../include/uart.h"
#include "../include/jump.h"
#include "../include/sd.h"
#include "../include/rewind_restore.h"

extern void enable_sdmmc1_kerclk_sysclk(void);
extern void enable_sdmmc1_kerclk_pll48(void);
extern void pc8pc12_to_sdmmc1(void);
extern void pd2_to_sdmmc1(void);
extern void enable_gpioc(void);
extern void enable_gpiod(void);

#define APP_BASE 0x08020000u
#define MAX_ARGUMENTS 8
#define SD_DUMP_BYTES 64u
#define SD_READ_MAX_BLOCKS 4u
#define RCC_BASE 0x40023800u
#define SDMMC1_BASE 0x40012C00u
#define GPIOC_BASE 0x40020800u
#define GPIOD_BASE 0x40020C00u
#define ADMIN_MAGIC 0x53535752u /* "RWSS" */
#define ADMIN_PROBE_LBA 1u
#define ADMIN_MIRROR_LBA 2u
#define ADMIN_VERSION 1u
#define ADMIN_RSV_SNAP_START_LBA 0u
#define ADMIN_RSV_SNAP_BLOCKS 1u
#define ADMIN_RSV_SNAP_SLOT_BLOCKS 2u
#define ADMIN_RSV_BOOT_TARGET_MODE 3u
#define ADMIN_RSV_BOOT_TARGET_SLOT 4u
#define ADMIN_RSV_BOOT_TARGET_SEQ 5u
#define ADMIN_RSV_LAST_SNAP_SLOT 6u
#define ADMIN_RSV_LAST_SNAP_SEQ 7u
#define ADMIN_RSV_JRN_START_LBA 8u
#define ADMIN_RSV_JRN_BLOCKS 9u
#include "../include/snapshot_config.h"
#define SNAP_PART_PCT 25u
#define JOURNAL_PART_PCT 5u
#define ADMIN_BLOCKS_MIN 32u
#define MIN_SNAP_BLOCKS 2048u
#define MIN_FS_BLOCKS 2048u
#define SNAPSHOT_HDR_MAGIC 0x50414E53u   /* "SNAP" */
#define SNAPSHOT_CMIT_MAGIC 0x54494D43u  /* "CMIT" */
#define SNAPSHOT_FMT_VERSION 1u
#define SNAPSHOT_HDR_WORDS 7u
#define SNAPSHOT_CMIT_WORDS 4u
#define SNAPSHOT_MIN_SLOT_BLOCKS 2u
#define SNAPSHOT_PAY0_MAGIC 0x30594150u /* "PAY0" */
#define SNAPSHOT_PAYE_MAGIC 0x45594150u /* "PAYE" */
#define SNAPSHOT_RGS0_MAGIC 0x30534752u /* "RGS0" */
#define SNAPSHOT_RGE0_MAGIC 0x30454752u /* "RGE0" */
#define SNAPSHOT_PAY0_WORDS 7u
#define SNAPSHOT_PAYE_WORDS 4u
#define SNAPSHOT_RGS0_WORDS 4u
#define SNAPSHOT_RGE0_WORDS 5u
#define BOOT_TARGET_RECENT 0u
#define BOOT_TARGET_FRESH 1u
#define BOOT_TARGET_SNAPSHOT 2u
#define BOOT_SNAPSHOT_MENU_COUNT 3u
#define BOOT_MENU_ITEM_COUNT 9u
#define SETUP_MENU_ITEM_COUNT 4u
#define BOOT_ERR_NOT_FOUND -100
#define BOOT_RESTORE_STACK_WINDOW_BYTES 8192u
#define BOOT_RESTORE_STACK_GUARD_BYTES 256u

typedef struct
{
        int app_present;
        int sd_present;
        int sd_init_ok;
        int sd_init_err;
        int has_mbr_signature;
        int has_admin_magic;
        uint32_t capacity_blocks;
        uint32_t capacity_mb;
        uint32_t fs_free_mb_est;
        uint32_t snapshot_slots;
        uint32_t snapshot_interval_s;
        uint32_t snapshot_start_lba;
        uint32_t snapshot_blocks;
        uint32_t snapshot_slot_blocks;
        uint32_t snapshot_valid_count;
        uint32_t snapshot_slot_idx[BOOT_SNAPSHOT_MENU_COUNT];
        uint32_t snapshot_slot_seq[BOOT_SNAPSHOT_MENU_COUNT];
        uint32_t boot_target_mode;
        uint32_t boot_target_slot;
        uint32_t boot_target_seq;
} boot_status_t;

typedef struct
{
        uint32_t magic;
        uint32_t version;
        uint32_t generation;
        uint32_t total_blocks;
        uint32_t fallback_mode;
        uint32_t snapshot_slots;
        uint32_t snapshot_interval_s;
        uint32_t reserved[10];
} boot_admin_block_t;

typedef struct
{
        uint32_t magic;
        uint32_t version;
        uint32_t seq;
        uint32_t slot_index;
        uint32_t payload_lba;
        uint32_t payload_blocks;
        uint32_t flags;
        uint32_t checksum;
        uint32_t reserved[(SD_BLOCK_SIZE / 4u) - 8u];
} boot_snapshot_hdr_t;

typedef struct
{
        uint32_t magic;
        uint32_t version;
        uint32_t seq;
        uint32_t slot_index;
        uint32_t checksum;
        uint32_t reserved[(SD_BLOCK_SIZE / 4u) - 5u];
} boot_snapshot_commit_t;

typedef struct
{
        uint32_t magic;
        uint32_t version;
        uint32_t reason;
        uint32_t captured_tick;
        uint32_t ready_bitmap;
        uint32_t region_count;
        uint32_t total_region_bytes;
        uint32_t checksum;
        uint32_t reserved[(SD_BLOCK_SIZE / 4u) - 8u];
} boot_snapshot_payload_start_t;

typedef struct
{
        uint32_t magic;
        uint32_t version;
        uint32_t region_count;
        uint32_t total_region_bytes;
        uint32_t checksum;
        uint32_t reserved[(SD_BLOCK_SIZE / 4u) - 5u];
} boot_snapshot_payload_end_t;

typedef struct
{
        uint32_t magic;
        uint32_t version;
        uint32_t region_addr;
        uint32_t region_len;
        uint32_t checksum;
        uint32_t reserved[(SD_BLOCK_SIZE / 4u) - 5u];
} boot_snapshot_region_start_t;

typedef struct
{
        uint32_t magic;
        uint32_t version;
        uint32_t region_addr;
        uint32_t region_len;
        uint32_t region_crc;
        uint32_t checksum;
        uint32_t reserved[(SD_BLOCK_SIZE / 4u) - 6u];
} boot_snapshot_region_end_t;

typedef enum
{
        BOOT_ACT_RECENT = 0,
        BOOT_ACT_FRESH,
        BOOT_ACT_SNAP0,
        BOOT_ACT_SNAP1,
        BOOT_ACT_SNAP2,
        BOOT_ACT_SETUP,
        BOOT_ACT_REFRESH,
        BOOT_ACT_RECOVERY,
        BOOT_ACT_SHELL
} boot_action_t;

typedef enum
{
        SETUP_ACT_FORMAT = 0,
        SETUP_ACT_CONFIG,
        SETUP_ACT_REFRESH,
        SETUP_ACT_RECOVERY
} boot_setup_action_t;

typedef struct
{
        const char *label;
        boot_action_t action;
        int enabled;
} boot_menu_item_t;

typedef struct
{
        const char *label;
        boot_setup_action_t action;
        int enabled;
} boot_setup_item_t;

enum
{
        KEY_NONE = 0,
        KEY_UP,
        KEY_DOWN,
        KEY_ENTER
};

static uint32_t sd_buf_words[SD_BLOCK_SIZE / 4u];
__attribute__((section(".snap_exclude"), aligned(8)))
static uint32_t sd_restore_words[SD_BLOCK_SIZE / 4u];
extern uint8_t _sram_start;
extern uint8_t _sram_end;

static void boot_dispatch(char *line);
static void boot_menu_print_line(int selected, const char *label, int enabled);
static int boot_admin_read_best(boot_admin_block_t *out);
static int boot_ensure_sd_ready(boot_status_t *st);

static void uart_put_s32(int v)
{
        if (v < 0)
        {
                uart_putc('-');
                uart_put_u32((uint32_t)(-v));
                return;
        }
        uart_put_u32((uint32_t)v);
}

static void uart_put_hex8(uint8_t v)
{
        static const char *hx = "0123456789ABCDEF";
        uart_putc(hx[(v >> 4) & 0xF]);
        uart_putc(hx[v & 0xF]);
}

static void sd_dump_bytes(const uint8_t *buf, uint32_t count)
{
        for (uint32_t i = 0; i < count; i++)
        {
                uart_put_hex8(buf[i]);
                if ((i & 0x0Fu) == 0x0Fu)
                        uart_puts("\r\n");
                else
                        uart_putc(' ');
        }

        if ((count & 0x0Fu) != 0u)
                uart_puts("\r\n");
}

static int sd_require_present(void)
{
        sd_detect_init();
        if (!sd_is_detected())
        {
                uart_puts("sd: not present\r\n");
                return 0;
        }
        return 1;
}

static void sd_print_info(void)
{
        const sd_info_t *info = sd_get_info();
        if (!info->initialized)
        {
                uart_puts("sd not initialized\r\n");
                return;
        }

        uart_puts("rca=");
        uart_put_hex32(info->rca);
        uart_puts(" ocr=");
        uart_put_hex32(info->ocr);
        uart_puts("\r\n");

        uart_puts("capacity_blocks=");
        uart_put_u32(info->capacity_blocks);
        uart_puts(" capacity_mb=");
        uart_put_u32(info->capacity_blocks / 2048u);
        uart_puts("\r\n");

        uart_puts("high_capacity=");
        uart_put_u32(info->high_capacity);
        uart_puts(" bus_width=");
        uart_put_u32(info->bus_width);
        uart_puts("\r\n");
}

static void dump_reg(const char *name, uint32_t addr)
{
        uart_puts(name);
        uart_puts("=");
        uart_put_hex32(*(volatile uint32_t *)addr);
        uart_puts("\r\n");
}

static void rcc_dump(void)
{
        dump_reg("RCC_CR", RCC_BASE + 0x00u);
        dump_reg("RCC_PLLCFGR", RCC_BASE + 0x04u);
        dump_reg("RCC_CFGR", RCC_BASE + 0x08u);
        dump_reg("RCC_DCKCFGR1", RCC_BASE + 0x8Cu);
        dump_reg("RCC_DCKCFGR2", RCC_BASE + 0x90u);
        dump_reg("RCC_APB2ENR", RCC_BASE + 0x44u);
}

static void sdmmc_dump(void)
{
        dump_reg("SDMMC_POWER", SDMMC1_BASE + 0x00u);
        dump_reg("SDMMC_CLKCR", SDMMC1_BASE + 0x04u);
        dump_reg("SDMMC_ARG", SDMMC1_BASE + 0x08u);
        dump_reg("SDMMC_CMD", SDMMC1_BASE + 0x0Cu);
        dump_reg("SDMMC_RESP1", SDMMC1_BASE + 0x14u);
        dump_reg("SDMMC_RESP2", SDMMC1_BASE + 0x18u);
        dump_reg("SDMMC_RESP3", SDMMC1_BASE + 0x1Cu);
        dump_reg("SDMMC_RESP4", SDMMC1_BASE + 0x20u);
        dump_reg("SDMMC_DTIMER", SDMMC1_BASE + 0x24u);
        dump_reg("SDMMC_DLEN", SDMMC1_BASE + 0x28u);
        dump_reg("SDMMC_DCTRL", SDMMC1_BASE + 0x2Cu);
        dump_reg("SDMMC_STA", SDMMC1_BASE + 0x34u);
        dump_reg("SDMMC_ICR", SDMMC1_BASE + 0x38u);
        dump_reg("SDMMC_MASK", SDMMC1_BASE + 0x3Cu);
        dump_reg("SDMMC_FIFOCNT", SDMMC1_BASE + 0x48u);
        uart_puts("SD_LAST_CMD=");
        uart_put_hex32(sd_last_cmd());
        uart_puts("\r\n");
        uart_puts("SD_LAST_STA=");
        uart_put_hex32(sd_last_sta());
        uart_puts("\r\n");
}

static void sdmmc_set_clkdiv(uint32_t div)
{
        volatile uint32_t *clkcr = (uint32_t *)(SDMMC1_BASE + 0x04u);
        uint32_t val = *clkcr;
        val &= ~0xFFu;
        val |= (div & 0xFFu);
        val |= (1u << 8);
        *clkcr = val;
}

static void sdmmc_set_kernel_clk_sys(void)
{
        enable_sdmmc1_kerclk_sysclk();
}

static void sdmmc_set_kernel_clk_pll(void)
{
        enable_sdmmc1_kerclk_pll48();
}

static void gpio_dump_port(const char *name, uint32_t base)
{
        uart_puts(name);
        uart_puts("\r\n");
        dump_reg("MODER", base + 0x00u);
        dump_reg("OTYPER", base + 0x04u);
        dump_reg("OSPEEDR", base + 0x08u);
        dump_reg("PUPDR", base + 0x0Cu);
        dump_reg("IDR", base + 0x10u);
        dump_reg("AFRL", base + 0x20u);
        dump_reg("AFRH", base + 0x24u);
}

static void sd_lines_dump(void)
{
        uint32_t idrc = *(volatile uint32_t *)(GPIOC_BASE + 0x10u);
        uint32_t idrd = *(volatile uint32_t *)(GPIOD_BASE + 0x10u);

        uart_puts("SD_LINES D0=");
        uart_put_u32((idrc >> 8) & 1u);
        uart_puts(" D1=");
        uart_put_u32((idrc >> 9) & 1u);
        uart_puts(" D2=");
        uart_put_u32((idrc >> 10) & 1u);
        uart_puts(" D3=");
        uart_put_u32((idrc >> 11) & 1u);
        uart_puts(" CK=");
        uart_put_u32((idrc >> 12) & 1u);
        uart_puts(" CMD=");
        uart_put_u32((idrd >> 2) & 1u);
        uart_puts("\r\n");
}

static void gpio_dump(void)
{
        gpio_dump_port("GPIOC", GPIOC_BASE);
        gpio_dump_port("GPIOD", GPIOD_BASE);
        sd_lines_dump();
}

static void sd_toggle_lines(uint32_t cycles)
{
        volatile uint32_t *gpioc_moder = (uint32_t *)(GPIOC_BASE + 0x00u);
        volatile uint32_t *gpioc_otyper = (uint32_t *)(GPIOC_BASE + 0x04u);
        volatile uint32_t *gpioc_ospeedr = (uint32_t *)(GPIOC_BASE + 0x08u);
        volatile uint32_t *gpioc_pupdr = (uint32_t *)(GPIOC_BASE + 0x0Cu);
        volatile uint32_t *gpioc_odr = (uint32_t *)(GPIOC_BASE + 0x14u);
        volatile uint32_t *gpiod_moder = (uint32_t *)(GPIOD_BASE + 0x00u);
        volatile uint32_t *gpiod_otyper = (uint32_t *)(GPIOD_BASE + 0x04u);
        volatile uint32_t *gpiod_ospeedr = (uint32_t *)(GPIOD_BASE + 0x08u);
        volatile uint32_t *gpiod_pupdr = (uint32_t *)(GPIOD_BASE + 0x0Cu);
        volatile uint32_t *gpiod_odr = (uint32_t *)(GPIOD_BASE + 0x14u);

        enable_gpioc();
        enable_gpiod();

        uint32_t moder = *gpioc_moder;
        moder &= ~0x03FF0000u;
        moder |= 0x01550000u;
        *gpioc_moder = moder;
        *gpioc_otyper &= ~0x00001F00u;
        uint32_t ospeed = *gpioc_ospeedr;
        ospeed &= ~0x03FF0000u;
        ospeed |= 0x03FF0000u;
        *gpioc_ospeedr = ospeed;
        *gpioc_pupdr &= ~0x03FF0000u;

        uint32_t d_moder = *gpiod_moder;
        d_moder &= ~0x00000030u;
        d_moder |= 0x00000010u;
        *gpiod_moder = d_moder;
        *gpiod_otyper &= ~0x00000004u;
        uint32_t d_speed = *gpiod_ospeedr;
        d_speed &= ~0x00000030u;
        d_speed |= 0x00000030u;
        *gpiod_ospeedr = d_speed;
        *gpiod_pupdr &= ~0x00000030u;

        uint32_t mask_c = (0x1Fu << 8);
        uint32_t mask_d = (1u << 2);
        for (uint32_t i = 0; i < cycles; i++)
        {
                *gpioc_odr ^= mask_c;
                *gpiod_odr ^= mask_d;
                for (volatile uint32_t d = 0; d < 20000u; d++)
                {
                }
        }

        pc8pc12_to_sdmmc1();
        pd2_to_sdmmc1();
}

static uint32_t align_up_u32(uint32_t n, uint32_t a)
{
        if (a == 0u)
                return n;
        return (n + (a - 1u)) & ~(a - 1u);
}

static uint32_t u32_max(uint32_t a, uint32_t b)
{
        return (a > b) ? a : b;
}

static uint32_t checksum_words(const uint32_t *words, uint32_t count)
{
        uint32_t h = 2166136261u;
        for (uint32_t i = 0u; i < count; i++)
        {
                h ^= words[i];
                h *= 16777619u;
        }
        return h ^ 0xA5A55A5Au;
}

static void copy_words(uint32_t *dst, const uint32_t *src, uint32_t words)
{
        for (uint32_t i = 0u; i < words; i++)
                dst[i] = src[i];
}

static uint32_t checksum_bytes_update(uint32_t h, const uint8_t *data, uint32_t len)
{
        for (uint32_t i = 0u; i < len; i++)
        {
                h ^= data[i];
                h *= 16777619u;
        }
        return h;
}

static uint32_t checksum_bytes_finalize(uint32_t h)
{
        return h ^ 0xA5A55A5Au;
}

static int boot_read_block_retry(uint32_t lba, void *buf)
{
        int rc = sd_read_blocks(lba, 1u, buf);
        if (rc == SD_OK)
                return SD_OK;

        rc = sd_init();
        if (rc != SD_OK)
                return rc;
        return sd_read_blocks(lba, 1u, buf);
}

static uint32_t *boot_restore_buf(void)
{
        return sd_restore_words;
}

static int boot_snapshot_layout_compute(uint32_t total_blocks,
                                        uint32_t slots,
                                        uint32_t *journal_start_lba,
                                        uint32_t *journal_blocks_out,
                                        uint32_t *snap_start_lba,
                                        uint32_t *snap_blocks,
                                        uint32_t *slot_blocks)
{
        if (total_blocks == 0u || slots == 0u)
                return 0;

        uint32_t target_snap = (total_blocks / 100u) * SNAP_PART_PCT;
        target_snap += ((total_blocks % 100u) * SNAP_PART_PCT) / 100u;
        uint32_t target_journal = (total_blocks / 100u) * JOURNAL_PART_PCT;
        target_journal += ((total_blocks % 100u) * JOURNAL_PART_PCT) / 100u;

        uint32_t admin_blocks = u32_max(ADMIN_BLOCKS_MIN, align_up_u32(total_blocks / 4096u, 32u));
        uint32_t journal_blocks = u32_max(ADMIN_BLOCKS_MIN, align_up_u32(target_journal, 32u));
        uint32_t used = admin_blocks + journal_blocks;
        if (total_blocks <= used + MIN_FS_BLOCKS)
                return 0;

        uint32_t max_snap = total_blocks - used - MIN_FS_BLOCKS;
        uint32_t snap = target_snap;
        if (snap < MIN_SNAP_BLOCKS)
                snap = MIN_SNAP_BLOCKS;
        if (snap > max_snap)
                snap = max_snap;
        if (snap < slots * SNAPSHOT_MIN_SLOT_BLOCKS)
                return 0;

        uint32_t per_slot = snap / slots;
        if (per_slot < SNAPSHOT_MIN_SLOT_BLOCKS)
                return 0;

        uint32_t real_snap = per_slot * slots;
        if (real_snap == 0u)
                return 0;

        if (journal_start_lba != 0)
                *journal_start_lba = admin_blocks;
        if (journal_blocks_out != 0)
                *journal_blocks_out = (journal_blocks > JOURNAL_BLOCKS_DEFAULT)
                                    ? JOURNAL_BLOCKS_DEFAULT : journal_blocks;
        if (snap_start_lba != 0)
                *snap_start_lba = admin_blocks + journal_blocks;
        if (snap_blocks != 0)
                *snap_blocks = real_snap;
        if (slot_blocks != 0)
                *slot_blocks = per_slot;
        return 1;
}

static int boot_snapshot_layout_valid(uint32_t total_blocks,
                                      uint32_t slots,
                                      uint32_t snap_start_lba,
                                      uint32_t snap_blocks,
                                      uint32_t slot_blocks)
{
        if (total_blocks == 0u || slots == 0u)
                return 0;
        if (slot_blocks < SNAPSHOT_MIN_SLOT_BLOCKS)
                return 0;
        if (snap_blocks < slots * slot_blocks)
                return 0;
        if (snap_start_lba < ADMIN_BLOCKS_MIN)
                return 0;
        if (snap_start_lba + snap_blocks > total_blocks)
                return 0;
        return 1;
}

static void boot_snapshot_summary_reset(boot_status_t *st)
{
        st->snapshot_valid_count = 0u;
        for (uint32_t i = 0u; i < BOOT_SNAPSHOT_MENU_COUNT; i++)
        {
                st->snapshot_slot_idx[i] = 0xFFFFFFFFu;
                st->snapshot_slot_seq[i] = 0u;
        }
}

static void boot_snapshot_summary_insert(boot_status_t *st, uint32_t slot_idx, uint32_t seq)
{
        st->snapshot_valid_count++;
        for (uint32_t i = 0u; i < BOOT_SNAPSHOT_MENU_COUNT; i++)
        {
                if (st->snapshot_slot_idx[i] == 0xFFFFFFFFu || seq > st->snapshot_slot_seq[i])
                {
                        for (uint32_t j = BOOT_SNAPSHOT_MENU_COUNT - 1u; j > i; j--)
                        {
                                st->snapshot_slot_idx[j] = st->snapshot_slot_idx[j - 1u];
                                st->snapshot_slot_seq[j] = st->snapshot_slot_seq[j - 1u];
                        }
                        st->snapshot_slot_idx[i] = slot_idx;
                        st->snapshot_slot_seq[i] = seq;
                        return;
                }
        }
}

static int boot_snapshot_slot_is_valid(uint32_t slot_lba,
                                       uint32_t slot_blocks,
                                       uint32_t expected_slot_idx,
                                       uint32_t *seq_out)
{
        int rc = sd_read_blocks(slot_lba, 1u, sd_buf_words);
        if (rc != SD_OK)
                return 0;

        const boot_snapshot_hdr_t *hdr = (const boot_snapshot_hdr_t *)sd_buf_words;
        if (hdr->magic != SNAPSHOT_HDR_MAGIC || hdr->version != SNAPSHOT_FMT_VERSION)
                return 0;
        if (hdr->slot_index != expected_slot_idx)
                return 0;
        if (hdr->payload_lba != (slot_lba + 1u))
                return 0;
        if (hdr->payload_blocks != (slot_blocks - 2u))
                return 0;
        if (hdr->checksum != checksum_words((const uint32_t *)hdr, SNAPSHOT_HDR_WORDS))
                return 0;
        uint32_t hdr_seq = hdr->seq;

        rc = boot_read_block_retry(slot_lba + 1u, sd_buf_words);
        if (rc != SD_OK)
                return 0;
        const boot_snapshot_payload_start_t *pay0 = (const boot_snapshot_payload_start_t *)sd_buf_words;
        if (pay0->magic != SNAPSHOT_PAY0_MAGIC || pay0->version != SNAPSHOT_FMT_VERSION)
                return 0;
        if (pay0->checksum != checksum_words((const uint32_t *)pay0, SNAPSHOT_PAY0_WORDS))
                return 0;

        rc = boot_read_block_retry(slot_lba + slot_blocks - 1u, sd_buf_words);
        if (rc != SD_OK)
                return 0;
        const boot_snapshot_commit_t *c = (const boot_snapshot_commit_t *)sd_buf_words;
        if (c->magic != SNAPSHOT_CMIT_MAGIC || c->version != SNAPSHOT_FMT_VERSION)
                return 0;
        if (c->slot_index != expected_slot_idx || c->seq != hdr_seq)
                return 0;
        if (c->checksum != checksum_words((const uint32_t *)c, SNAPSHOT_CMIT_WORDS))
                return 0;

        if (seq_out != 0)
                *seq_out = hdr_seq;
        return 1;
}

static void boot_snapshot_scan(boot_status_t *st)
{
        boot_snapshot_summary_reset(st);
        if (!st->has_admin_magic || !st->sd_present || !st->sd_init_ok)
                return;
        if (!boot_snapshot_layout_valid(st->capacity_blocks,
                                        st->snapshot_slots,
                                        st->snapshot_start_lba,
                                        st->snapshot_blocks,
                                        st->snapshot_slot_blocks))
                return;

        for (uint32_t i = 0u; i < st->snapshot_slots; i++)
        {
                uint32_t slot_lba = st->snapshot_start_lba + (i * st->snapshot_slot_blocks);
                uint32_t seq = 0u;
                if (boot_snapshot_slot_is_valid(slot_lba, st->snapshot_slot_blocks, i, &seq))
                        boot_snapshot_summary_insert(st, i, seq);
        }
}

static int boot_snapshot_rank_to_slot(const boot_status_t *st, uint32_t rank, uint32_t *slot_idx, uint32_t *seq)
{
        if (rank >= BOOT_SNAPSHOT_MENU_COUNT)
                return 0;
        if (st->snapshot_slot_idx[rank] == 0xFFFFFFFFu)
                return 0;
        if (slot_idx != 0)
                *slot_idx = st->snapshot_slot_idx[rank];
        if (seq != 0)
                *seq = st->snapshot_slot_seq[rank];
        return 1;
}

static void boot_switch_to_restore_stack(void)
{
        uintptr_t top = (uintptr_t)&_sram_end;
        uintptr_t low = (top > BOOT_RESTORE_STACK_WINDOW_BYTES) ? (top - BOOT_RESTORE_STACK_WINDOW_BYTES) : (uintptr_t)&_sram_start;
        uintptr_t sp = (top > BOOT_RESTORE_STACK_GUARD_BYTES) ? (top - BOOT_RESTORE_STACK_GUARD_BYTES) : top;
        if (sp < low + 128u)
                sp = top;
        sp &= ~(uintptr_t)7u; /* ARM EABI stack alignment */
        __asm volatile("msr msp, %0" : : "r"(sp) : "memory");
}

static int boot_snapshot_read_header(uint32_t slot_lba, uint32_t slot_blocks, uint32_t expected_slot_idx,
                                     boot_snapshot_hdr_t *hdr_out, uint32_t *seq_out)
{
        uint32_t *rb = boot_restore_buf();
        int rc = boot_read_block_retry(slot_lba, rb);
        if (rc != SD_OK)
                return rc;

        const boot_snapshot_hdr_t *hdr = (const boot_snapshot_hdr_t *)rb;
        if (hdr->magic != SNAPSHOT_HDR_MAGIC || hdr->version != SNAPSHOT_FMT_VERSION)
                return SD_ERR_PARAM;
        if (hdr->slot_index != expected_slot_idx)
                return SD_ERR_PARAM;
        if (hdr->payload_lba != (slot_lba + 1u) || hdr->payload_blocks != (slot_blocks - 2u))
                return SD_ERR_PARAM;
        if (hdr->checksum != checksum_words((const uint32_t *)hdr, SNAPSHOT_HDR_WORDS))
                return SD_ERR_PARAM;
        uint32_t hdr_seq = hdr->seq;
        if (hdr_out != 0)
                copy_words((uint32_t *)hdr_out,
                           (const uint32_t *)hdr,
                           (uint32_t)(sizeof(boot_snapshot_hdr_t) / sizeof(uint32_t)));
        if (seq_out != 0)
                *seq_out = hdr_seq;

        rc = boot_read_block_retry(slot_lba + slot_blocks - 1u, rb);
        if (rc != SD_OK)
                return rc;
        const boot_snapshot_commit_t *c = (const boot_snapshot_commit_t *)rb;
        if (c->magic != SNAPSHOT_CMIT_MAGIC || c->version != SNAPSHOT_FMT_VERSION)
                return SD_ERR_PARAM;
        if (c->slot_index != expected_slot_idx || c->seq != hdr_seq)
                return SD_ERR_PARAM;
        if (c->checksum != checksum_words((const uint32_t *)c, SNAPSHOT_CMIT_WORDS))
                return SD_ERR_PARAM;
        return SD_OK;
}

static int boot_snapshot_restore_payload(uint32_t slot_lba, uint32_t slot_blocks, uint32_t slot_idx, uint32_t expected_seq)
{
        boot_snapshot_hdr_t hdr;
        uint32_t *rb = boot_restore_buf();
        uint32_t seq = 0u;
        int rc = boot_snapshot_read_header(slot_lba, slot_blocks, slot_idx, &hdr, &seq);
        if (rc != SD_OK) {
                uart_puts("R:hdr rc="); uart_put_s32(rc); uart_puts("\r\n");
                return rc;
        }
        if (expected_seq != 0u && seq != expected_seq) {
                uart_puts("R:seq exp="); uart_put_u32(expected_seq);
                uart_puts(" got="); uart_put_u32(seq); uart_puts("\r\n");
                return SD_ERR_PARAM;
        }

        uint32_t lba = hdr.payload_lba;
        uint32_t remaining_blocks = hdr.payload_blocks;
        if (remaining_blocks == 0u) {
                uart_puts("R:0blk\r\n");
                return SD_ERR_PARAM;
        }

        rc = boot_read_block_retry(lba, rb);
        if (rc != SD_OK) {
                uart_puts("R:pay0 rd rc="); uart_put_s32(rc); uart_puts("\r\n");
                return rc;
        }
        const boot_snapshot_payload_start_t *pay0 = (const boot_snapshot_payload_start_t *)rb;
        if (pay0->magic != SNAPSHOT_PAY0_MAGIC || pay0->version != SNAPSHOT_FMT_VERSION) {
                uart_puts("R:pay0 m="); uart_put_hex32(pay0->magic); uart_puts("\r\n");
                return SD_ERR_PARAM;
        }
        if (pay0->checksum != checksum_words((const uint32_t *)pay0, SNAPSHOT_PAY0_WORDS)) {
                uart_puts("R:pay0 cksum\r\n");
                return SD_ERR_PARAM;
        }
        if (pay0->region_count == 0u || pay0->region_count > 8u) {
                uart_puts("R:pay0 rgn="); uart_put_u32(pay0->region_count); uart_puts("\r\n");
                return SD_ERR_PARAM;
        }
        uint32_t saved_region_count = pay0->region_count;
        lba++;
        remaining_blocks--;

        uint32_t copied_regions = 0u;
        uint32_t copied_bytes = 0u;
        const uintptr_t sram_start = (uintptr_t)&_sram_start;
        const uintptr_t sram_end = (uintptr_t)&_sram_end;

        while (copied_regions < saved_region_count)
        {
                if (remaining_blocks == 0u) {
                        uart_puts("R:rgs noblk\r\n");
                        return SD_ERR_PARAM;
                }
                rc = boot_read_block_retry(lba, rb);
                if (rc != SD_OK)
                        return rc;
                const boot_snapshot_region_start_t *rgs = (const boot_snapshot_region_start_t *)rb;
                if (rgs->magic != SNAPSHOT_RGS0_MAGIC || rgs->version != SNAPSHOT_FMT_VERSION) {
                        uart_puts("R:rgs m="); uart_put_hex32(rgs->magic); uart_puts("\r\n");
                        return SD_ERR_PARAM;
                }
                if (rgs->checksum != checksum_words((const uint32_t *)rgs, SNAPSHOT_RGS0_WORDS)) {
                        uart_puts("R:rgs cksum\r\n");
                        return SD_ERR_PARAM;
                }
                /* Save values before rb is reused */
                uint32_t saved_region_addr = rgs->region_addr;
                uint32_t saved_region_len = rgs->region_len;
                lba++;
                remaining_blocks--;

                uintptr_t dst_addr = (uintptr_t)saved_region_addr;
                uint32_t rem = saved_region_len;
                if (dst_addr < sram_start || dst_addr > sram_end) {
                        uart_puts("R:rgn addr="); uart_put_hex32(saved_region_addr); uart_puts("\r\n");
                        return SD_ERR_PARAM;
                }
                if (rem > (uint32_t)(sram_end - dst_addr)) {
                        uart_puts("R:rgn len="); uart_put_u32(rem); uart_puts("\r\n");
                        return SD_ERR_PARAM;
                }

                uint8_t *dst = (uint8_t *)dst_addr;
                uint32_t crc = 2166136261u;
                while (rem > 0u)
                {
                        if (remaining_blocks == 0u) {
                                uart_puts("R:data noblk\r\n");
                                return SD_ERR_PARAM;
                        }
                        rc = boot_read_block_retry(lba, rb);
                        if (rc != SD_OK)
                                return rc;
                        const uint8_t *src = (const uint8_t *)rb;
                        uint32_t n = (rem > SD_BLOCK_SIZE) ? SD_BLOCK_SIZE : rem;
                        crc = checksum_bytes_update(crc, src, n);
                        uintptr_t src_addr = (uintptr_t)src;
                        uintptr_t dst_addr_cur = (uintptr_t)dst;
                        if ((dst_addr_cur > src_addr) && (dst_addr_cur < (src_addr + n)))
                        {
                                for (uint32_t i = n; i > 0u; i--)
                                        dst[i - 1u] = src[i - 1u];
                        }
                        else
                        {
                                for (uint32_t i = 0u; i < n; i++)
                                        dst[i] = src[i];
                        }
                        dst += n;
                        rem -= n;
                        lba++;
                        remaining_blocks--;
                }

                if (remaining_blocks == 0u) {
                        uart_puts("R:rge noblk\r\n");
                        return SD_ERR_PARAM;
                }
                rc = boot_read_block_retry(lba, rb);
                if (rc != SD_OK)
                        return rc;
                const boot_snapshot_region_end_t *rge = (const boot_snapshot_region_end_t *)rb;
                if (rge->magic != SNAPSHOT_RGE0_MAGIC || rge->version != SNAPSHOT_FMT_VERSION) {
                        uart_puts("R:rge m="); uart_put_hex32(rge->magic); uart_puts("\r\n");
                        return SD_ERR_PARAM;
                }
                if (rge->region_addr != saved_region_addr || rge->region_len != saved_region_len) {
                        uart_puts("R:rge addr/len\r\n");
                        return SD_ERR_PARAM;
                }
                if (rge->checksum != checksum_words((const uint32_t *)rge, SNAPSHOT_RGE0_WORDS)) {
                        uart_puts("R:rge cksum\r\n");
                        return SD_ERR_PARAM;
                }
                if (rge->region_crc != checksum_bytes_finalize(crc)) {
                        uart_puts("R:rge crc\r\n");
                        return SD_ERR_PARAM;
                }
                lba++;
                remaining_blocks--;

                copied_regions++;
                copied_bytes += saved_region_len;
        }

        if (remaining_blocks == 0u) {
                uart_puts("R:paye noblk\r\n");
                return SD_ERR_PARAM;
        }
        rc = boot_read_block_retry(lba, rb);
        if (rc != SD_OK)
                return rc;
        const boot_snapshot_payload_end_t *paye = (const boot_snapshot_payload_end_t *)rb;
        if (paye->magic != SNAPSHOT_PAYE_MAGIC || paye->version != SNAPSHOT_FMT_VERSION) {
                uart_puts("R:paye m="); uart_put_hex32(paye->magic); uart_puts("\r\n");
                return SD_ERR_PARAM;
        }
        if (paye->checksum != checksum_words((const uint32_t *)paye, SNAPSHOT_PAYE_WORDS)) {
                uart_puts("R:paye cksum\r\n");
                return SD_ERR_PARAM;
        }
        if (paye->region_count != copied_regions || paye->total_region_bytes != copied_bytes) {
                uart_puts("R:paye count\r\n");
                return SD_ERR_PARAM;
        }

        return SD_OK;
}

static int boot_select_restore_candidate_from_admin(boot_status_t *st,
                                                    const boot_admin_block_t *adm,
                                                    uint32_t *slot_idx,
                                                    uint32_t *seq)
{
        uint32_t mode = adm->reserved[ADMIN_RSV_BOOT_TARGET_MODE];
        if (mode == BOOT_TARGET_FRESH)
                return BOOT_ERR_NOT_FOUND;

        uint32_t slot_blocks = adm->reserved[ADMIN_RSV_SNAP_SLOT_BLOCKS];
        uint32_t snap_start = adm->reserved[ADMIN_RSV_SNAP_START_LBA];

        if (mode == BOOT_TARGET_SNAPSHOT)
        {
                uint32_t req_slot = adm->reserved[ADMIN_RSV_BOOT_TARGET_SLOT];
                uint32_t req_seq = adm->reserved[ADMIN_RSV_BOOT_TARGET_SEQ];
                if (req_slot < adm->snapshot_slots)
                {
                        uint32_t found_seq = 0u;
                        if (boot_snapshot_slot_is_valid(snap_start + (req_slot * slot_blocks), slot_blocks, req_slot, &found_seq))
                        {
                                if (req_seq == 0u || req_seq == found_seq)
                                {
                                        if (slot_idx != 0)
                                                *slot_idx = req_slot;
                                        if (seq != 0)
                                                *seq = found_seq;
                                        return SD_OK;
                                }
                        }
                }
        }

        boot_snapshot_scan(st);
        if (!boot_snapshot_rank_to_slot(st, 0u, slot_idx, seq))
                return BOOT_ERR_NOT_FOUND;
        return SD_OK;
}

static int boot_admin_update_target(boot_status_t *st, uint32_t mode, uint32_t slot_idx, uint32_t seq);

static int boot_restore_from_admin_target(boot_status_t *st, uint32_t *slot_out, uint32_t *seq_out)
{
        if (!st->has_admin_magic)
                return SD_ERR_NO_INIT;
        if (!boot_ensure_sd_ready(st))
                return (st->sd_init_err != 0) ? st->sd_init_err : SD_ERR_NO_INIT;

        boot_admin_block_t adm;
        if (!boot_admin_read_best(&adm))
                return SD_ERR_NO_INIT;
        if (!boot_snapshot_layout_valid(st->capacity_blocks,
                                        adm.snapshot_slots,
                                        adm.reserved[ADMIN_RSV_SNAP_START_LBA],
                                        adm.reserved[ADMIN_RSV_SNAP_BLOCKS],
                                        adm.reserved[ADMIN_RSV_SNAP_SLOT_BLOCKS]))
                return SD_ERR_PARAM;

        st->snapshot_slots = adm.snapshot_slots;
        st->snapshot_start_lba = adm.reserved[ADMIN_RSV_SNAP_START_LBA];
        st->snapshot_blocks = adm.reserved[ADMIN_RSV_SNAP_BLOCKS];
        st->snapshot_slot_blocks = adm.reserved[ADMIN_RSV_SNAP_SLOT_BLOCKS];

        uint32_t slot_idx = 0u;
        uint32_t seq = 0u;
        int rc = boot_select_restore_candidate_from_admin(st, &adm, &slot_idx, &seq);
        if (rc != SD_OK)
                return rc;

        boot_switch_to_restore_stack();
        rc = boot_snapshot_restore_payload(st->snapshot_start_lba + (slot_idx * st->snapshot_slot_blocks),
                                           st->snapshot_slot_blocks,
                                           slot_idx,
                                           seq);
        if (rc != SD_OK)
                return rc;

        /* Re-read the admin block from SD AFTER the restore completes.
         * All locals (including 'adm' and any saved_ copies) lived on the
         * stack, which is inside the SRAM restore region.  At -O0 the
         * compiler spills everything, so the snapshot data overwrites
         * them.  The SD card itself is unaffected by the SRAM restore,
         * so a fresh read is the only reliable source of truth.          */
        boot_admin_block_t adm2;
        if (!boot_admin_read_best(&adm2))
                return SD_ERR_NO_INIT;

        rewind_handoff_mark_resume(slot_idx, seq,
                                   adm2.reserved[ADMIN_RSV_JRN_START_LBA],
                                   adm2.reserved[ADMIN_RSV_JRN_BLOCKS]);

        /* Consume one-shot explicit snapshot target after successful restore.
         * "recent" remains sticky, but a pinned slot target should not persist
         * forever after it has been used once. */
        if (adm2.reserved[ADMIN_RSV_BOOT_TARGET_MODE] == BOOT_TARGET_SNAPSHOT)
                (void)boot_admin_update_target(st, BOOT_TARGET_RECENT, 0u, 0u);

        if (slot_out != 0)
                *slot_out = slot_idx;
        if (seq_out != 0)
                *seq_out = seq;
        return SD_OK;
}

static int boot_admin_write_raw(const boot_admin_block_t *adm)
{
        uint32_t block_words[SD_BLOCK_SIZE / 4u];
        for (uint32_t i = 0u; i < (SD_BLOCK_SIZE / 4u); i++)
                block_words[i] = 0u;

        const uint32_t *src = (const uint32_t *)adm;
        for (uint32_t i = 0u; i < (sizeof(boot_admin_block_t) / sizeof(uint32_t)); i++)
                block_words[i] = src[i];

        int rc = sd_write_blocks(ADMIN_PROBE_LBA, 1u, block_words);
        if (rc != SD_OK)
                return rc;
        rc = sd_write_blocks(ADMIN_MIRROR_LBA, 1u, block_words);
        if (rc != SD_OK)
                return rc;
        return SD_OK;
}

static int boot_admin_update_target(boot_status_t *st, uint32_t mode, uint32_t slot_idx, uint32_t seq)
{
        boot_admin_block_t adm;
        if (!boot_admin_read_best(&adm))
                return SD_ERR_NO_INIT;
        if (!boot_ensure_sd_ready(st))
                return (st->sd_init_err != 0) ? st->sd_init_err : SD_ERR_NO_INIT;

        if (adm.generation != 0xFFFFFFFFu)
                adm.generation += 1u;
        adm.reserved[ADMIN_RSV_BOOT_TARGET_MODE] = mode;
        adm.reserved[ADMIN_RSV_BOOT_TARGET_SLOT] = slot_idx;
        adm.reserved[ADMIN_RSV_BOOT_TARGET_SEQ] = seq;
        return boot_admin_write_raw(&adm);
}

static int app_image_present(void)
{
        const uint32_t ram_lo = (uint32_t)(uintptr_t)&_sram_start;
        const uint32_t ram_hi = (uint32_t)(uintptr_t)&_sram_end;
        const uint32_t flash_hi = 0x08100000u;
        uint32_t sp = *(volatile const uint32_t *)APP_BASE;
        uint32_t rv = *(volatile const uint32_t *)(APP_BASE + 4u);
        uint32_t rv_addr = rv & ~1u; /* clear Thumb bit for range check */

        /* Linker uses _estack == _sram_end as initial MSP, so top bound must be inclusive. */
        if (sp < ram_lo || sp > ram_hi)
                return 0;
        if ((rv & 1u) == 0u)
                return 0;
        if (rv_addr < APP_BASE || rv_addr >= flash_hi)
                return 0;
        return 1;
}

static void boot_status_init(boot_status_t *st)
{
        st->app_present = app_image_present();
        st->sd_present = 0;
        st->sd_init_ok = 0;
        st->sd_init_err = 0;
        st->has_mbr_signature = 0;
        st->has_admin_magic = 0;
        st->capacity_blocks = 0u;
        st->capacity_mb = 0u;
        st->fs_free_mb_est = 0u;
        st->snapshot_slots = SNAPSHOT_SLOTS_DEFAULT;
        st->snapshot_interval_s = SNAPSHOT_INTERVAL_DEFAULT_S;
        st->snapshot_start_lba = 0u;
        st->snapshot_blocks = 0u;
        st->snapshot_slot_blocks = 0u;
        st->boot_target_mode = BOOT_TARGET_RECENT;
        st->boot_target_slot = 0u;
        st->boot_target_seq = 0u;
        boot_snapshot_summary_reset(st);
}

static void boot_admin_apply_defaults(boot_status_t *st)
{
        if (st->snapshot_slots < SNAPSHOT_SLOTS_MIN || st->snapshot_slots > SNAPSHOT_SLOTS_MAX)
                st->snapshot_slots = SNAPSHOT_SLOTS_DEFAULT;
        if (st->snapshot_interval_s < SNAPSHOT_INTERVAL_MIN_S || st->snapshot_interval_s > SNAPSHOT_INTERVAL_MAX_S)
                st->snapshot_interval_s = SNAPSHOT_INTERVAL_DEFAULT_S;
        if (st->boot_target_mode > BOOT_TARGET_SNAPSHOT)
                st->boot_target_mode = BOOT_TARGET_RECENT;
}

static int boot_admin_parse_block(const uint8_t *blk, boot_admin_block_t *out)
{
        const boot_admin_block_t *adm = (const boot_admin_block_t *)blk;
        if (adm->magic == ADMIN_MAGIC && adm->version == ADMIN_VERSION)
        {
                if (out != 0)
                        copy_words((uint32_t *)out,
                                   (const uint32_t *)adm,
                                   (uint32_t)(sizeof(boot_admin_block_t) / sizeof(uint32_t)));
                return 1;
        }

        /* Recognise the main firmware's RWOS_DISK_MAGIC ("RWDI") table and
         * synthesize a compatible boot_admin_block_t from it so the restore
         * path, update-target path, and format detection all work after the
         * main image has written its own disk table at LBA 1-2.
         * rwos_disk_t word layout: 0=magic, 1=version, 2=generation,
         * 3=total_blocks, 4=journal_start, 5=journal_blocks,
         * 6=snap_start, 7=snap_blocks, 8=snap_slots, 9=snap_slot_blocks,
         * 10=snap_interval_s, 11=rwfs_start, 12=rwfs_blocks,
         * 13=boot_target_mode, 14=boot_target_slot, 15=boot_target_seq,
         * 16=last_snap_slot, 17=last_snap_seq */
        const uint32_t *w = (const uint32_t *)blk;
        if (w[0] == 0x49445752u && w[1] == 1u) /* "RWDI" v1 */
        {
                if (out != 0)
                {
                        for (uint32_t i = 0u; i < (sizeof(boot_admin_block_t) / sizeof(uint32_t)); i++)
                                ((uint32_t *)out)[i] = 0u;
                        out->magic = ADMIN_MAGIC;       /* pretend it's native */
                        out->version = ADMIN_VERSION;
                        out->generation = w[2];
                        out->total_blocks = w[3];
                        out->snapshot_slots = w[8];
                        out->snapshot_interval_s = w[10];
                        out->reserved[ADMIN_RSV_SNAP_START_LBA] = w[6];
                        out->reserved[ADMIN_RSV_SNAP_BLOCKS] = w[7];
                        out->reserved[ADMIN_RSV_SNAP_SLOT_BLOCKS] = w[9];
                        out->reserved[ADMIN_RSV_BOOT_TARGET_MODE] = w[13];
                        out->reserved[ADMIN_RSV_BOOT_TARGET_SLOT] = w[14];
                        out->reserved[ADMIN_RSV_BOOT_TARGET_SEQ] = w[15];
                        out->reserved[ADMIN_RSV_LAST_SNAP_SLOT] = w[16];
                        out->reserved[ADMIN_RSV_LAST_SNAP_SEQ] = w[17];
                        out->reserved[ADMIN_RSV_JRN_START_LBA] = w[4];
                        out->reserved[ADMIN_RSV_JRN_BLOCKS] = w[5];
                }
                return 1;
        }

        return 0;
}

static int boot_admin_read_best(boot_admin_block_t *out)
{
        boot_admin_block_t cand;
        boot_admin_block_t best;
        int found = 0;

        int rc = sd_read_blocks(ADMIN_PROBE_LBA, 1u, sd_buf_words);
        if (rc == SD_OK && boot_admin_parse_block((const uint8_t *)sd_buf_words, &cand))
        {
                copy_words((uint32_t *)&best,
                           (const uint32_t *)&cand,
                           (uint32_t)(sizeof(boot_admin_block_t) / sizeof(uint32_t)));
                found = 1;
        }

        rc = sd_read_blocks(ADMIN_MIRROR_LBA, 1u, sd_buf_words);
        if (rc == SD_OK && boot_admin_parse_block((const uint8_t *)sd_buf_words, &cand))
        {
                if (!found || cand.generation >= best.generation)
                {
                        copy_words((uint32_t *)&best,
                                   (const uint32_t *)&cand,
                                   (uint32_t)(sizeof(boot_admin_block_t) / sizeof(uint32_t)));
                        found = 1;
                }
        }

        if (found && out != 0)
                copy_words((uint32_t *)out,
                           (const uint32_t *)&best,
                           (uint32_t)(sizeof(boot_admin_block_t) / sizeof(uint32_t)));
        return found;
}

static void boot_probe_storage_markers(boot_status_t *st)
{
        int rc = sd_read_blocks(0u, 1u, sd_buf_words);
        if (rc == SD_OK)
        {
                const uint8_t *blk0 = (const uint8_t *)sd_buf_words;
                if (blk0[510] == 0x55u && blk0[511] == 0xAAu)
                        st->has_mbr_signature = 1;
        }

        boot_admin_block_t adm;
        if (boot_admin_read_best(&adm))
        {
                st->has_admin_magic = 1;
                st->snapshot_slots = adm.snapshot_slots;
                st->snapshot_interval_s = adm.snapshot_interval_s;
                st->boot_target_mode = adm.reserved[ADMIN_RSV_BOOT_TARGET_MODE];
                st->boot_target_slot = adm.reserved[ADMIN_RSV_BOOT_TARGET_SLOT];
                st->boot_target_seq = adm.reserved[ADMIN_RSV_BOOT_TARGET_SEQ];
        }

        boot_admin_apply_defaults(st);

        if (st->sd_init_ok)
        {
                uint32_t start = 0u;
                uint32_t blocks = 0u;
                uint32_t per_slot = 0u;
                int have_layout = 0;

                if (st->has_admin_magic)
                {
                        start = adm.reserved[ADMIN_RSV_SNAP_START_LBA];
                        blocks = adm.reserved[ADMIN_RSV_SNAP_BLOCKS];
                        per_slot = adm.reserved[ADMIN_RSV_SNAP_SLOT_BLOCKS];
                        have_layout = boot_snapshot_layout_valid(st->capacity_blocks,
                                                                 st->snapshot_slots,
                                                                 start,
                                                                 blocks,
                                                                 per_slot);
                }

                if (!have_layout)
                {
                        uint32_t journal_start = 0u;
                        uint32_t journal_blocks = 0u;
                        have_layout = boot_snapshot_layout_compute(st->capacity_blocks,
                                                                   st->snapshot_slots,
                                                                   &journal_start,
                                                                   &journal_blocks,
                                                                   &start,
                                                                   &blocks,
                                                                   &per_slot);
                }

                if (have_layout)
                {
                        st->snapshot_start_lba = start;
                        st->snapshot_blocks = blocks;
                        st->snapshot_slot_blocks = per_slot;
                }
        }
}

static uint32_t boot_estimate_fs_blocks(uint32_t total_blocks)
{
        uint32_t snap_blocks = (total_blocks / 100u) * SNAP_PART_PCT;
        snap_blocks += ((total_blocks % 100u) * SNAP_PART_PCT) / 100u;
        uint32_t journal_blocks = (total_blocks / 100u) * JOURNAL_PART_PCT;
        journal_blocks += ((total_blocks % 100u) * JOURNAL_PART_PCT) / 100u;
        uint32_t admin_blocks = u32_max(ADMIN_BLOCKS_MIN, align_up_u32(total_blocks / 4096u, 32u));
        uint32_t jrn_blocks = u32_max(ADMIN_BLOCKS_MIN, align_up_u32((uint32_t)journal_blocks, 32u));
        uint32_t snap = snap_blocks;
        if (snap < MIN_SNAP_BLOCKS)
                snap = MIN_SNAP_BLOCKS;

        uint32_t used = admin_blocks + jrn_blocks;
        if (total_blocks <= used + MIN_FS_BLOCKS)
                return 0u;

        uint32_t max_snap = total_blocks - used - MIN_FS_BLOCKS;
        if (snap > max_snap)
                snap = max_snap;
        used += snap;
        if (used >= total_blocks)
                return 0u;
        return total_blocks - used;
}

static void sd_abort_stale_transfer(void)
{
        /* If the card was mid-data-transfer when the MCU reset,
         * the SDMMC DCTRL may still have DTEN set and the card
         * is waiting for clocks.  Clear the data path and send
         * dummy clocks so the card can finish / time-out.       */
        volatile uint32_t *SDMMC_DCTRL = (uint32_t *)0x40012C2Cu;
        volatile uint32_t *SDMMC_ICR   = (uint32_t *)0x40012C38u;
        volatile uint32_t *SDMMC_MASK  = (uint32_t *)0x40012C3Cu;
        *SDMMC_MASK  = 0u;
        *SDMMC_DCTRL = 0u;
        *SDMMC_ICR   = 0xFFFFFFFFu;
        /* Small delay — ~100k iterations ≈ a few ms at boot clock.
         * Gives the card time to release its busy signal.           */
        for (volatile uint32_t d = 0; d < 100000u; d++) {}
}

#define SD_INIT_RETRIES 3u

static int boot_ensure_sd_ready(boot_status_t *st)
{
        sd_detect_init();
        if (!sd_is_detected())
        {
                if (st != 0)
                {
                        st->sd_present = 0;
                        st->sd_init_ok = 0;
                        st->sd_init_err = 0;
                }
                return 0;
        }

        sd_use_pll48(0);
        sd_set_data_clkdiv(SD_CLKDIV_BOOT);
        sd_abort_stale_transfer(); /* clean up any in-flight transfer from before reset */

        int rc = SD_OK;
        for (uint32_t attempt = 0; attempt < SD_INIT_RETRIES; attempt++)
        {
                if (attempt > 0u)
                {
                        uart_puts("  sd: retry ");
                        uart_put_u32(attempt);
                        uart_puts("...\r\n");
                        sd_abort_stale_transfer();
                }
                rc = sd_init();
                if (rc == SD_OK)
                        break;
        }

        if (st != 0)
        {
                st->sd_present = 1;
                st->sd_init_ok = (rc == SD_OK) ? 1 : 0;
                st->sd_init_err = (rc == SD_OK) ? 0 : rc;
                if (rc == SD_OK)
                {
                        const sd_info_t *info = sd_get_info();
                        st->capacity_blocks = info->capacity_blocks;
                        st->capacity_mb = info->capacity_blocks / 2048u;
                        st->fs_free_mb_est = boot_estimate_fs_blocks(info->capacity_blocks) / 2048u;
                }
        }

        return (rc == SD_OK) ? 1 : 0;
}

static void boot_refresh_status(boot_status_t *st)
{
        boot_status_init(st);
        if (!boot_ensure_sd_ready(st))
                return;
        boot_probe_storage_markers(st);
        boot_snapshot_scan(st);
}

static void boot_wait_enter(void)
{
        uart_puts("\r\nPress Enter to continue...");
        for (;;)
        {
                char c = uart_getc();
                if (c == '\r')
                {
                        char n = 0;
                        if (uart_try_getc(&n) && n != '\n')
                        {
                                /* non-LF tail byte consumed; ignore */
                        }
                        break;
                }
                if (c == '\n')
                        break;
        }
        uart_puts("\r\n");
}

static void boot_system_reset(void)
{
        /* Quiesce the SD peripheral so the card is not mid-transaction
         * when the MCU resets.  The card stays powered across
         * SYSRESETREQ so it needs to be back in TRAN state. */
        volatile uint32_t *SDMMC_MASK  = (uint32_t *)0x40012C3Cu;
        volatile uint32_t *SDMMC_DCTRL = (uint32_t *)0x40012C2Cu;
        volatile uint32_t *SDMMC_ICR   = (uint32_t *)0x40012C38u;
        *SDMMC_MASK  = 0u;
        *SDMMC_DCTRL = 0u;
        *SDMMC_ICR   = 0xFFFFFFFFu;
        (void)sd_wait_card_ready();

        volatile uint32_t *aircr = (uint32_t *)0xE000ED0Cu;
        *aircr = 0x05FA0004u;
        for (;;)
        {
        }
}

static int boot_confirm_destructive_format(const boot_status_t *st)
{
        char answer[8];
        uint32_t len = 0u;

        uart_puts("\r\nWARNING: format/setup is destructive.\r\n");
        uart_puts("It will overwrite storage metadata and may destroy existing data.\r\n");
        if (st->has_mbr_signature)
        {
                uart_puts("Detected: MBR signature on LBA0 (existing data likely present).\r\n");
        }
        uart_puts("Snapshot policy to write: slots=");
        uart_put_u32(st->snapshot_slots);
        uart_puts(" interval=");
        uart_put_u32(st->snapshot_interval_s);
        uart_puts("s\r\n");
        uart_puts("Type YES then Enter to continue: ");

        for (;;)
        {
                char c = uart_getc();
                if (c == '\r' || c == '\n')
                {
                        uart_puts("\r\n");
                        break;
                }

                if ((c == '\b' || (unsigned char)c == 0x7Fu) && len > 0u)
                {
                        len--;
                        uart_putc('\b');
                        uart_putc(' ');
                        uart_putc('\b');
                        continue;
                }

                if ((unsigned char)c >= 0x20u && (unsigned char)c <= 0x7Eu)
                {
                        if (len < (sizeof(answer) - 1u))
                        {
                                answer[len++] = c;
                                uart_putc(c);
                        }
                }
        }

        answer[len] = '\0';
        if (streq(answer, "YES") || streq(answer, "yes"))
                return 1;

        uart_puts("Cancelled.\r\n");
        return 0;
}

static void boot_read_line(char *buf, uint32_t cap)
{
        uint32_t len = 0u;
        if (cap == 0u)
                return;

        for (;;)
        {
                char c = uart_getc();
                if (c == '\r' || c == '\n')
                {
                        uart_puts("\r\n");
                        break;
                }

                if ((c == '\b' || (unsigned char)c == 0x7Fu) && len > 0u)
                {
                        len--;
                        uart_putc('\b');
                        uart_putc(' ');
                        uart_putc('\b');
                        continue;
                }

                if ((unsigned char)c >= 0x20u && (unsigned char)c <= 0x7Eu)
                {
                        if (len < (cap - 1u))
                        {
                                buf[len++] = c;
                                uart_putc(c);
                        }
                }
        }
        buf[len] = '\0';
}

static int boot_prompt_u32(const char *name, uint32_t *value, uint32_t min_v, uint32_t max_v)
{
        char line[24];
        for (;;)
        {
                uart_puts(name);
                uart_puts(" [");
                uart_put_u32(*value);
                uart_puts("] (range ");
                uart_put_u32(min_v);
                uart_puts("..");
                uart_put_u32(max_v);
                uart_puts(", Enter=keep): ");

                boot_read_line(line, sizeof(line));
                if (line[0] == '\0')
                        return 1;

                uint32_t parsed = 0u;
                if (!parse_u32(line, &parsed))
                {
                        uart_puts("Invalid number.\r\n");
                        continue;
                }
                if (parsed < min_v || parsed > max_v)
                {
                        uart_puts("Out of range.\r\n");
                        continue;
                }
                *value = parsed;
                return 1;
        }
}

static void boot_render_setup(const boot_status_t *st, const boot_setup_item_t *items, unsigned int selected)
{
        uart_puts("\x1B[2J\x1B[H");
        uart_puts("Setup RewindOS\r\n");
        uart_puts("==============\r\n");
        uart_puts("RewindOS stores periodic snapshots and input journal data on SD.\r\n");
        uart_puts("This first setup initializes storage metadata used by recovery.\r\n");
        uart_puts("\r\n");

        if (!st->sd_present)
        {
            uart_puts("SD: not detected\r\n");
        }
        else if (!st->sd_init_ok)
        {
            uart_puts("SD: init failed (err=");
            uart_put_s32(st->sd_init_err);
            uart_puts(")\r\n");
        }
        else
        {
            uart_puts("SD: ");
            uart_put_u32(st->capacity_mb);
            uart_puts("MB\r\n");
        }

        uart_puts("Status: ");
        uart_puts(st->has_admin_magic ? "formatted\r\n" : "not formatted\r\n");
        uart_puts("Snapshots: slots=");
        uart_put_u32(st->snapshot_slots);
        uart_puts(" interval=");
        uart_put_u32(st->snapshot_interval_s);
        uart_puts("s\r\n");
        uart_puts("\r\n");
        uart_puts("Use Up/Down (or k/j), Enter to select.\r\n");
        uart_puts("\r\n");
        for (unsigned int i = 0u; i < SETUP_MENU_ITEM_COUNT; i++)
                boot_menu_print_line((int)(i == selected), items[i].label, items[i].enabled);
        uart_puts("\r\n");
}

static void boot_menu_print_line(int selected, const char *label, int enabled)
{
        uart_puts("   ");
        uart_puts(label);
        if (!enabled)
                uart_puts(" [disabled]");
        uart_puts(selected ? " <" : "  ");
        uart_puts("\r\n");
}

static void boot_render_menu(const boot_status_t *st, const boot_menu_item_t *items, unsigned int selected)
{
        uart_puts("\x1B[2J\x1B[H");
        uart_puts("RewindOS Bootloader\r\n");
        uart_puts("===================\r\n");

        uart_puts("App image: ");
        uart_puts(st->app_present ? "present\r\n" : "missing\r\n");

        if (!st->sd_present)
        {
                uart_puts("SD: not detected\r\n");
        }
        else if (!st->sd_init_ok)
        {
                uart_puts("SD: init failed (err=");
                uart_put_s32(st->sd_init_err);
                uart_puts(")\r\n");
        }
        else
        {
                uart_puts("SD: ");
                uart_put_u32(st->capacity_mb);
                uart_puts("MB, est free fs=");
                uart_put_u32(st->fs_free_mb_est);
                uart_puts("MB\r\n");
                uart_puts("Storage markers: admin=");
                uart_put_u32((uint32_t)st->has_admin_magic);
                uart_puts(" mbr=");
                uart_put_u32((uint32_t)st->has_mbr_signature);
                uart_puts("\r\n");
        }

        uart_puts("Setup: ");
        uart_puts(st->has_admin_magic ? "ok\r\n" : "required (not formatted)\r\n");
        uart_puts("Snapshots: valid=");
        uart_put_u32(st->snapshot_valid_count);
        uart_puts("/");
        uart_put_u32(st->snapshot_slots);
        if (st->snapshot_slot_blocks != 0u)
        {
                uart_puts(" slot_blocks=");
                uart_put_u32(st->snapshot_slot_blocks);
        }
        uart_puts("\r\n");
        if (st->snapshot_slot_idx[0] != 0xFFFFFFFFu)
        {
                uart_puts("Latest snapshot: slot=");
                uart_put_u32(st->snapshot_slot_idx[0]);
                uart_puts(" seq=");
                uart_put_u32(st->snapshot_slot_seq[0]);
                uart_puts("\r\n");
        }
        uart_puts("Target: ");
        if (st->boot_target_mode == BOOT_TARGET_FRESH)
        {
                uart_puts("fresh");
        }
        else if (st->boot_target_mode == BOOT_TARGET_SNAPSHOT)
        {
                uart_puts("snapshot slot=");
                uart_put_u32(st->boot_target_slot);
                uart_puts(" seq=");
                uart_put_u32(st->boot_target_seq);
        }
        else
        {
                uart_puts("recent");
        }
        uart_puts("\r\n");
        uart_puts("\r\n");
        uart_puts("Use Up/Down (or k/j), Enter to select.\r\n");
        uart_puts("\r\n");
        for (unsigned int i = 0u; i < BOOT_MENU_ITEM_COUNT; i++)
                boot_menu_print_line((int)(i == selected), items[i].label, items[i].enabled);
        uart_puts("\r\n");
}

static unsigned int boot_find_next_enabled(const boot_menu_item_t *items, unsigned int from, int dir)
{
        unsigned int idx = from;
        for (unsigned int i = 0u; i < BOOT_MENU_ITEM_COUNT; i++)
        {
                if (dir > 0)
                        idx = (idx + 1u) % BOOT_MENU_ITEM_COUNT;
                else if (idx == 0u)
                        idx = BOOT_MENU_ITEM_COUNT - 1u;
                else
                        idx--;

                if (items[idx].enabled)
                        return idx;
        }
        return from;
}

static unsigned int boot_setup_find_next_enabled(const boot_setup_item_t *items, unsigned int from, int dir)
{
        unsigned int idx = from;
        for (unsigned int i = 0u; i < SETUP_MENU_ITEM_COUNT; i++)
        {
                if (dir > 0)
                        idx = (idx + 1u) % SETUP_MENU_ITEM_COUNT;
                else if (idx == 0u)
                        idx = SETUP_MENU_ITEM_COUNT - 1u;
                else
                        idx--;

                if (items[idx].enabled)
                        return idx;
        }
        return from;
}

static int boot_menu_read_key(uint8_t *eol_state)
{
        for (;;)
        {
                char c = uart_getc();

                if (*eol_state == 1u)
                {
                        if (c == '\n')
                        {
                                *eol_state = 0u;
                                continue;
                        }
                        *eol_state = 0u;
                }

                if ((unsigned char)c == 0x1Bu)
                {
                        char a = uart_getc();
                        if ((unsigned char)a == '[')
                        {
                                char b = uart_getc();
                                if (b == 'A')
                                        return KEY_UP;
                                if (b == 'B')
                                        return KEY_DOWN;
                        }
                        continue;
                }

                if (c == '\r')
                {
                        *eol_state = 1u;
                        return KEY_ENTER;
                }
                if (c == '\n')
                        return KEY_ENTER;
                if (c == 'k' || c == 'w')
                        return KEY_UP;
                if (c == 'j' || c == 's')
                        return KEY_DOWN;
        }
}

static int boot_write_admin_region(boot_status_t *st)
{
        if (!boot_ensure_sd_ready(st))
        {
                if (st != 0 && st->sd_init_err != 0)
                        return st->sd_init_err;
                return SD_ERR_NO_INIT;
        }

        uint32_t snap_start_lba = 0u;
        uint32_t snap_blocks = 0u;
        uint32_t slot_blocks = 0u;
        uint32_t journal_start_lba = 0u;
        uint32_t journal_blocks = 0u;
        if (!boot_snapshot_layout_compute(st->capacity_blocks,
                                          st->snapshot_slots,
                                          &journal_start_lba,
                                          &journal_blocks,
                                          &snap_start_lba,
                                          &snap_blocks,
                                          &slot_blocks))
        {
                return SD_ERR_PARAM;
        }

        boot_admin_block_t adm;
        for (uint32_t i = 0u; i < (sizeof(adm) / sizeof(uint32_t)); i++)
                ((uint32_t *)&adm)[i] = 0u;

        uint32_t next_gen = 1u;
        boot_admin_block_t existing;
        if (boot_admin_read_best(&existing))
        {
                if (existing.generation != 0xFFFFFFFFu)
                        next_gen = existing.generation + 1u;
        }

        adm.magic = ADMIN_MAGIC;
        adm.version = ADMIN_VERSION;
        adm.generation = next_gen;
        adm.total_blocks = st->capacity_blocks;
        adm.fallback_mode = 0u; /* 0=recovery(default), 1=fresh */
        adm.snapshot_slots = st->snapshot_slots;
        adm.snapshot_interval_s = st->snapshot_interval_s;
        adm.reserved[ADMIN_RSV_JRN_START_LBA] = journal_start_lba;
        adm.reserved[ADMIN_RSV_JRN_BLOCKS] = journal_blocks;
        adm.reserved[ADMIN_RSV_SNAP_START_LBA] = snap_start_lba;
        adm.reserved[ADMIN_RSV_SNAP_BLOCKS] = snap_blocks;
        adm.reserved[ADMIN_RSV_SNAP_SLOT_BLOCKS] = slot_blocks;
        adm.reserved[ADMIN_RSV_BOOT_TARGET_MODE] = BOOT_TARGET_RECENT;
        adm.reserved[ADMIN_RSV_BOOT_TARGET_SLOT] = 0u;
        adm.reserved[ADMIN_RSV_BOOT_TARGET_SEQ] = 0u;
        adm.reserved[ADMIN_RSV_LAST_SNAP_SLOT] = 0u;
        adm.reserved[ADMIN_RSV_LAST_SNAP_SEQ] = 0u;

        int rc = boot_admin_write_raw(&adm);
        if (rc != SD_OK)
                return rc;

        /* Zero the header block of every snapshot slot so stale data from
         * a previous format is not mistaken for a valid snapshot. */
        uint32_t zero_block[SD_BLOCK_SIZE / 4u];
        for (uint32_t i = 0u; i < (SD_BLOCK_SIZE / 4u); i++)
                zero_block[i] = 0u;
        for (uint32_t s = 0u; s < adm.snapshot_slots && s < SNAPSHOT_SLOTS_MAX; s++)
        {
                uint32_t slot_lba = snap_start_lba + (s * slot_blocks);
                rc = sd_write_blocks(slot_lba, 1u, zero_block);
                if (rc != SD_OK)
                        return rc;
        }
        return SD_OK;
}

static void boot_setup_loop(int force_show)
{
        boot_status_t st;
        boot_refresh_status(&st);
        uint32_t cfg_slots = st.snapshot_slots;
        uint32_t cfg_interval = st.snapshot_interval_s;
        uint8_t eol_state = 0u;
        unsigned int selected = 0u;

        for (;;)
        {
                if (!force_show && st.has_admin_magic)
                        return;

                boot_setup_item_t items[SETUP_MENU_ITEM_COUNT];
                items[0] = (boot_setup_item_t){ "Format metadata + reboot", SETUP_ACT_FORMAT, st.sd_present && st.sd_init_ok };
                items[1] = (boot_setup_item_t){ "Configure snapshot slots/interval", SETUP_ACT_CONFIG, 1 };
                items[2] = (boot_setup_item_t){ "Refresh status", SETUP_ACT_REFRESH, 1 };
                items[3] = (boot_setup_item_t){ "Recovery shell", SETUP_ACT_RECOVERY, 1 };

                if (!items[selected].enabled)
                        selected = 1u;

                st.snapshot_slots = cfg_slots;
                st.snapshot_interval_s = cfg_interval;
                boot_render_setup(&st, items, selected);
                int key = boot_menu_read_key(&eol_state);
                if (key == KEY_UP)
                {
                        selected = boot_setup_find_next_enabled(items, selected, -1);
                        continue;
                }
                if (key == KEY_DOWN)
                {
                        selected = boot_setup_find_next_enabled(items, selected, +1);
                        continue;
                }
                if (key != KEY_ENTER || !items[selected].enabled)
                        continue;

                switch (items[selected].action)
                {
                case SETUP_ACT_FORMAT:
                {
                        st.snapshot_slots = cfg_slots;
                        st.snapshot_interval_s = cfg_interval;
                        if (!boot_confirm_destructive_format(&st))
                        {
                                boot_wait_enter();
                                break;
                        }
                        uart_puts("\r\nsetup: writing ADMIN metadata...\r\n");
                        int rc = boot_write_admin_region(&st);
                        if (rc != SD_OK)
                        {
                                uart_puts("setup: failed err=");
                                uart_put_s32(rc);
                                uart_puts(" cmd=");
                                uart_put_hex32(sd_last_cmd());
                                uart_puts(" sta=");
                                uart_put_hex32(sd_last_sta());
                                uart_puts("\r\n");
                                boot_wait_enter();
                                break;
                        }

                        boot_refresh_status(&st);
                        if (!st.has_admin_magic)
                        {
                                uart_puts("setup: marker probe failed after write\r\n");
                                boot_wait_enter();
                                break;
                        }

                        cfg_slots = st.snapshot_slots;
                        cfg_interval = st.snapshot_interval_s;
                        uart_puts("setup: complete. rebooting...\r\n");
                        uart_flush_tx();
                        boot_system_reset();
                        break;
                }
                case SETUP_ACT_CONFIG:
                        uart_puts("\r\nSetup snapshot policy\r\n");
                        if (!boot_prompt_u32("slots", &cfg_slots, SNAPSHOT_SLOTS_MIN, SNAPSHOT_SLOTS_MAX))
                                break;
                        if (!boot_prompt_u32("interval_s", &cfg_interval, SNAPSHOT_INTERVAL_MIN_S, SNAPSHOT_INTERVAL_MAX_S))
                                break;
                        uart_puts("policy: pending values saved for format\r\n");
                        boot_wait_enter();
                        break;
                case SETUP_ACT_REFRESH:
                        boot_refresh_status(&st);
                        if (st.has_admin_magic)
                        {
                                cfg_slots = st.snapshot_slots;
                                cfg_interval = st.snapshot_interval_s;
                        }
                        break;
                case SETUP_ACT_RECOVERY:
                        uart_puts("\r\nEntering recovery shell...\r\n");
                        shell_loop("recovery> ", boot_dispatch);
                        boot_refresh_status(&st);
                        if (st.has_admin_magic)
                        {
                                cfg_slots = st.snapshot_slots;
                                cfg_interval = st.snapshot_interval_s;
                        }
                        break;
                default:
                        break;
                }
        }
}

static void boot_action_boot(boot_status_t *st, const char *mode, uint32_t target_mode)
{
        if (!st->app_present)
        {
                uart_puts("\r\nboot: app image missing; cannot jump.\r\n");
                boot_wait_enter();
                return;
        }

        rewind_handoff_clear();
        if (target_mode != BOOT_TARGET_FRESH && st->has_admin_magic)
        {
                uint32_t restored_slot = 0u;
                uint32_t restored_seq = 0u;
                int rc = boot_restore_from_admin_target(st, &restored_slot, &restored_seq);
                if (rc != SD_OK)
                {
                        uart_puts("\r\nboot: restore failed err=");
                        uart_put_s32(rc);
                        uart_puts(" cmd=");
                        uart_put_hex32(sd_last_cmd());
                        uart_puts(" sta=");
                        uart_put_hex32(sd_last_sta());
                        uart_puts("\r\n");
                        boot_wait_enter();
                        return;
                }
                uart_puts("restore: slot=");
                uart_put_u32(restored_slot);
                uart_puts(" seq=");
                uart_put_u32(restored_seq);
                uart_puts("\r\n");
        }

        uart_puts("\r\nboot ");
        uart_puts(mode);
        uart_puts("...\r\n");
        uart_flush_tx();
        jump_to_image(APP_BASE);
        for (;;)
        {
        }
}

static void boot_action_boot_recent(boot_status_t *st)
{
        if (st->snapshot_slot_idx[0] == 0xFFFFFFFFu)
        {
                uart_puts("\r\nboot: no valid snapshot for 'recent'.\r\n");
                boot_wait_enter();
                return;
        }
        if (st->has_admin_magic)
        {
                int rc = boot_admin_update_target(st, BOOT_TARGET_RECENT, 0u, 0u);
                if (rc != SD_OK)
                {
                        uart_puts("\r\nboot: failed to set recent target err=");
                        uart_put_s32(rc);
                        uart_puts("\r\n");
                        boot_wait_enter();
                        return;
                }
        }
        boot_action_boot(st, "recent", BOOT_TARGET_RECENT);
}

static void boot_action_boot_fresh(boot_status_t *st)
{
        if (st->has_admin_magic)
        {
                int rc = boot_admin_update_target(st, BOOT_TARGET_FRESH, 0u, 0u);
                if (rc != SD_OK)
                {
                        uart_puts("\r\nboot: failed to set fresh target err=");
                        uart_put_s32(rc);
                        uart_puts("\r\n");
                        boot_wait_enter();
                        return;
                }
        }
        boot_action_boot(st, "fresh", BOOT_TARGET_FRESH);
}

static void boot_action_boot_snapshot_rank(boot_status_t *st, uint32_t rank)
{
        if (!st->app_present)
        {
                uart_puts("\r\nboot: app image missing; cannot jump.\r\n");
                boot_wait_enter();
                return;
        }

        uint32_t slot_idx = 0u;
        uint32_t seq = 0u;
        if (!boot_snapshot_rank_to_slot(st, rank, &slot_idx, &seq))
        {
                uart_puts("\r\nsnapshot target not available.\r\n");
                boot_wait_enter();
                return;
        }

        int rc = boot_admin_update_target(st, BOOT_TARGET_SNAPSHOT, slot_idx, seq);
        if (rc != SD_OK)
        {
                uart_puts("\r\nboot: failed to set snapshot target err=");
                uart_put_s32(rc);
                uart_puts("\r\n");
                boot_wait_enter();
                return;
        }

        boot_action_boot(st, "snapshot", BOOT_TARGET_SNAPSHOT);
}

static void boot_snapshot_list(const boot_status_t *st)
{
        if (!st->has_admin_magic)
        {
                uart_puts("snapls: storage not formatted\r\n");
                return;
        }
        if (!boot_snapshot_layout_valid(st->capacity_blocks,
                                        st->snapshot_slots,
                                        st->snapshot_start_lba,
                                        st->snapshot_blocks,
                                        st->snapshot_slot_blocks))
        {
                uart_puts("snapls: invalid snapshot geometry\r\n");
                return;
        }

        uart_puts("snapshots: slots=");
        uart_put_u32(st->snapshot_slots);
        uart_puts(" start_lba=");
        uart_put_u32(st->snapshot_start_lba);
        uart_puts(" slot_blocks=");
        uart_put_u32(st->snapshot_slot_blocks);
        uart_puts("\r\n");

        for (uint32_t i = 0u; i < st->snapshot_slots; i++)
        {
                uint32_t slot_lba = st->snapshot_start_lba + (i * st->snapshot_slot_blocks);
                uint32_t seq = 0u;
                int valid = boot_snapshot_slot_is_valid(slot_lba, st->snapshot_slot_blocks, i, &seq);
                uart_puts("  slot ");
                uart_put_u32(i);
                uart_puts(": ");
                if (valid)
                {
                        uart_puts("valid seq=");
                        uart_put_u32(seq);
                }
                else
                {
                        uart_puts("empty/invalid");
                }
                uart_puts("\r\n");
        }
}

static void boot_menu_loop(void)
{
        boot_status_t st;
        boot_refresh_status(&st);

        boot_menu_item_t items[BOOT_MENU_ITEM_COUNT];
        uint8_t eol_state = 0u;

        for (;;)
        {
                items[0] = (boot_menu_item_t){ "Boot recent", BOOT_ACT_RECENT, st.app_present && (st.snapshot_slot_idx[0] != 0xFFFFFFFFu) };
                items[1] = (boot_menu_item_t){ "Boot fresh", BOOT_ACT_FRESH, st.app_present };
                items[2] = (boot_menu_item_t){ "Snapshot 0 (latest)", BOOT_ACT_SNAP0, st.app_present && (st.snapshot_slot_idx[0] != 0xFFFFFFFFu) };
                items[3] = (boot_menu_item_t){ "Snapshot 1", BOOT_ACT_SNAP1, st.app_present && (st.snapshot_slot_idx[1] != 0xFFFFFFFFu) };
                items[4] = (boot_menu_item_t){ "Snapshot 2", BOOT_ACT_SNAP2, st.app_present && (st.snapshot_slot_idx[2] != 0xFFFFFFFFu) };
                items[5] = (boot_menu_item_t){ "Setup / format", BOOT_ACT_SETUP, 1 };
                items[6] = (boot_menu_item_t){ "Refresh status", BOOT_ACT_REFRESH, 1 };
                items[7] = (boot_menu_item_t){ "Recovery shell", BOOT_ACT_RECOVERY, 1 };
                items[8] = (boot_menu_item_t){ "Command shell", BOOT_ACT_SHELL, 1 };

                unsigned int selected = 0u;
                if (!items[selected].enabled)
                        selected = 5u;

                for (;;)
                {
                        boot_render_menu(&st, items, selected);
                        int key = boot_menu_read_key(&eol_state);
                        if (key == KEY_UP)
                        {
                                selected = boot_find_next_enabled(items, selected, -1);
                                continue;
                        }
                        if (key == KEY_DOWN)
                        {
                                selected = boot_find_next_enabled(items, selected, +1);
                                continue;
                        }
                        if (key != KEY_ENTER || !items[selected].enabled)
                                continue;

                        switch (items[selected].action)
                        {
                        case BOOT_ACT_RECENT:
                                boot_action_boot_recent(&st);
                                break;
                        case BOOT_ACT_FRESH:
                                boot_action_boot_fresh(&st);
                                break;
                        case BOOT_ACT_SNAP0:
                                boot_action_boot_snapshot_rank(&st, 0u);
                                break;
                        case BOOT_ACT_SNAP1:
                                boot_action_boot_snapshot_rank(&st, 1u);
                                break;
                        case BOOT_ACT_SNAP2:
                                boot_action_boot_snapshot_rank(&st, 2u);
                                break;
                        case BOOT_ACT_SETUP:
                                boot_setup_loop(1);
                                boot_refresh_status(&st);
                                break;
                        case BOOT_ACT_REFRESH:
                                boot_refresh_status(&st);
                                break;
                        case BOOT_ACT_RECOVERY:
                                uart_puts("\r\nEntering recovery shell...\r\n");
                                shell_loop("recovery> ", boot_dispatch);
                                break;
                        case BOOT_ACT_SHELL:
                                uart_puts("\r\nEntering command shell...\r\n");
                                shell_loop("boot> ", boot_dispatch);
                                break;
                        default:
                                break;
                        }

                        break;
                }
        }
}

static void boot_dispatch(char *line)
{
        char *argv[MAX_ARGUMENTS];
        int argc = tokenize(line, argv, MAX_ARGUMENTS);

        if (argc == 0)
                return;

        if (streq(argv[0], "help"))
        {
                uart_puts("\r\n");
                uart_puts("  Boot\r\n");
                uart_puts("    bootfast          Jump to main firmware\r\n");
                uart_puts("\r\n");
                uart_puts("  SD Card\r\n");
                uart_puts("    sdinit            Initialize SD card\r\n");
                uart_puts("    sdinfo            Show card info\r\n");
                uart_puts("    sdtest            Init + read test\r\n");
                uart_puts("    sdread <lba> [n]  Read blocks (n<=4)\r\n");
                uart_puts("    format [--yes]    Write ADMIN metadata (destructive)\r\n");
                uart_puts("    save              Disabled (use rewind> snapnow)\r\n");
                uart_puts("    snapls            List snapshot slots\r\n");
                uart_puts("    restore [0|1|2]   Boot latest or selected snapshot rank\r\n");
                uart_puts("    sddetect          Check card presence\r\n");
                uart_puts("\r\n");
                uart_puts("  SD Debug\r\n");
                uart_puts("    sdclk <sys|pll>   Set kernel clock source\r\n");
                uart_puts("    sdclkdiv <0-255>  Set clock divider\r\n");
                uart_puts("    sdstat            Dump SDMMC registers\r\n");
                uart_puts("    sdlast            Show last cmd/error\r\n");
                uart_puts("    sdtoggle <n>      Toggle SD lines\r\n");
                uart_puts("\r\n");
                uart_puts("  Debug\r\n");
                uart_puts("    gpiodump          Dump GPIO registers\r\n");
                uart_puts("    rccdump           Dump RCC registers\r\n");
                uart_puts("\r\n");
                return;
        }

        if (streq(argv[0], "bootfast"))
        {
                rewind_handoff_clear();
                uart_puts("booting...\r\n");
                uart_flush_tx();
                jump_to_image(APP_BASE);
                for (;;)
                {
                }
        }

        if (streq(argv[0], "sdinit"))
        {
                if (!sd_require_present())
                        return;
                sd_use_pll48(0);
                sd_set_data_clkdiv(SD_CLKDIV_BOOT);
                int rc = sd_init();
                if (rc == SD_OK)
                {
                        uart_puts("sdinit: ok\r\n");
                        return;
                }
                uart_puts("sdinit: err=");
                uart_put_s32(rc);
                uart_puts("\r\n");
                return;
        }

        if (streq(argv[0], "sdinfo"))
        {
                sd_print_info();
                return;
        }

        if (streq(argv[0], "format"))
        {
                boot_status_t st;
                boot_refresh_status(&st);
                if (!st.sd_present)
                {
                        uart_puts("format: SD not present\r\n");
                        return;
                }
                if (!st.sd_init_ok)
                {
                        uart_puts("format: SD init failed err=");
                        uart_put_s32(st.sd_init_err);
                        uart_puts("\r\n");
                        return;
                }
                if (argc < 2 || !streq(argv[1], "--yes"))
                {
                        if (!boot_confirm_destructive_format(&st))
                                return;
                }
                int rc = boot_write_admin_region(&st);
                if (rc != SD_OK)
                {
                        uart_puts("format: failed err=");
                        uart_put_s32(rc);
                        uart_puts(" cmd=");
                        uart_put_hex32(sd_last_cmd());
                        uart_puts(" sta=");
                        uart_put_hex32(sd_last_sta());
                        uart_puts("\r\n");
                        return;
                }
                uart_puts("format: ok (ADMIN A/B written)\r\n");
                return;
        }

        if (streq(argv[0], "save"))
        {
                uart_puts("save: disabled in bootloader (use rewind> snapnow)\r\n");
                return;
        }

        if (streq(argv[0], "snapls"))
        {
                boot_status_t st;
                boot_refresh_status(&st);
                if (!st.sd_present)
                {
                        uart_puts("snapls: SD not present\r\n");
                        return;
                }
                if (!st.sd_init_ok)
                {
                        uart_puts("snapls: SD init failed err=");
                        uart_put_s32(st.sd_init_err);
                        uart_puts("\r\n");
                        return;
                }
                boot_snapshot_list(&st);
                return;
        }

        if (streq(argv[0], "restore"))
        {
                boot_status_t st;
                boot_refresh_status(&st);
                if (!st.sd_present)
                {
                        uart_puts("restore: SD not present\r\n");
                        return;
                }
                if (!st.sd_init_ok)
                {
                        uart_puts("restore: SD init failed err=");
                        uart_put_s32(st.sd_init_err);
                        uart_puts("\r\n");
                        return;
                }

                uint32_t rank = 0u;
                if (argc >= 2)
                {
                        if (!parse_u32(argv[1], &rank) || rank >= BOOT_SNAPSHOT_MENU_COUNT)
                        {
                                uart_puts("usage: restore [0|1|2]\r\n");
                                return;
                        }
                }
                if (!boot_snapshot_rank_to_slot(&st, rank, 0, 0))
                {
                        uart_puts("restore: snapshot rank unavailable\r\n");
                        return;
                }
                boot_action_boot_snapshot_rank(&st, rank);
                return;
        }

        if (streq(argv[0], "sddetect"))
        {
                sd_detect_init();
                if (sd_is_detected())
                {
                        uart_puts("sd detect: present\r\n");
                }
                else
                {
                        uart_puts("sd detect: not present\r\n");
                }
                return;
        }

        if (streq(argv[0], "gpiodump"))
        {
                gpio_dump();
                return;
        }

        if (streq(argv[0], "sdclk"))
        {
                if (argc < 2)
                {
                        uart_puts("usage: sdclk <sys|pll>\r\n");
                        return;
                }
                if (streq(argv[1], "sys"))
                {
                        sdmmc_set_kernel_clk_sys();
                        uart_puts("sdclk: sys\r\n");
                        return;
                }
                if (streq(argv[1], "pll"))
                {
                        sdmmc_set_kernel_clk_pll();
                        uart_puts("sdclk: pll\r\n");
                        return;
                }
                uart_puts("sdclk: unknown\r\n");
                return;
        }

        if (streq(argv[0], "sdclkdiv"))
        {
                if (argc < 2)
                {
                        uart_puts("usage: sdclkdiv <0-255>\r\n");
                        return;
                }
                uint32_t div = 0;
                if (!parse_u32(argv[1], &div))
                {
                        uart_puts("sdclkdiv: bad value\r\n");
                        return;
                }
                if (div > 255u)
                        div = 255u;
                sdmmc_set_clkdiv(div);
                sd_set_init_clkdiv(div);
                sd_set_data_clkdiv(div);
                uart_puts("sdclkdiv: ");
                uart_put_u32(div);
                uart_puts("\r\n");
                return;
        }

        if (streq(argv[0], "sdtoggle"))
        {
                uint32_t cycles = 8;
                if (argc >= 2)
                {
                        if (!parse_u32(argv[1], &cycles))
                        {
                                uart_puts("sdtoggle: bad value\r\n");
                                return;
                        }
                        if (cycles == 0)
                                cycles = 1;
                        if (cycles > 100)
                                cycles = 100;
                }
                sd_toggle_lines(cycles);
                uart_puts("sdtoggle: done\r\n");
                return;
        }

        if (streq(argv[0], "rccdump"))
        {
                rcc_dump();
                return;
        }

        if (streq(argv[0], "sdstat"))
        {
                sdmmc_dump();
                return;
        }

        if (streq(argv[0], "sdlast"))
        {
                uart_puts("sd_last_error=");
                uart_put_s32(sd_last_error());
                uart_puts(" sd_last_cmd=");
                uart_put_hex32(sd_last_cmd());
                uart_puts(" sd_last_sta=");
                uart_put_hex32(sd_last_sta());
                uart_puts("\r\n");
                return;
        }

        if (streq(argv[0], "sdtest"))
        {
                if (!sd_require_present())
                        return;
                uart_puts("sdtest: initializing...\r\n");
                sd_use_pll48(0);
                sd_set_data_clkdiv(SD_CLKDIV_BOOT);
                int rc = sd_init();
                if (rc != SD_OK)
                {
                        uart_puts("sdtest: init failed err=");
                        uart_put_s32(rc);
                        uart_puts(" cmd=");
                        uart_put_hex32(sd_last_cmd());
                        uart_puts(" sta=");
                        uart_put_hex32(sd_last_sta());
                        uart_puts("\r\n");
                        return;
                }

                const sd_info_t *info = sd_get_info();
                uart_puts("sdtest: init ok, capacity=");
                uart_put_u32(info->capacity_blocks / 2048u);
                uart_puts("MB, hc=");
                uart_put_u32(info->high_capacity);
                uart_puts(", bus=");
                uart_put_u32(info->bus_width);
                uart_puts("bit\r\n");

                uart_puts("sdtest: reading block 0...\r\n");
                rc = sd_read_blocks(0, 1, sd_buf_words);
                if (rc != SD_OK)
                {
                        uart_puts("sdtest: read failed err=");
                        uart_put_s32(rc);
                        uart_puts("\r\n");
                        return;
                }

                const uint8_t *buf = (const uint8_t *)sd_buf_words;
                uint8_t sig0 = buf[510];
                uint8_t sig1 = buf[511];
                uart_puts("sdtest: block 0 signature=");
                uart_put_hex8(sig0);
                uart_put_hex8(sig1);
                if (sig0 == 0x55 && sig1 == 0xAA)
                        uart_puts(" (valid MBR)\r\n");
                else
                        uart_puts(" (no MBR)\r\n");

                uart_puts("sdtest: first 64 bytes:\r\n");
                sd_dump_bytes(buf, 64);
                uart_puts("sdtest: PASS\r\n");
                return;
        }

        if (streq(argv[0], "sdread"))
        {
                if (argc < 2)
                {
                        uart_puts("usage: sdread <lba> [count]\r\n");
                        return;
                }

                uint32_t lba = 0;
                uint32_t count = 1;
                if (!parse_u32(argv[1], &lba))
                {
                        uart_puts("sdread: bad lba\r\n");
                        return;
                }
                if (argc >= 3 && !parse_u32(argv[2], &count))
                {
                        uart_puts("sdread: bad count\r\n");
                        return;
                }
                if (count == 0)
                        count = 1;
                if (count > SD_READ_MAX_BLOCKS)
                        count = SD_READ_MAX_BLOCKS;

                for (uint32_t i = 0; i < count; i++)
                {
                        int rc = sd_read_blocks(lba + i, 1, sd_buf_words);
                        if (rc != SD_OK)
                        {
                                uart_puts("sdread: err=");
                                uart_put_s32(rc);
                                uart_puts("\r\n");
                                return;
                        }

                        uart_puts("lba ");
                        uart_put_u32(lba + i);
                        uart_puts(":\r\n");
                        sd_dump_bytes((const uint8_t *)sd_buf_words, SD_DUMP_BYTES);
                }
                return;
        }

        uart_puts("unknown cmd: ");
        uart_puts(argv[0]);
        uart_puts("\r\n");
}

void boot_main(void)
{
        boot_status_t st;

        rewind_handoff_clear();

        uart_puts("\r\n");
        uart_puts("RewindOS Bootloader\r\n");
        uart_puts("\r\n");

        boot_refresh_status(&st);

        /* If SD failed but a valid app image exists, offer a quick
         * escape so the user isn't stuck in the setup loop.        */
        if (!st.sd_init_ok && st.app_present)
        {
                uart_puts("SD init failed (err=");
                uart_put_s32(st.sd_init_err);
                uart_puts(").  Press any key for menu, or wait 3s to boot fresh...\r\n");
                for (uint32_t i = 0u; i < 3000000u; i++)
                {
                        char tmp;
                        if (uart_try_getc(&tmp))
                                goto menu;
                        /* ~1 µs per iteration at 16 MHz (boot clock) ≈ 3 s total */
                }
                uart_puts("boot fresh...\r\n");
                uart_flush_tx();
                jump_to_image(APP_BASE);
                for (;;) {}
        }

menu:
        if (!st.has_admin_magic)
        {
                boot_setup_loop(0);
        }

        boot_menu_loop();
}
