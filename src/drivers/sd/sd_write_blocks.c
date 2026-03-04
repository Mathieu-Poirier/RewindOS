#include "../../../include/sd.h"

extern sd_info_t g_sd_info;
extern int32_t g_sd_last_error;
extern int sdmmc_send_cmd(uint32_t cmd, uint32_t arg, uint32_t waitresp, uint32_t ignore_crc);

#define SDMMC_BASE      0x40012C00u
#define SDMMC_DTIMER    (*(volatile uint32_t *)(SDMMC_BASE + 0x24u))
#define SDMMC_DLEN      (*(volatile uint32_t *)(SDMMC_BASE + 0x28u))
#define SDMMC_DCTRL     (*(volatile uint32_t *)(SDMMC_BASE + 0x2Cu))
#define SDMMC_STA       (*(volatile uint32_t *)(SDMMC_BASE + 0x34u))
#define SDMMC_ICR       (*(volatile uint32_t *)(SDMMC_BASE + 0x38u))
#define SDMMC_RESP1     (*(volatile uint32_t *)(SDMMC_BASE + 0x14u))
#define SDMMC_MASK      (*(volatile uint32_t *)(SDMMC_BASE + 0x3Cu))
#define SDMMC_FIFO      (*(volatile uint32_t *)(SDMMC_BASE + 0x80u))

#define SDMMC_STA_DCRCFAIL       (1u << 1)
#define SDMMC_STA_DTIMEOUT       (1u << 3)
#define SDMMC_STA_TXUNDERR       (1u << 4)
#define SDMMC_STA_DATAEND        (1u << 8)
#define SDMMC_STA_STBITERR       (1u << 9)
#define SDMMC_STA_TXFIFOHE       (1u << 14)
#define SDMMC_STA_CKBUSY         (1u << 24)
#define SDMMC_STA_BUSYD0END      (1u << 25)
#define SDMMC_STA_DATAERR_MASK   (SDMMC_STA_DCRCFAIL | SDMMC_STA_DTIMEOUT | SDMMC_STA_TXUNDERR | SDMMC_STA_STBITERR)

#define SDMMC_DCTRL_DTEN             (1u << 0)
#define SDMMC_DCTRL_DBLOCKSIZE_512   (9u << 4)
#define SDMMC_DCTRL_WRITE            (SDMMC_DCTRL_DBLOCKSIZE_512 | SDMMC_DCTRL_DTEN)

/* Hardware data-timeout in SDMMC_CK cycles (24 MHz): 250 ms = 6,000,000 cycles. */
#define SDMMC_HW_DTIMER          6000000u
/* Software loop timeout; must outlast HW DTIMER. */
#define SDMMC_SW_TIMEOUT         10000000u
/* Post-write programming timeout (card DAT0 busy). */
#define SDMMC_PROG_TIMEOUT       10000000u
#define SDMMC_CARD_READY_POLLS   100000u
#define SDMMC_CARD_READY_CMD_FAIL_LIMIT 3u
#define SDMMC_CMD_WAITRESP_SHORT (1u << 6)
#define SD_CMD_WRITE_SINGLE_BLOCK 24u
#define SD_CMD_SEND_STATUS 13u

static volatile uint32_t g_sd_dbg_wait_ready_calls;
static volatile uint32_t g_sd_dbg_wait_ready_ok;
static volatile uint32_t g_sd_dbg_wait_ready_timeout;
static volatile uint32_t g_sd_dbg_wait_ready_cmd_fail_fast;

/* Write-failure stage: 1=wait_ready, 2=cmd24, 3=fifo, 4=dataend, 5=ckbusy */
static volatile uint32_t g_sd_dbg_write_stage;
static volatile uint32_t g_sd_dbg_write_last_sta;

uint32_t sd_dbg_write_stage(void) { return g_sd_dbg_write_stage; }
uint32_t sd_dbg_write_last_sta(void) { return g_sd_dbg_write_last_sta; }

int sd_wait_card_ready(void)
{
    uint32_t arg = g_sd_info.rca; /* already positioned for addressed commands */
    uint32_t polls = SDMMC_CARD_READY_POLLS;
    uint32_t cmd_fails = 0u;
    int last_rc = SD_ERR_TIMEOUT;

    g_sd_dbg_wait_ready_calls++;

    while (polls-- > 0u) {
        int rc = sdmmc_send_cmd(SD_CMD_SEND_STATUS, arg, SDMMC_CMD_WAITRESP_SHORT, 0u);
        if (rc != SD_OK) {
            last_rc = rc;
            cmd_fails++;
            if (cmd_fails >= SDMMC_CARD_READY_CMD_FAIL_LIMIT) {
                g_sd_dbg_wait_ready_cmd_fail_fast++;
                return rc;
            }
            continue;
        }

        cmd_fails = 0u;
        {
            uint32_t resp = SDMMC_RESP1;
            uint32_t ready = (resp >> 8) & 0x1u;
            uint32_t state = (resp >> 9) & 0xFu;
            if (ready && state == 4u) { /* TRAN */
                g_sd_dbg_wait_ready_ok++;
                return SD_OK;
            }
        }
    }

    g_sd_dbg_wait_ready_timeout++;
    return last_rc;
}

uint32_t sd_dbg_wait_ready_calls(void) { return g_sd_dbg_wait_ready_calls; }
uint32_t sd_dbg_wait_ready_ok(void) { return g_sd_dbg_wait_ready_ok; }
uint32_t sd_dbg_wait_ready_timeout(void) { return g_sd_dbg_wait_ready_timeout; }
uint32_t sd_dbg_wait_ready_cmd_fail_fast(void) { return g_sd_dbg_wait_ready_cmd_fail_fast; }

static int sd_write_block(uint32_t addr, const uint32_t *src_words)
{
    uint32_t words_left;
    uint32_t timeout;
    int rc;

    g_sd_dbg_write_stage = 1u;
    rc = sd_wait_card_ready();
    if (rc != SD_OK) {
        return rc;
    }

    /* Disable async interrupt mask to prevent ISR interference. */
    SDMMC_MASK = 0u;
    SDMMC_DCTRL = 0u;
    SDMMC_ICR = 0xFFFFFFFFu;

    SDMMC_DTIMER = SDMMC_HW_DTIMER;
    SDMMC_DLEN = SD_BLOCK_SIZE;

    /* For writes, send CMD24 before enabling DPSM. */
    g_sd_dbg_write_stage = 2u;
    rc = sdmmc_send_cmd(SD_CMD_WRITE_SINGLE_BLOCK, addr, SDMMC_CMD_WAITRESP_SHORT, 0u);
    if (rc != SD_OK) {
        SDMMC_ICR = 0xFFFFFFFFu;
        return rc;
    }

    SDMMC_DCTRL = SDMMC_DCTRL_WRITE;

    g_sd_dbg_write_stage = 3u;
    words_left = SD_BLOCK_SIZE / 4u;
    timeout = SDMMC_SW_TIMEOUT;
    while (words_left > 0u) {
        uint32_t sta = SDMMC_STA;
        if ((sta & SDMMC_STA_DATAERR_MASK) != 0u) {
            SDMMC_DCTRL = 0u;
            SDMMC_ICR = 0xFFFFFFFFu;
            return SD_ERR_DATA;
        }
        if ((sta & SDMMC_STA_TXFIFOHE) != 0u) {
            SDMMC_FIFO = *src_words++;
            words_left--;
            timeout = SDMMC_SW_TIMEOUT;
            continue;
        }
        if (timeout == 0u) {
            g_sd_dbg_write_last_sta = SDMMC_STA;
            SDMMC_DCTRL = 0u;
            SDMMC_ICR = 0xFFFFFFFFu;
            return SD_ERR_TIMEOUT;
        }
        timeout--;
    }

    g_sd_dbg_write_stage = 4u;
    timeout = SDMMC_SW_TIMEOUT;
    while (timeout > 0u) {
        uint32_t sta = SDMMC_STA;
        if ((sta & SDMMC_STA_DATAEND) != 0u) {
            break;
        }
        if ((sta & SDMMC_STA_DATAERR_MASK) != 0u) {
            SDMMC_DCTRL = 0u;
            SDMMC_ICR = 0xFFFFFFFFu;
            return SD_ERR_DATA;
        }
        timeout--;
    }
    if (timeout == 0u) {
        g_sd_dbg_write_last_sta = SDMMC_STA;
        SDMMC_DCTRL = 0u;
        SDMMC_ICR = 0xFFFFFFFFu;
        return SD_ERR_TIMEOUT;
    }

    g_sd_dbg_write_stage = 5u;
    timeout = SDMMC_PROG_TIMEOUT;
    while (timeout > 0u) {
        uint32_t sta = SDMMC_STA;
        if ((sta & SDMMC_STA_DATAERR_MASK) != 0u) {
            SDMMC_DCTRL = 0u;
            SDMMC_ICR = 0xFFFFFFFFu;
            return SD_ERR_DATA;
        }
        if ((sta & SDMMC_STA_CKBUSY) == 0u || (sta & SDMMC_STA_BUSYD0END) != 0u) {
            g_sd_dbg_write_stage = 0u;
            SDMMC_DCTRL = 0u;
            SDMMC_ICR = 0xFFFFFFFFu;
            return SD_OK;
        }
        timeout--;
    }

    g_sd_dbg_write_last_sta = SDMMC_STA;
    SDMMC_DCTRL = 0u;
    SDMMC_ICR = 0xFFFFFFFFu;
    return SD_ERR_TIMEOUT;
}

int sd_write_blocks(uint32_t lba, uint32_t count, const void *buf)
{
    const uint32_t *src;
    uint32_t i;

    if (!g_sd_info.initialized) {
        g_sd_last_error = SD_ERR_NO_INIT;
        return SD_ERR_NO_INIT;
    }
    if (count == 0u || buf == 0) {
        g_sd_last_error = SD_ERR_PARAM;
        return SD_ERR_PARAM;
    }
    if (((uintptr_t)buf & 3u) != 0u) {
        g_sd_last_error = SD_ERR_PARAM;
        return SD_ERR_PARAM;
    }

    src = (const uint32_t *)buf;
    for (i = 0u; i < count; i++) {
        uint32_t curr_lba = lba + i;
        uint32_t addr = g_sd_info.high_capacity ? curr_lba : (curr_lba << 9);
        int rc = sd_write_block(addr, src);
        if (rc != SD_OK) {
            g_sd_last_error = rc;
            return rc;
        }
        src += (SD_BLOCK_SIZE / 4u);
    }

    g_sd_last_error = SD_OK;
    return SD_OK;
}
