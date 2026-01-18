.syntax unified
.cpu cortex-m7
.thumb

.global enable_sdmmc1_kerclk_pll48
.type   enable_sdmmc1_kerclk_pll48, %function

enable_sdmmc1_kerclk_pll48:
    /* Ensure PLL48CLK is available without switching SYSCLK. */
    ldr r0, =0x40023800      @ RCC_CR
    ldr r1, [r0]
    tst r1, #(1 << 25)       @ PLLRDY
    bne pll_ready
    tst r1, #(1 << 24)       @ PLLON
    bne pll_wait

    /* Make sure HSI is on. */
    orr r1, r1, #(1 << 0)    @ HSION
    str r1, [r0]

    ldr r2, =0x100000
1:
    ldr r1, [r0]
    tst r1, #(1 << 1)        @ HSIRDY
    bne hsi_ready
    subs r2, r2, #1
    bne 1b
hsi_ready:
    /* Configure PLL: HSI=16, M=16, N=192, P=2, Q=4 (PLL48CLK=48 MHz). */
    ldr r0, =0x40023804      @ RCC_PLLCFGR
    ldr r1, =0x04003010
    str r1, [r0]

    ldr r0, =0x40023800      @ RCC_CR
    ldr r1, [r0]
    orr r1, r1, #(1 << 24)   @ PLLON
    str r1, [r0]

pll_wait:
    ldr r2, =0x100000
2:
    ldr r1, [r0]
    tst r1, #(1 << 25)       @ PLLRDY
    bne pll_ready
    subs r2, r2, #1
    bne 2b

pll_ready:
    /* Select PLL48CLK for SDMMC1 kernel clock (SDMMC1SEL=0). */
    ldr r0, =0x40023890      @ RCC_DCKCFGR2
    ldr r1, [r0]
    bic r1, r1, #(1 << 28)
    str r1, [r0]
    bx lr
