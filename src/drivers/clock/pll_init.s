    .syntax unified
    .cpu cortex-m7
    .thumb

    .global pll_init      @ export the symbol so C can link to it
    .type   pll_init, %function

pll_init:
    ldr   r0, =0x40023804            @ RCC_PLLCFGR

    @ Build PLLCFGR value:
    @ PLLSRC = 1 (HSE)
    @ PLLM = 25
    @ PLLN = 432
    @ PLLP = 2 (00)
    @ PLLQ = 9 (optional for USB/SDMMC)

    @ We construct:
    @ [31:24 reserved]
    @ PLLQ = 9  << 24
    @ PLLSRC = 1 << 22
    @ PLLP = 0 << 16     (division factor = 2)
    @ PLLN = 432 << 6
    @ PLLM = 25

    ldr   r1, =( (9 << 24) | (1 << 22) | (432 << 6) | (25) )
    str   r1, [r0]

    bx    lr
