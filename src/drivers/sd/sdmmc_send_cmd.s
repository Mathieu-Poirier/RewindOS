.syntax unified
.cpu cortex-m7
.thumb

.global sdmmc_send_cmd
.type sdmmc_send_cmd, %function

.extern g_sd_last_cmd
.extern g_sd_last_sta

.equ SDMMC_BASE, 0x40012C00
.equ SDMMC_ARG,  (SDMMC_BASE + 0x08)
.equ SDMMC_CMD,  (SDMMC_BASE + 0x0C)
.equ SDMMC_STA,  (SDMMC_BASE + 0x34)
.equ SDMMC_ICR,  (SDMMC_BASE + 0x38)

.equ SDMMC_CMD_CPSMEN, (1 << 10)

.equ SDMMC_STA_CCRCFAIL, (1 << 0)
.equ SDMMC_STA_CTIMEOUT, (1 << 2)
.equ SDMMC_STA_CMDREND,  (1 << 6)
.equ SDMMC_STA_CMDSENT,  (1 << 7)

.equ SDMMC_CMD_TIMEOUT, 5000000

sdmmc_send_cmd:
    push {r4-r6, lr}

    ldr r4, =SDMMC_ICR
    ldr r5, =0xFFFFFFFF
    str r5, [r4]

    ldr r4, =SDMMC_ARG
    str r1, [r4]

    ldr r4, =SDMMC_CMD
    orr r5, r0, r2
    orr r5, r5, #SDMMC_CMD_CPSMEN
    ldr r6, =g_sd_last_cmd
    str r5, [r6]
    str r5, [r4]

    ldr r4, =SDMMC_STA
    ldr r5, =SDMMC_CMD_TIMEOUT
1:
    ldr r6, [r4]
    tst r6, #SDMMC_STA_CTIMEOUT
    bne cmd_timeout
    cmp r2, #0
    beq check_sent
    tst r6, #SDMMC_STA_CMDREND
    bne cmd_done
    b check_crc
check_sent:
    tst r6, #SDMMC_STA_CMDSENT
    bne cmd_done
check_crc:
    tst r6, #SDMMC_STA_CCRCFAIL
    beq dec
    cmp r3, #0
    bne cmd_ok          @ CRC fail but ignored = command completed
    mvn r0, #1          @ SD_ERR_CRC = -2
    b cmd_exit

dec:
    subs r5, r5, #1
    bne 1b
    b cmd_timeout

cmd_done:
    tst r6, #SDMMC_STA_CCRCFAIL
    beq cmd_ok
    cmp r3, #0
    beq cmd_crc
cmd_ok:
    movs r0, #0
    b cmd_exit
cmd_crc:
    mvn r0, #1          @ SD_ERR_CRC = -2
    b cmd_exit
cmd_timeout:
    mvn r0, #0          @ SD_ERR_TIMEOUT = -1

cmd_exit:
    ldr r4, =g_sd_last_sta
    str r6, [r4]
    ldr r4, =SDMMC_ICR
    ldr r5, =0xFFFFFFFF
    str r5, [r4]
    pop {r4-r6, pc}
