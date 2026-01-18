.syntax unified
.cpu cortex-m7
.thumb

.global sd_init
.type sd_init, %function

.extern enable_gpioc
.extern enable_gpiod
.extern pc8pc12_to_sdmmc1
.extern pd2_to_sdmmc1
.extern sd_detect_init
.extern enable_sdmmc1_kerclk_pll48
.extern enable_sdmmc1_kerclk_sysclk
.extern enable_sdmmc1_clock
.extern sdmmc_send_cmd
.extern g_sd_info
.extern g_sd_last_error
.extern g_sd_init_clkdiv
.extern g_sd_data_clkdiv
.extern g_sd_use_pll48

.equ SDMMC_BASE, 0x40012C00
.equ SDMMC_POWER, (SDMMC_BASE + 0x00)
.equ SDMMC_CLKCR, (SDMMC_BASE + 0x04)
.equ SDMMC_RESP1, (SDMMC_BASE + 0x14)
.equ SDMMC_ICR, (SDMMC_BASE + 0x38)
.equ SDMMC_MASK, (SDMMC_BASE + 0x3C)

.equ SDMMC_CMD_WAITRESP_NONE, 0
.equ SDMMC_CMD_WAITRESP_SHORT, (1 << 6)
.equ SDMMC_CMD_WAITRESP_LONG, (3 << 6)

.equ SDMMC_CLKCR_CLKEN, (1 << 8)
.equ SDMMC_CLKCR_WIDBUS_4, (1 << 11)

.equ SD_CMD_GO_IDLE_STATE, 0
.equ SD_CMD_ALL_SEND_CID, 2
.equ SD_CMD_SEND_RELATIVE_ADDR, 3
.equ SD_CMD_SEND_IF_COND, 8
.equ SD_CMD_SEND_CSD, 9
.equ SD_CMD_SELECT_CARD, 7
.equ SD_CMD_SET_BLOCKLEN, 16
.equ SD_CMD_APP_CMD, 55
.equ SD_ACMD_SD_SEND_OP_COND, 41
.equ SD_ACMD_SET_BUS_WIDTH, 6

.equ SDMMC_INIT_RETRIES, 10000
.equ SDMMC_DATA_CLKDIV, 0
.equ SD_BLOCK_SIZE, 512

.equ SDMMC_POWER_DELAY, 0x20000
.equ SDMMC_CLK_DELAY, 0x20000
.equ SDMMC_POWERUP_DELAY, 0x40000
.equ SDMMC_ACMD41_DELAY, 0x20000

.equ SD_ERR_TIMEOUT, -1

.equ SD_INFO_RCA, 0
.equ SD_INFO_OCR, 4
.equ SD_INFO_CAPACITY, 8
.equ SD_INFO_CID, 12
.equ SD_INFO_CSD, 28
.equ SD_INFO_HIGH_CAPACITY, 44
.equ SD_INFO_BUS_WIDTH, 45
.equ SD_INFO_INITIALIZED, 46

sd_init:
    push {r4-r7, lr}

    ldr r4, =g_sd_info
    movs r5, #0
    movs r6, #12
zero_info:
    str r5, [r4], #4
    subs r6, r6, #1
    bne zero_info

    ldr r4, =g_sd_info
    movs r5, #1
    strb r5, [r4, #SD_INFO_BUS_WIDTH]

    ldr r5, =g_sd_last_error
    movs r6, #0
    str r6, [r5]

    bl enable_gpioc
    bl enable_gpiod
    bl sd_detect_init
    bl pc8pc12_to_sdmmc1
    bl pd2_to_sdmmc1

    ldr r5, =g_sd_use_pll48
    ldr r5, [r5]
    cmp r5, #0
    bne use_pll48
    bl enable_sdmmc1_kerclk_sysclk
    b kerclk_done
use_pll48:
    bl enable_sdmmc1_kerclk_pll48
kerclk_done:
    bl enable_sdmmc1_clock

    ldr r5, =SDMMC_POWER
    movs r6, #0
    str r6, [r5]
    ldr r6, =SDMMC_POWER_DELAY
sd_power_delay0:
    subs r6, r6, #1
    bne sd_power_delay0

    movs r6, #3
    str r6, [r5]
    ldr r6, =SDMMC_POWER_DELAY
sd_power_delay1:
    subs r6, r6, #1
    bne sd_power_delay1

    ldr r5, =SDMMC_MASK
    movs r6, #0
    str r6, [r5]

    ldr r5, =SDMMC_CLKCR
    ldr r7, [r5]
    ldr r6, =0x00003E00
    bic r7, r7, r6
    ldr r6, =0x000000FF
    bic r7, r7, r6
    ldr r6, =g_sd_init_clkdiv
    ldr r6, [r6]
    cmp r6, #0
    bne sd_clkdiv_ok
    ldr r6, =118
sd_clkdiv_ok:
    orr r7, r7, r6
    orr r7, r7, #SDMMC_CLKCR_CLKEN
    str r7, [r5]
    ldr r6, =SDMMC_CLK_DELAY
sd_clk_delay:
    subs r6, r6, #1
    bne sd_clk_delay

    ldr r6, =SDMMC_POWERUP_DELAY
sd_powerup_delay:
    subs r6, r6, #1
    bne sd_powerup_delay

    ldr r5, =SDMMC_ICR
    ldr r6, =0xFFFFFFFF
    str r6, [r5]

    movs r0, #SD_CMD_GO_IDLE_STATE
    movs r1, #0
    movs r2, #SDMMC_CMD_WAITRESP_NONE
    movs r3, #1
    bl sdmmc_send_cmd
    cmp r0, #0
    bne sd_fail

    movs r7, #0
    movs r0, #SD_CMD_SEND_IF_COND
    ldr r1, =0x1AA
    movs r2, #SDMMC_CMD_WAITRESP_SHORT
    movs r3, #0
    bl sdmmc_send_cmd
    cmp r0, #0
    bne cmd8_check
    ldr r5, =SDMMC_RESP1
    ldr r6, [r5]
    ldr r5, =0x0FFF
    and r6, r6, r5
    ldr r5, =0x1AA
    cmp r6, r5
    bne cmd8_done
    movs r7, #1
    b cmd8_done
cmd8_check:
    ldr r5, =SD_ERR_TIMEOUT
    cmp r0, r5
    bne sd_fail
    movs r0, #SD_CMD_GO_IDLE_STATE
    movs r1, #0
    movs r2, #SDMMC_CMD_WAITRESP_NONE
    movs r3, #1
    bl sdmmc_send_cmd
    cmp r0, #0
    bne sd_fail
    b cmd8_done
cmd8_done:
    ldr r6, =0x80100000
    cmp r7, #0
    beq acmd41_start
    ldr r5, =0x41000000
    orr r6, r6, r5
acmd41_start:
    ldr r5, =SDMMC_INIT_RETRIES
acmd41_loop:
    movs r0, #SD_CMD_APP_CMD
    movs r1, #0
    movs r2, #SDMMC_CMD_WAITRESP_SHORT
    movs r3, #0
    bl sdmmc_send_cmd
    cmp r0, #0
    bne acmd41_retry

    movs r0, #SD_ACMD_SD_SEND_OP_COND
    mov r1, r6
    movs r2, #SDMMC_CMD_WAITRESP_SHORT
    movs r3, #1
    bl sdmmc_send_cmd
    cmp r0, #0
    bne acmd41_retry

    ldr r1, =SDMMC_RESP1
    ldr r2, [r1]
    ldr r3, =0x80000000
    tst r2, r3
    bne acmd41_ready

acmd41_retry:
    ldr r7, =SDMMC_ACMD41_DELAY
acmd41_delay:
    subs r7, r7, #1
    bne acmd41_delay
    subs r5, r5, #1
    bne acmd41_loop
    ldr r0, =SD_ERR_TIMEOUT
    b sd_fail

acmd41_ready:
    str r2, [r4, #SD_INFO_OCR]
    movs r3, #0
    ldr r5, =0x40000000
    tst r2, r5
    beq acmd41_store
    movs r3, #1
acmd41_store:
    strb r3, [r4, #SD_INFO_HIGH_CAPACITY]

    movs r0, #SD_CMD_ALL_SEND_CID
    movs r1, #0
    movs r2, #SDMMC_CMD_WAITRESP_LONG
    movs r3, #0
    bl sdmmc_send_cmd
    cmp r0, #0
    bne sd_fail

    ldr r5, =SDMMC_RESP1
    ldr r6, [r5]
    str r6, [r4, #SD_INFO_CID]
    ldr r6, [r5, #4]
    str r6, [r4, #(SD_INFO_CID + 4)]
    ldr r6, [r5, #8]
    str r6, [r4, #(SD_INFO_CID + 8)]
    ldr r6, [r5, #12]
    str r6, [r4, #(SD_INFO_CID + 12)]

    movs r0, #SD_CMD_SEND_RELATIVE_ADDR
    movs r1, #0
    movs r2, #SDMMC_CMD_WAITRESP_SHORT
    movs r3, #0
    bl sdmmc_send_cmd
    cmp r0, #0
    bne sd_fail

    ldr r5, =SDMMC_RESP1
    ldr r6, [r5]
    ldr r5, =0xFFFF0000
    and r6, r6, r5
    str r6, [r4, #SD_INFO_RCA]

    movs r0, #SD_CMD_SEND_CSD
    mov r1, r6
    movs r2, #SDMMC_CMD_WAITRESP_LONG
    movs r3, #0
    bl sdmmc_send_cmd
    cmp r0, #0
    bne sd_fail

    ldr r5, =SDMMC_RESP1
    ldr r6, [r5]
    str r6, [r4, #SD_INFO_CSD]
    ldr r6, [r5, #4]
    str r6, [r4, #(SD_INFO_CSD + 4)]
    ldr r6, [r5, #8]
    str r6, [r4, #(SD_INFO_CSD + 8)]
    ldr r6, [r5, #12]
    str r6, [r4, #(SD_INFO_CSD + 12)]

    add r0, r4, #SD_INFO_CSD
    bl sd_csd_capacity_blocks
    str r0, [r4, #SD_INFO_CAPACITY]

    movs r0, #SD_CMD_SELECT_CARD
    ldr r1, [r4, #SD_INFO_RCA]
    movs r2, #SDMMC_CMD_WAITRESP_SHORT
    movs r3, #0
    bl sdmmc_send_cmd
    cmp r0, #0
    bne sd_fail

    movs r0, #SD_CMD_APP_CMD
    ldr r1, [r4, #SD_INFO_RCA]
    movs r2, #SDMMC_CMD_WAITRESP_SHORT
    movs r3, #0
    bl sdmmc_send_cmd
    cmp r0, #0
    bne bus_width_done

    movs r0, #SD_ACMD_SET_BUS_WIDTH
    movs r1, #2
    movs r2, #SDMMC_CMD_WAITRESP_SHORT
    movs r3, #0
    bl sdmmc_send_cmd
    cmp r0, #0
    bne bus_width_done
    movs r5, #4
    strb r5, [r4, #SD_INFO_BUS_WIDTH]
bus_width_done:
    ldr r5, =SDMMC_CLKCR
    ldr r6, =g_sd_data_clkdiv
    ldr r6, [r6]
    ldrb r7, [r4, #SD_INFO_BUS_WIDTH]
    cmp r7, #4
    bne clk_apply
    orr r6, r6, #SDMMC_CLKCR_WIDBUS_4
clk_apply:
    orr r6, r6, #SDMMC_CLKCR_CLKEN
    str r6, [r5]

    ldrb r5, [r4, #SD_INFO_HIGH_CAPACITY]
    cmp r5, #0
    bne init_done

    movs r0, #SD_CMD_SET_BLOCKLEN
    ldr r1, =SD_BLOCK_SIZE
    movs r2, #SDMMC_CMD_WAITRESP_SHORT
    movs r3, #0
    bl sdmmc_send_cmd
    cmp r0, #0
    bne sd_fail

init_done:
    movs r5, #1
    strb r5, [r4, #SD_INFO_INITIALIZED]
    movs r0, #0
    ldr r5, =g_sd_last_error
    str r0, [r5]
    pop {r4-r7, pc}

sd_fail:
    ldr r5, =g_sd_last_error
    str r0, [r5]
    pop {r4-r7, pc}

sd_csd_capacity_blocks:
    push {r4-r7, lr}

    ldr r1, [r0]
    lsrs r1, r1, #30
    and r1, r1, #3
    cmp r1, #1
    beq csd_v2

    ldr r2, [r0, #4]
    ldr r3, [r0, #8]
    ldr r4, =0x3FF
    and r4, r2, r4
    lsls r4, r4, #2
    lsrs r5, r3, #30
    orr r4, r4, r5

    lsrs r5, r2, #16
    and r5, r5, #0xF
    movs r6, #1
    lsls r6, r6, r5

    lsrs r5, r3, #15
    and r5, r5, #0x7
    adds r5, r5, #2
    movs r7, #1
    lsls r7, r7, r5

    adds r4, r4, #1
    mul r4, r4, r7
    mul r4, r4, r6
    lsrs r4, r4, #9
    mov r0, r4
    pop {r4-r7, pc}

csd_v2:
    ldr r2, [r0, #4]
    ldr r3, [r0, #8]
    and r4, r2, #0x3F
    lsls r4, r4, #16
    lsrs r5, r3, #16
    orr r4, r4, r5
    adds r4, r4, #1
    lsls r4, r4, #10
    mov r0, r4
    pop {r4-r7, pc}

