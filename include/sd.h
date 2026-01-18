#pragma once
#include "stdint.h"

#define SD_BLOCK_SIZE 512u

/* Clock dividers: SDMMC_CK = KERNEL_CLK / (CLKDIV + 2)
 * Boot (HSI 16MHz):   CLKDIV=0  -> 8MHz
 * Fast (PLL48 48MHz): CLKDIV=0  -> 24MHz
 * Fast (SYSCLK 216MHz): CLKDIV=8 -> 21.6MHz (default speed)
 *                       CLKDIV=4 -> 36MHz   (high speed)
 */
#define SD_CLKDIV_INIT  118u  /* ~400kHz for card identification */
#define SD_CLKDIV_BOOT  0u    /* 8MHz with 16MHz HSI */
#define SD_CLKDIV_FAST  8u    /* 21.6MHz with 216MHz SYSCLK */
#define SD_CLKDIV_HS    4u    /* 36MHz with 216MHz SYSCLK */

#define SD_OK 0
#define SD_ERR_TIMEOUT -1
#define SD_ERR_CRC -2
#define SD_ERR_CMD -3
#define SD_ERR_DATA -4
#define SD_ERR_NO_INIT -5
#define SD_ERR_PARAM -6

typedef struct sd_info
{
        uint32_t rca;
        uint32_t ocr;
        uint32_t capacity_blocks;
        uint32_t cid[4];
        uint32_t csd[4];
        uint8_t high_capacity;
        uint8_t bus_width;
        uint8_t initialized;
        uint8_t _pad;
} sd_info_t;

int sd_init(void);
int sd_read_blocks(uint32_t lba, uint32_t count, void *buf);
const sd_info_t *sd_get_info(void);
int sd_last_error(void);
void sd_detect_init(void);
int sd_is_detected(void);
uint32_t sd_last_cmd(void);
uint32_t sd_last_sta(void);
void sd_set_init_clkdiv(uint32_t div);
uint32_t sd_get_init_clkdiv(void);
void sd_set_data_clkdiv(uint32_t div);
uint32_t sd_get_data_clkdiv(void);
void sd_use_pll48(int use_pll);
int sd_get_use_pll48(void);
