.syntax unified
.cpu cortex-m7
.thumb

.global sd_read_blocks
.type sd_read_blocks, %function

.extern g_sd_info
.extern g_sd_last_error
.extern sdmmc_send_cmd
.extern sd_wait_card_ready

.equ SDMMC_BASE, 0x40012C00
.equ SDMMC_CLKCR,  (SDMMC_BASE + 0x04)
.equ SDMMC_DTIMER, (SDMMC_BASE + 0x24)
.equ SDMMC_DLEN,   (SDMMC_BASE + 0x28)
.equ SDMMC_DCTRL,  (SDMMC_BASE + 0x2C)
.equ SDMMC_STA,    (SDMMC_BASE + 0x34)
.equ SDMMC_ICR,    (SDMMC_BASE + 0x38)
.equ SDMMC_MASK,  (SDMMC_BASE + 0x3C)
.equ SDMMC_FIFO,   (SDMMC_BASE + 0x80)

.equ SDMMC_STA_DCRCFAIL, (1 << 1)
.equ SDMMC_STA_DTIMEOUT, (1 << 3)
.equ SDMMC_STA_RXOVERR,  (1 << 5)
.equ SDMMC_STA_DATAEND,  (1 << 8)
.equ SDMMC_STA_RXDAVL,   (1 << 21)
.equ SDMMC_STA_DATAERR_MASK, ((1 << 1) | (1 << 3) | (1 << 5))

.equ SDMMC_DCTRL_DTEN, (1 << 0)
.equ SDMMC_DCTRL_DTDIR, (1 << 1)
.equ SDMMC_DCTRL_DBLOCKSIZE_512, (9 << 4)
.equ SDMMC_DCTRL_READ, (SDMMC_DCTRL_DBLOCKSIZE_512 | SDMMC_DCTRL_DTDIR | SDMMC_DCTRL_DTEN)

@ Hardware data-timeout written to SDMMC_DTIMER register.
@ Counts in SDMMC_CK cycles (24 MHz).  250 ms = 6 000 000 cycles.
.equ SDMMC_HW_DTIMER,  6000000

@ Software poll-loop limit.  Must outlast SDMMC_HW_DTIMER so the
@ hardware DTIMEOUT flag fires first and the loop sees it in STA.
@ At 216 MHz CPU ≈ 280 ms.
.equ SDMMC_SW_TIMEOUT, 10000000

.equ SDMMC_CMD_WAITRESP_SHORT, (1 << 6)

.equ SD_CMD_READ_SINGLE_BLOCK, 17
.equ SD_BLOCK_SIZE, 512

.equ SD_ERR_TIMEOUT, -1
.equ SD_ERR_CMD, -3
.equ SD_ERR_DATA, -4
.equ SD_ERR_NO_INIT, -5
.equ SD_ERR_PARAM, -6

.equ SDMMC_CLKCR_HWFC_EN, (1 << 14)

.equ SD_INFO_HIGH_CAPACITY, 44
.equ SD_INFO_INITIALIZED, 46

sd_read_blocks:
    push {r4-r7, lr}

    ldr r4, =g_sd_info
    ldrb r5, [r4, #SD_INFO_INITIALIZED]
    cmp r5, #0
    beq err_no_init
    cmp r1, #0
    beq err_param
    cmp r2, #0
    beq err_param
    ands r5, r2, #3
    bne err_param

    ldrb r5, [r4, #SD_INFO_HIGH_CAPACITY]
    mov r6, r0
    mov r7, r1
    mov r4, r2

read_loop:
    mov r0, r6
    cmp r5, #0
    beq addr_byte
    b addr_ready
addr_byte:
    lsls r0, r0, #9
addr_ready:
    mov r1, r4
    bl sd_read_block
    cmp r0, #0
    bne err_return

    adds r6, r6, #1
    add r4, r4, #512
    subs r7, r7, #1
    bne read_loop

    movs r0, #0
    b store_exit

err_no_init:
    ldr r0, =SD_ERR_NO_INIT
    b store_exit
err_param:
    ldr r0, =SD_ERR_PARAM
    b store_exit
err_return:
    b store_exit

store_exit:
    ldr r1, =g_sd_last_error
    str r0, [r1]
    pop {r4-r7, pc}

sd_read_block:
    push {r4-r7, lr}

    mov r4, r0
    mov r5, r1

    @ ---- Full peripheral cleanup (mirrors sd_async sd_start_single_block_read) ----

    @ 1. Disable async interrupt mask to prevent ISR interference
    ldr r0, =SDMMC_MASK
    movs r1, #0
    str r1, [r0]

    @ 2. Stop DPSM — clears FIFOs per RM0410 (Idle state clears FIFOs)
    ldr r0, =SDMMC_DCTRL
    movs r1, #0
    str r1, [r0]

    @ 3. Clear all status flags
    ldr r0, =SDMMC_ICR
    ldr r1, =0xFFFFFFFF
    str r1, [r0]

    @ 4. Drain any residual data from FIFO
    ldr r0, =SDMMC_STA
    ldr r1, =SDMMC_FIFO
drain_fifo:
    ldr r2, [r0]
    tst r2, #SDMMC_STA_RXDAVL
    beq fifo_drained
    ldr r2, [r1]
    b drain_fifo
fifo_drained:

    @ 5. Enable hardware flow control to prevent RXOVERR
    ldr r0, =SDMMC_CLKCR
    ldr r1, [r0]
    orr r1, r1, #SDMMC_CLKCR_HWFC_EN
    str r1, [r0]

    @ 6. Wait for card to be in TRAN state (ready for new command)
    @    Critical after writes — card may still be programming.
    bl sd_wait_card_ready
    @ Ignore error — if card-ready fails, CMD17 will also fail
    @ and we return a clear error.

    @ ---- Set up data transfer ----

    ldr r0, =SDMMC_DTIMER
    ldr r1, =SDMMC_HW_DTIMER
    str r1, [r0]

    ldr r0, =SDMMC_DLEN
    ldr r1, =SD_BLOCK_SIZE
    str r1, [r0]

    ldr r0, =SDMMC_DCTRL
    ldr r1, =SDMMC_DCTRL_READ
    str r1, [r0]

    movs r0, #SD_CMD_READ_SINGLE_BLOCK
    mov r1, r4
    movs r2, #SDMMC_CMD_WAITRESP_SHORT
    movs r3, #0
    bl sdmmc_send_cmd
    cmp r0, #0
    bne read_cmd_err

    ldr r0, =SDMMC_STA
    ldr r2, =SDMMC_STA_RXDAVL
    ldr r3, =SDMMC_FIFO
    movs r6, #128

read_word:
    ldr r7, =SDMMC_SW_TIMEOUT
wait_data:
    ldr r1, [r0]
    tst r1, r2
    bne data_avail
    tst r1, #SDMMC_STA_DATAERR_MASK
    bne data_err
    subs r7, r7, #1
    bne wait_data
    b data_timeout

data_avail:
    ldr r1, [r3]
    str r1, [r5], #4
    subs r6, r6, #1
    bne read_word

    ldr r7, =SDMMC_SW_TIMEOUT
wait_end:
    ldr r1, [r0]
    tst r1, #SDMMC_STA_DATAEND
    bne data_done
    tst r1, #SDMMC_STA_DATAERR_MASK
    bne data_err
    subs r7, r7, #1
    bne wait_end
    b data_timeout

data_done:
    ldr r0, =SDMMC_DCTRL
    movs r1, #0
    str r1, [r0]
    ldr r0, =SDMMC_ICR
    ldr r1, =0xFFFFFFFF
    str r1, [r0]
    movs r0, #0
    pop {r4-r7, pc}

data_err:
    ldr r0, =SDMMC_DCTRL
    movs r1, #0
    str r1, [r0]
    ldr r0, =SDMMC_ICR
    ldr r1, =0xFFFFFFFF
    str r1, [r0]
    ldr r0, =SD_ERR_DATA
    pop {r4-r7, pc}

data_timeout:
    ldr r0, =SDMMC_DCTRL
    movs r1, #0
    str r1, [r0]
    ldr r0, =SDMMC_ICR
    ldr r1, =0xFFFFFFFF
    str r1, [r0]
    ldr r0, =SD_ERR_TIMEOUT
    pop {r4-r7, pc}

read_cmd_err:
    @ CMD17 failed — clean up DCTRL (DPSM is still armed with DTEN=1)
    mov r6, r0
    ldr r0, =SDMMC_DCTRL
    movs r1, #0
    str r1, [r0]
    ldr r0, =SDMMC_ICR
    ldr r1, =0xFFFFFFFF
    str r1, [r0]
    ldr r0, =SD_ERR_CMD
    pop {r4-r7, pc}
