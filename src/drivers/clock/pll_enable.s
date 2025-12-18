    .syntax unified
    .cpu cortex-m7
    .thumb

    .global pll_enable      @ export the symbol so C can link to it
    .type   pll_enable, %function

pll_enable:
    ldr   r0, =0x40023800        @ RCC_CR
    ldr   r1, [r0]
    orr   r1, r1, #(1 << 24)     @ PLLON = bit 24
    str   r1, [r0]

wait_pllrd:
    ldr   r1, [r0]
    tst   r1, #(1 << 25)         @ PLLRDY = bit 25
    beq   wait_pllrd
    bx    lr
