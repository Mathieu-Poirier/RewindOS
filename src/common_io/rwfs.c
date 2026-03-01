#include "../../include/rwfs.h"
#include "../../include/rwos_disk.h"
#include "../../include/sd.h"
#include "../../include/sd_async.h"
#include "../../include/panic.h"

#define RWFS_MAGIC 0x53465752u /* "RWFS" */
#define RWFS_VERSION 1u

#define RWFS_SUPER_A_OFF 0u
#define RWFS_SUPER_B_OFF 1u
#define RWFS_INDEX_OFF 2u
#define RWFS_INDEX_BLOCKS 4u
#define RWFS_DATA_OFF (RWFS_INDEX_OFF + RWFS_INDEX_BLOCKS)
#define RWFS_ENTRY_FREE 0u
#define RWFS_ENTRY_LIVE 1u

#define RWFS_FDAT_MAGIC 0x54414446u /* "FDAT" */
#define RWFS_FDEN_MAGIC 0x4E454446u /* "FDEN" */
#define RWFS_HDR_WORDS 6u
#define RWFS_TAIL_WORDS 6u

#define RWFS_ERR_NAME -20
#define RWFS_ERR_FULL -21
#define RWFS_ERR_NOT_FOUND -22
#define RWFS_ERR_CORRUPT -23
#define RWFS_ERR_BUSY -24

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t generation;
    uint32_t region_start_lba;
    uint32_t region_blocks;
    uint32_t index_start_lba;
    uint32_t index_blocks;
    uint32_t data_start_lba;
    uint32_t data_blocks;
    uint32_t entry_count;
    uint32_t next_data_lba;
    uint32_t checksum;
    uint32_t reserved[(SD_BLOCK_SIZE / 4u) - 12u];
} rwfs_super_t;

typedef struct {
    char name[RWFS_NAME_MAX];
    uint32_t state;
    uint32_t data_lba;
    uint32_t data_len;
    uint32_t generation;
    uint32_t content_crc;
    uint32_t record_blocks;
    uint32_t entry_crc;
} rwfs_index_entry_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t name_hash;
    uint32_t generation;
    uint32_t payload_len;
    uint32_t payload_crc;
    uint32_t checksum;
    uint32_t reserved[(SD_BLOCK_SIZE / 4u) - 7u];
} rwfs_data_hdr_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t name_hash;
    uint32_t generation;
    uint32_t payload_len;
    uint32_t payload_crc;
    uint32_t checksum;
    uint32_t reserved[(SD_BLOCK_SIZE / 4u) - 7u];
} rwfs_data_tail_t;

typedef struct {
    uint8_t mounted;
    uint8_t formatted;
    uint16_t _pad;
    int32_t last_err;
    rwfs_super_t super;
    rwfs_index_entry_t entries[(RWFS_INDEX_BLOCKS * SD_BLOCK_SIZE) / sizeof(rwfs_index_entry_t)];
} rwfs_ctx_t;

static rwfs_ctx_t g_rwfs;
static uint32_t g_rwfs_blk[SD_BLOCK_SIZE / 4u];

static void rwfs_assert_invariants(void)
{
    PANIC_IF(sizeof(rwfs_super_t) != SD_BLOCK_SIZE, "rwfs super size != block");
    PANIC_IF(sizeof(rwfs_data_hdr_t) != SD_BLOCK_SIZE, "rwfs data hdr size != block");
    PANIC_IF(sizeof(rwfs_data_tail_t) != SD_BLOCK_SIZE, "rwfs data tail size != block");
    PANIC_IF(sizeof(g_rwfs.entries) == 0u, "rwfs entries storage empty");
}

static uint32_t rwfs_checksum_words(const uint32_t *w, uint32_t count)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0u; i < count; i++) {
        h ^= w[i];
        h *= 16777619u;
    }
    return h ^ 0xA5A55A5Au;
}

static void rwfs_words_copy(uint32_t *dst, const uint32_t *src, uint32_t words)
{
    for (uint32_t i = 0u; i < words; i++) {
        dst[i] = src[i];
    }
}

static uint32_t rwfs_checksum_bytes(const uint8_t *b, uint32_t len)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0u; i < len; i++) {
        h ^= b[i];
        h *= 16777619u;
    }
    return h ^ 0xA5A55A5Au;
}

static uint32_t rwfs_div_up(uint32_t n, uint32_t d)
{
    if (d == 0u) {
        return 0u;
    }
    return (n + d - 1u) / d;
}

static uint32_t rwfs_name_hash(const char *name)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0u; i < RWFS_NAME_MAX && name[i] != '\0'; i++) {
        h ^= (uint8_t)name[i];
        h *= 16777619u;
    }
    return h ^ 0xA5A55A5Au;
}

static uint32_t rwfs_strnlen(const char *s, uint32_t max)
{
    uint32_t n = 0u;
    while (n < max && s[n] != '\0') {
        n++;
    }
    return n;
}

static int rwfs_name_valid(const char *name)
{
    if (name == 0 || name[0] == '\0') {
        return 0;
    }
    uint32_t n = rwfs_strnlen(name, RWFS_NAME_MAX);
    if (n == 0u || n >= RWFS_NAME_MAX) {
        return 0;
    }
    return 1;
}

static int rwfs_streq_name(const char *a, const char *b)
{
    for (uint32_t i = 0u; i < RWFS_NAME_MAX; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
        if (a[i] == '\0') {
            return 1;
        }
    }
    return 1;
}

static void rwfs_name_copy(char *dst, const char *src)
{
    uint32_t i = 0u;
    for (; i < (RWFS_NAME_MAX - 1u) && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
    for (i = i + 1u; i < RWFS_NAME_MAX; i++) {
        dst[i] = '\0';
    }
}

static int rwfs_media_ready(void)
{
    if (g_sd_ctx.operation != SD_OP_NONE || g_sd_ctx.status == DRV_IN_PROGRESS) {
        return RWFS_ERR_BUSY;
    }
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

static int rwfs_super_valid(const rwfs_super_t *s)
{
    if (s->magic != RWFS_MAGIC || s->version != RWFS_VERSION) {
        return 0;
    }
    if (s->index_blocks != RWFS_INDEX_BLOCKS) {
        return 0;
    }
    if (s->entry_count != (uint32_t)(sizeof(g_rwfs.entries) / sizeof(g_rwfs.entries[0]))) {
        return 0;
    }
    if (s->data_start_lba < (s->region_start_lba + RWFS_DATA_OFF)) {
        return 0;
    }
    if (s->next_data_lba < s->data_start_lba) {
        return 0;
    }
    if (s->next_data_lba > (s->region_start_lba + s->region_blocks)) {
        return 0;
    }
    return s->checksum == rwfs_checksum_words((const uint32_t *)s, 11u);
}

static int rwfs_read_super_copy(uint32_t lba, rwfs_super_t *out)
{
    if (sd_read_blocks(lba, 1u, g_rwfs_blk) != SD_OK) {
        return 0;
    }
    const rwfs_super_t *s = (const rwfs_super_t *)g_rwfs_blk;
    if (!rwfs_super_valid(s)) {
        return 0;
    }
    if (out != 0) {
        rwfs_words_copy((uint32_t *)out,
                        (const uint32_t *)s,
                        (uint32_t)(sizeof(rwfs_super_t) / sizeof(uint32_t)));
    }
    return 1;
}

static int rwfs_write_super_both(const rwfs_super_t *s)
{
    int rc = sd_write_blocks(s->region_start_lba + RWFS_SUPER_A_OFF, 1u, s);
    if (rc != SD_OK) {
        return rc;
    }
    return sd_write_blocks(s->region_start_lba + RWFS_SUPER_B_OFF, 1u, s);
}

static uint32_t rwfs_entry_crc(const rwfs_index_entry_t *e)
{
    return rwfs_checksum_words((const uint32_t *)e,
                               (uint32_t)(sizeof(rwfs_index_entry_t) / sizeof(uint32_t)) - 1u);
}

static int rwfs_read_index(const rwfs_super_t *s)
{
    uint32_t total_entries = (uint32_t)(sizeof(g_rwfs.entries) / sizeof(g_rwfs.entries[0]));
    uint32_t entries_per_block = SD_BLOCK_SIZE / (uint32_t)sizeof(rwfs_index_entry_t);
    uint32_t idx = 0u;

    for (uint32_t b = 0u; b < RWFS_INDEX_BLOCKS; b++) {
        int rc = sd_read_blocks(s->index_start_lba + b, 1u, g_rwfs_blk);
        if (rc != SD_OK) {
            return rc;
        }
        rwfs_index_entry_t *blk_entries = (rwfs_index_entry_t *)g_rwfs_blk;
        for (uint32_t i = 0u; i < entries_per_block && idx < total_entries; i++, idx++) {
            rwfs_words_copy((uint32_t *)&g_rwfs.entries[idx],
                            (const uint32_t *)&blk_entries[i],
                            (uint32_t)(sizeof(rwfs_index_entry_t) / sizeof(uint32_t)));
            if (g_rwfs.entries[idx].state == RWFS_ENTRY_LIVE) {
                if (g_rwfs.entries[idx].entry_crc != rwfs_entry_crc(&g_rwfs.entries[idx])) {
                    g_rwfs.entries[idx].state = RWFS_ENTRY_FREE;
                }
            }
        }
    }
    return SD_OK;
}

static int rwfs_write_index_entry(const rwfs_super_t *s, uint32_t entry_idx, const rwfs_index_entry_t *entry)
{
    uint32_t entries_per_block = SD_BLOCK_SIZE / (uint32_t)sizeof(rwfs_index_entry_t);
    uint32_t block_idx = entry_idx / entries_per_block;
    uint32_t slot_idx = entry_idx % entries_per_block;
    if (block_idx >= RWFS_INDEX_BLOCKS) {
        return RWFS_ERR_CORRUPT;
    }

    int rc = sd_read_blocks(s->index_start_lba + block_idx, 1u, g_rwfs_blk);
    if (rc != SD_OK) {
        return rc;
    }
    rwfs_index_entry_t *blk_entries = (rwfs_index_entry_t *)g_rwfs_blk;
    rwfs_words_copy((uint32_t *)&blk_entries[slot_idx],
                    (const uint32_t *)entry,
                    (uint32_t)(sizeof(rwfs_index_entry_t) / sizeof(uint32_t)));
    return sd_write_blocks(s->index_start_lba + block_idx, 1u, g_rwfs_blk);
}

static int rwfs_format_region(uint32_t region_start_lba, uint32_t region_blocks)
{
    if (region_blocks <= RWFS_DATA_OFF) {
        return RWFS_ERR_FULL;
    }

    rwfs_super_t s;
    for (uint32_t i = 0u; i < (sizeof(s) / sizeof(uint32_t)); i++) {
        ((uint32_t *)&s)[i] = 0u;
    }
    s.magic = RWFS_MAGIC;
    s.version = RWFS_VERSION;
    s.generation = 1u;
    s.region_start_lba = region_start_lba;
    s.region_blocks = region_blocks;
    s.index_start_lba = region_start_lba + RWFS_INDEX_OFF;
    s.index_blocks = RWFS_INDEX_BLOCKS;
    s.data_start_lba = region_start_lba + RWFS_DATA_OFF;
    s.data_blocks = region_blocks - RWFS_DATA_OFF;
    s.entry_count = (uint32_t)(sizeof(g_rwfs.entries) / sizeof(g_rwfs.entries[0]));
    s.next_data_lba = s.data_start_lba;
    s.checksum = rwfs_checksum_words((const uint32_t *)&s, 11u);

    for (uint32_t i = 0u; i < (SD_BLOCK_SIZE / 4u); i++) {
        g_rwfs_blk[i] = 0u;
    }
    for (uint32_t b = 0u; b < RWFS_INDEX_BLOCKS; b++) {
        int rc = sd_write_blocks(s.index_start_lba + b, 1u, g_rwfs_blk);
        if (rc != SD_OK) {
            return rc;
        }
    }

    int rc = rwfs_write_super_both(&s);
    if (rc != SD_OK) {
        return rc;
    }

    rwfs_words_copy((uint32_t *)&g_rwfs.super,
                    (const uint32_t *)&s,
                    (uint32_t)(sizeof(rwfs_super_t) / sizeof(uint32_t)));
    for (uint32_t i = 0u; i < s.entry_count; i++) {
        g_rwfs.entries[i].state = RWFS_ENTRY_FREE;
    }
    g_rwfs.mounted = 1u;
    g_rwfs.formatted = 1u;
    g_rwfs.last_err = SD_OK;
    return SD_OK;
}

int rwfs_init_or_mount(void)
{
    rwfs_assert_invariants();

    int rc = rwfs_media_ready();
    if (rc != SD_OK) {
        g_rwfs.last_err = rc;
        return rc;
    }

    const rwos_disk_t *disk = rwos_disk_get();
    if (disk == 0) {
        g_rwfs.last_err = SD_ERR_NO_INIT;
        return SD_ERR_NO_INIT;
    }

    uint32_t fs_start  = disk->rwfs_start_lba;
    uint32_t fs_blocks = disk->rwfs_blocks;
    if (fs_blocks <= RWFS_DATA_OFF) {
        g_rwfs.last_err = RWFS_ERR_FULL;
        return RWFS_ERR_FULL;
    }

    rwfs_super_t a;
    rwfs_super_t b;
    int have_a = rwfs_read_super_copy(fs_start + RWFS_SUPER_A_OFF, &a);
    int have_b = rwfs_read_super_copy(fs_start + RWFS_SUPER_B_OFF, &b);

    if (!have_a && !have_b) {
        return rwfs_format_region(fs_start, fs_blocks);
    }

    {
        const rwfs_super_t *best = (have_a && (!have_b || a.generation >= b.generation)) ? &a : &b;
        rwfs_words_copy((uint32_t *)&g_rwfs.super,
                        (const uint32_t *)best,
                        (uint32_t)(sizeof(rwfs_super_t) / sizeof(uint32_t)));
    }
    if (g_rwfs.super.region_start_lba != fs_start || g_rwfs.super.region_blocks > fs_blocks) {
        return rwfs_format_region(fs_start, fs_blocks);
    }

    rc = rwfs_read_index(&g_rwfs.super);
    if (rc != SD_OK) {
        g_rwfs.last_err = rc;
        return rc;
    }

    g_rwfs.mounted = 1u;
    g_rwfs.formatted = 1u;
    g_rwfs.last_err = SD_OK;
    return SD_OK;
}

static int rwfs_find_entry(const char *name, uint32_t *idx_out)
{
    for (uint32_t i = 0u; i < g_rwfs.super.entry_count; i++) {
        if (g_rwfs.entries[i].state != RWFS_ENTRY_LIVE) {
            continue;
        }
        if (rwfs_streq_name(g_rwfs.entries[i].name, name)) {
            if (idx_out != 0) {
                *idx_out = i;
            }
            return 1;
        }
    }
    return 0;
}

static int rwfs_find_free_entry(uint32_t *idx_out)
{
    for (uint32_t i = 0u; i < g_rwfs.super.entry_count; i++) {
        if (g_rwfs.entries[i].state == RWFS_ENTRY_FREE) {
            if (idx_out != 0) {
                *idx_out = i;
            }
            return 1;
        }
    }
    return 0;
}

int rwfs_write_all_atomic(const char *name, const void *data, uint32_t len)
{
    if (!rwfs_name_valid(name)) {
        return RWFS_ERR_NAME;
    }
    if (data == 0 && len > 0u) {
        return SD_ERR_PARAM;
    }

    int rc = rwfs_init_or_mount();
    if (rc != SD_OK) {
        return rc;
    }

    uint32_t entry_idx = 0u;
    uint32_t prior_gen = 0u;
    if (rwfs_find_entry(name, &entry_idx)) {
        prior_gen = g_rwfs.entries[entry_idx].generation;
    } else if (!rwfs_find_free_entry(&entry_idx)) {
        return RWFS_ERR_FULL;
    }
    uint32_t new_gen = prior_gen + 1u;

    uint32_t payload_blocks = rwfs_div_up(len, SD_BLOCK_SIZE);
    uint32_t record_blocks = 2u + payload_blocks;
    uint32_t data_end = g_rwfs.super.region_start_lba + g_rwfs.super.region_blocks;
    if (g_rwfs.super.next_data_lba + record_blocks > data_end) {
        return RWFS_ERR_FULL;
    }

    uint32_t name_hash = rwfs_name_hash(name);
    uint32_t content_crc = rwfs_checksum_bytes((const uint8_t *)data, len);
    uint32_t rec_lba = g_rwfs.super.next_data_lba;

    rwfs_data_hdr_t hdr;
    for (uint32_t i = 0u; i < (sizeof(hdr) / sizeof(uint32_t)); i++) {
        ((uint32_t *)&hdr)[i] = 0u;
    }
    hdr.magic = RWFS_FDAT_MAGIC;
    hdr.version = RWFS_VERSION;
    hdr.name_hash = name_hash;
    hdr.generation = new_gen;
    hdr.payload_len = len;
    hdr.payload_crc = content_crc;
    hdr.checksum = rwfs_checksum_words((const uint32_t *)&hdr, RWFS_HDR_WORDS);

    rc = sd_write_blocks(rec_lba, 1u, &hdr);
    if (rc != SD_OK) {
        return rc;
    }

    const uint8_t *src = (const uint8_t *)data;
    uint32_t rem = len;
    uint32_t lba = rec_lba + 1u;
    while (rem > 0u) {
        uint8_t *blk = (uint8_t *)g_rwfs_blk;
        uint32_t n = (rem > SD_BLOCK_SIZE) ? SD_BLOCK_SIZE : rem;
        for (uint32_t i = 0u; i < SD_BLOCK_SIZE; i++) {
            blk[i] = 0u;
        }
        for (uint32_t i = 0u; i < n; i++) {
            blk[i] = src[i];
        }
        rc = sd_write_blocks(lba, 1u, g_rwfs_blk);
        if (rc != SD_OK) {
            return rc;
        }
        lba++;
        src += n;
        rem -= n;
    }

    rwfs_data_tail_t tail;
    for (uint32_t i = 0u; i < (sizeof(tail) / sizeof(uint32_t)); i++) {
        ((uint32_t *)&tail)[i] = 0u;
    }
    tail.magic = RWFS_FDEN_MAGIC;
    tail.version = RWFS_VERSION;
    tail.name_hash = name_hash;
    tail.generation = new_gen;
    tail.payload_len = len;
    tail.payload_crc = content_crc;
    tail.checksum = rwfs_checksum_words((const uint32_t *)&tail, RWFS_TAIL_WORDS);

    rc = sd_write_blocks(rec_lba + record_blocks - 1u, 1u, &tail);
    if (rc != SD_OK) {
        return rc;
    }

    rwfs_index_entry_t e;
    for (uint32_t i = 0u; i < (sizeof(e) / sizeof(uint32_t)); i++) {
        ((uint32_t *)&e)[i] = 0u;
    }
    rwfs_name_copy(e.name, name);
    e.state = RWFS_ENTRY_LIVE;
    e.data_lba = rec_lba;
    e.data_len = len;
    e.generation = new_gen;
    e.content_crc = content_crc;
    e.record_blocks = record_blocks;
    e.entry_crc = rwfs_entry_crc(&e);

    rc = rwfs_write_index_entry(&g_rwfs.super, entry_idx, &e);
    if (rc != SD_OK) {
        return rc;
    }

    rwfs_words_copy((uint32_t *)&g_rwfs.entries[entry_idx],
                    (const uint32_t *)&e,
                    (uint32_t)(sizeof(rwfs_index_entry_t) / sizeof(uint32_t)));
    g_rwfs.super.next_data_lba += record_blocks;
    g_rwfs.super.generation += 1u;
    g_rwfs.super.checksum = rwfs_checksum_words((const uint32_t *)&g_rwfs.super, 11u);
    rc = rwfs_write_super_both(&g_rwfs.super);
    if (rc != SD_OK) {
        return rc;
    }

    g_rwfs.last_err = SD_OK;
    return SD_OK;
}

int rwfs_read_all(const char *name, void *buf, uint32_t cap, uint32_t *out_len)
{
    if (!rwfs_name_valid(name) || buf == 0 || out_len == 0) {
        return SD_ERR_PARAM;
    }

    int rc = rwfs_init_or_mount();
    if (rc != SD_OK) {
        return rc;
    }

    uint32_t idx = 0u;
    if (!rwfs_find_entry(name, &idx)) {
        return RWFS_ERR_NOT_FOUND;
    }
    const rwfs_index_entry_t *e = &g_rwfs.entries[idx];
    if (e->data_len > cap) {
        return SD_ERR_PARAM;
    }

    rc = sd_read_blocks(e->data_lba, 1u, g_rwfs_blk);
    if (rc != SD_OK) {
        return rc;
    }
    const rwfs_data_hdr_t *hdr = (const rwfs_data_hdr_t *)g_rwfs_blk;
    uint32_t name_hash = rwfs_name_hash(name);
    if (hdr->magic != RWFS_FDAT_MAGIC || hdr->version != RWFS_VERSION ||
        hdr->name_hash != name_hash || hdr->generation != e->generation ||
        hdr->payload_len != e->data_len ||
        hdr->checksum != rwfs_checksum_words((const uint32_t *)hdr, RWFS_HDR_WORDS)) {
        return RWFS_ERR_CORRUPT;
    }

    uint8_t *dst = (uint8_t *)buf;
    uint32_t rem = e->data_len;
    uint32_t lba = e->data_lba + 1u;
    while (rem > 0u) {
        rc = sd_read_blocks(lba, 1u, g_rwfs_blk);
        if (rc != SD_OK) {
            return rc;
        }
        uint32_t n = (rem > SD_BLOCK_SIZE) ? SD_BLOCK_SIZE : rem;
        const uint8_t *blk = (const uint8_t *)g_rwfs_blk;
        for (uint32_t i = 0u; i < n; i++) {
            dst[i] = blk[i];
        }
        dst += n;
        rem -= n;
        lba++;
    }

    rc = sd_read_blocks(e->data_lba + e->record_blocks - 1u, 1u, g_rwfs_blk);
    if (rc != SD_OK) {
        return rc;
    }
    const rwfs_data_tail_t *tail = (const rwfs_data_tail_t *)g_rwfs_blk;
    if (tail->magic != RWFS_FDEN_MAGIC || tail->version != RWFS_VERSION ||
        tail->name_hash != name_hash || tail->generation != e->generation ||
        tail->payload_len != e->data_len ||
        tail->checksum != rwfs_checksum_words((const uint32_t *)tail, RWFS_TAIL_WORDS)) {
        return RWFS_ERR_CORRUPT;
    }
    if (tail->payload_crc != e->content_crc || hdr->payload_crc != e->content_crc) {
        return RWFS_ERR_CORRUPT;
    }

    if (rwfs_checksum_bytes((const uint8_t *)buf, e->data_len) != e->content_crc) {
        return RWFS_ERR_CORRUPT;
    }
    *out_len = e->data_len;
    return SD_OK;
}

int rwfs_list(rwfs_file_info_t *out, uint32_t cap, uint32_t *out_count)
{
    if (out_count == 0) {
        return SD_ERR_PARAM;
    }

    int rc = rwfs_init_or_mount();
    if (rc != SD_OK) {
        return rc;
    }

    uint32_t n = 0u;
    for (uint32_t i = 0u; i < g_rwfs.super.entry_count; i++) {
        const rwfs_index_entry_t *e = &g_rwfs.entries[i];
        if (e->state != RWFS_ENTRY_LIVE) {
            continue;
        }
        if (out != 0 && n < cap) {
            rwfs_name_copy(out[n].name, e->name);
            out[n].size_bytes = e->data_len;
            out[n].generation = e->generation;
        }
        n++;
    }
    *out_count = n;
    return SD_OK;
}

void rwfs_get_status(rwfs_status_t *out)
{
    if (out == 0) {
        return;
    }
    out->mounted = g_rwfs.mounted;
    out->formatted = g_rwfs.formatted;
    out->_pad = 0u;
    out->last_err = g_rwfs.last_err;
    out->region_start_lba = g_rwfs.super.region_start_lba;
    out->region_blocks = g_rwfs.super.region_blocks;
    out->next_data_lba = g_rwfs.super.next_data_lba;
    out->super_generation = g_rwfs.super.generation;

    uint32_t count = 0u;
    for (uint32_t i = 0u; i < g_rwfs.super.entry_count; i++) {
        if (g_rwfs.entries[i].state == RWFS_ENTRY_LIVE) {
            count++;
        }
    }
    out->file_count = count;
}
