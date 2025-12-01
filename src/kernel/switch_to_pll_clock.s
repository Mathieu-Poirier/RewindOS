    .syntax unified
    .cpu cortex-m7
    .thumb

    .global switch_to_pll_clock      @ export the symbol so C can link to it
    .type   switch_to_pll_clock, %function

switch_to_pll_clock:
    ldr   r0, =0x40023808        @ RCC_CFGR

    @ ---- Set SW = 10 (PLL) ----
    ldr   r1, [r0]               @ read RCC_CFGR
    bic   r1, r1, #3             @ clear SW[1:0]
    orr   r1, r1, #2             @ SW = 10 (PLL)
    str   r1, [r0]               @ write RCC_CFGR

wait_sws_pll:
    ldr   r1, [r0]               @ read RCC_CFGR again
    ands  r1, r1, #(3 << 2)      @ mask SWS[3:2]
    cmp   r1, #(2 << 2)          @ expect SWS = 10
    bne   wait_sws_pll           @ loop until system clock is PLL

    bx    lr
