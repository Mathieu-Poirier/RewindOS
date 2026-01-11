.syntax unified
.cpu cortex-m7
.thumb

.global enable_sdmmc1_clock
.type   enable_sdmmc1_clock, %function

enable_sdmmc1_clock:
    ldr  r0, =0x40023844      @ RCC_APB2ENR
    ldr  r1, [r0]
    orr  r1, r1, #(1 << 11)  @ SDMMC1EN
    str  r1, [r0]
    ldr  r1, [r0]            @ read-back
    bx   lr

