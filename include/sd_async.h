#pragma once

#include "stdint.h"
#include "driver_common.h"
#include "sd.h"
#include "scheduler.h"

typedef enum {
    SD_OP_NONE  = 0,
    SD_OP_READ  = 1,
    SD_OP_WRITE = 2
} sd_operation_t;

typedef struct {
    volatile sd_operation_t operation;
    volatile drv_status_t   status;
    volatile int32_t        error_code;
    volatile uint32_t       error_detail;  /* SDMMC_STA value on error */

    volatile uint32_t  lba;
    volatile uint32_t  total_blocks;
    volatile uint32_t  blocks_done;
    volatile uint8_t  *buffer;
    volatile uint32_t  fifo_words_left;
    volatile uint8_t  *current_ptr;
    volatile uint8_t   high_capacity;
} sd_context_t;

extern sd_context_t g_sd_ctx;

void sd_async_init(void);
int  sd_async_read_start(uint32_t lba, uint32_t count, void *buf);
drv_status_t sd_async_poll(void);
int  sd_async_error(void);
int  sd_read_blocks_blocking(uint32_t lba, uint32_t count, void *buf);

void sd_async_bind_scheduler(scheduler_t *sched, uint8_t ao_id, uint16_t done_sig, uint16_t err_sig);
void sd_async_unbind_scheduler(void);

void SDMMC1_IRQHandler(void);
