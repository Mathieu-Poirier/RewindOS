.syntax unified
.cpu cortex-m7
.thumb

.global enable_sdmmc1_kerclk_sysclk
.type   enable_sdmmc1_kerclk_sysclk, %function

enable_sdmmc1_kerclk_sysclk:
    /* Ensure HSI is on (SYSCLK source in bootloader). */
    ldr r0, =0x40023800      @ RCC_CR
    ldr r1, [r0]
    orr r1, r1, #(1 << 0)    @ HSION
    str r1, [r0]

    ldr r2, =0x100000
1:
    ldr r1, [r0]
    tst r1, #(1 << 1)        @ HSIRDY
    bne 2f
    subs r2, r2, #1
    bne 1b
2:
    /* Select SYSCLK for SDMMC1 kernel clock (SDMMC1SEL=1). */
    ldr r0, =0x40023890      @ RCC_DCKCFGR2
    ldr r1, [r0]
    orr r1, r1, #(1 << 28)
    str r1, [r0]
    bx lr
