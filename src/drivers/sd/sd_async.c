#include "../../../include/sd_async.h"
#include "../../../include/nvic.h"

#define SDMMC_BASE      0x40012C00
#define SDMMC_DTIMER    (*(volatile uint32_t *)(SDMMC_BASE + 0x24))
#define SDMMC_DLEN      (*(volatile uint32_t *)(SDMMC_BASE + 0x28))
#define SDMMC_DCTRL     (*(volatile uint32_t *)(SDMMC_BASE + 0x2C))
#define SDMMC_STA       (*(volatile uint32_t *)(SDMMC_BASE + 0x34))
#define SDMMC_ICR       (*(volatile uint32_t *)(SDMMC_BASE + 0x38))
#define SDMMC_MASK      (*(volatile uint32_t *)(SDMMC_BASE + 0x3C))
#define SDMMC_FIFO      (*(volatile uint32_t *)(SDMMC_BASE + 0x80))

#define SDMMC_STA_DCRCFAIL  (1 << 1)
#define SDMMC_STA_DTIMEOUT  (1 << 3)
#define SDMMC_STA_RXOVERR   (1 << 5)
#define SDMMC_STA_DATAEND   (1 << 8)
#define SDMMC_STA_RXFIFOHF  (1 << 15)
#define SDMMC_STA_RXDAVL    (1 << 21)

#define SDMMC_MASK_DCRCFAILIE   (1 << 1)
#define SDMMC_MASK_DTIMEOUTIE   (1 << 3)
#define SDMMC_MASK_RXOVERRIE    (1 << 5)
#define SDMMC_MASK_DATAENDIE    (1 << 8)
#define SDMMC_MASK_RXFIFOHFIE   (1 << 15)

#define SDMMC_DCTRL_DTEN            (1 << 0)
#define SDMMC_DCTRL_DTDIR           (1 << 1)
#define SDMMC_DCTRL_DBLOCKSIZE_512  (9 << 4)
#define SDMMC_DCTRL_READ            (SDMMC_DCTRL_DBLOCKSIZE_512 | SDMMC_DCTRL_DTDIR | SDMMC_DCTRL_DTEN)

#define SDMMC_DATA_TIMEOUT  1000000
#define SD_CMD_READ_SINGLE_BLOCK 17
#define SDMMC_CMD_WAITRESP_SHORT (1 << 6)

extern int sdmmc_send_cmd(uint32_t cmd, uint32_t arg, uint32_t waitresp, uint32_t ignore_crc);

sd_context_t g_sd_ctx;

static void sd_start_single_block_read(uint32_t addr)
{
    SDMMC_ICR = 0xFFFFFFFF;
    SDMMC_DTIMER = SDMMC_DATA_TIMEOUT;
    SDMMC_DLEN = SD_BLOCK_SIZE;
    SDMMC_DCTRL = SDMMC_DCTRL_READ;

    sdmmc_send_cmd(SD_CMD_READ_SINGLE_BLOCK, addr, SDMMC_CMD_WAITRESP_SHORT, 0);

    SDMMC_MASK = SDMMC_MASK_DCRCFAILIE | SDMMC_MASK_DTIMEOUTIE |
                 SDMMC_MASK_RXOVERRIE | SDMMC_MASK_DATAENDIE |
                 SDMMC_MASK_RXFIFOHFIE;
}

void SDMMC1_IRQHandler(void)
{
    uint32_t status = SDMMC_STA;

    if (status & (SDMMC_STA_DCRCFAIL | SDMMC_STA_DTIMEOUT | SDMMC_STA_RXOVERR)) {
        SDMMC_ICR = 0xFFFFFFFF;
        SDMMC_DCTRL = 0;
        SDMMC_MASK = 0;

        g_sd_ctx.status = DRV_ERROR;
        if (status & SDMMC_STA_DTIMEOUT) {
            g_sd_ctx.error_code = SD_ERR_TIMEOUT;
        } else if (status & SDMMC_STA_DCRCFAIL) {
            g_sd_ctx.error_code = SD_ERR_CRC;
        } else {
            g_sd_ctx.error_code = SD_ERR_DATA;
        }
        return;
    }

    if (status & SDMMC_STA_RXFIFOHF) {
        for (int i = 0; i < 8 && g_sd_ctx.fifo_words_left > 0; i++) {
            uint32_t data = SDMMC_FIFO;
            *((volatile uint32_t *)g_sd_ctx.current_ptr) = data;
            g_sd_ctx.current_ptr += 4;
            g_sd_ctx.fifo_words_left--;
        }
    }

    while ((SDMMC_STA & SDMMC_STA_RXDAVL) && g_sd_ctx.fifo_words_left > 0) {
        uint32_t data = SDMMC_FIFO;
        *((volatile uint32_t *)g_sd_ctx.current_ptr) = data;
        g_sd_ctx.current_ptr += 4;
        g_sd_ctx.fifo_words_left--;
    }

    if (status & SDMMC_STA_DATAEND) {
        SDMMC_ICR = SDMMC_STA_DATAEND;

        g_sd_ctx.blocks_done++;

        if (g_sd_ctx.blocks_done < g_sd_ctx.total_blocks) {
            g_sd_ctx.fifo_words_left = 128;
            uint32_t next_lba = g_sd_ctx.lba + g_sd_ctx.blocks_done;
            uint32_t addr = g_sd_ctx.high_capacity ? next_lba : (next_lba << 9);
            sd_start_single_block_read(addr);
        } else {
            SDMMC_DCTRL = 0;
            SDMMC_MASK = 0;
            SDMMC_ICR = 0xFFFFFFFF;
            g_sd_ctx.status = DRV_COMPLETE;
            g_sd_ctx.error_code = SD_OK;
        }
    }
}

void sd_async_init(void)
{
    g_sd_ctx.operation = SD_OP_NONE;
    g_sd_ctx.status = DRV_IDLE;
    g_sd_ctx.error_code = SD_OK;
    g_sd_ctx.lba = 0;
    g_sd_ctx.total_blocks = 0;
    g_sd_ctx.blocks_done = 0;
    g_sd_ctx.buffer = 0;
    g_sd_ctx.fifo_words_left = 0;
    g_sd_ctx.current_ptr = 0;
    g_sd_ctx.high_capacity = 0;
}

int sd_async_read_start(uint32_t lba, uint32_t count, void *buf)
{
    const sd_info_t *info = sd_get_info();
    if (!info->initialized) {
        return SD_ERR_NO_INIT;
    }
    if (count == 0 || buf == 0) {
        return SD_ERR_PARAM;
    }
    if (((uintptr_t)buf & 3) != 0) {
        return SD_ERR_PARAM;
    }

    g_sd_ctx.operation = SD_OP_READ;
    g_sd_ctx.status = DRV_IN_PROGRESS;
    g_sd_ctx.error_code = SD_OK;
    g_sd_ctx.lba = lba;
    g_sd_ctx.total_blocks = count;
    g_sd_ctx.blocks_done = 0;
    g_sd_ctx.buffer = (uint8_t *)buf;
    g_sd_ctx.current_ptr = (uint8_t *)buf;
    g_sd_ctx.fifo_words_left = 128;
    g_sd_ctx.high_capacity = info->high_capacity;

    nvic_set_priority(SDMMC1_IRQn, 1);
    nvic_clear_pending(SDMMC1_IRQn);
    nvic_enable_irq(SDMMC1_IRQn);

    uint32_t addr = g_sd_ctx.high_capacity ? lba : (lba << 9);
    sd_start_single_block_read(addr);

    return SD_OK;
}

drv_status_t sd_async_poll(void)
{
    return g_sd_ctx.status;
}

int sd_async_error(void)
{
    return g_sd_ctx.error_code;
}

int sd_read_blocks_blocking(uint32_t lba, uint32_t count, void *buf)
{
    int result = sd_async_read_start(lba, count, buf);
    if (result != SD_OK) {
        return result;
    }

    while (sd_async_poll() == DRV_IN_PROGRESS) {
        __asm__ volatile("nop");
    }

    if (sd_async_poll() == DRV_COMPLETE) {
        return SD_OK;
    }

    return sd_async_error();
}
