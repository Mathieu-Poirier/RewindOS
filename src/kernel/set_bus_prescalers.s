    .syntax unified
    .cpu cortex-m7
    .thumb

    .global set_bus_prescalers      @ export the symbol so C can link to it
    .type   set_bus_prescalers, %function

set_bus_prescalers:
    ldr   r0, =0x40023808        @ RCC_CFGR

    @ Build CFGR value for prescalers:
    @ HPRE  = 0   << 4    (AHB  prescaler = /1)
    @ PPRE1 = 4   << 10   (APB1 prescaler = /4)
    @ PPRE2 = 4   << 13   (APB2 prescaler = /2)

    @ We construct:
    @ [31:16 reserved]
    @ PPRE2 = 4 << 13
    @ PPRE1 = 4 << 10
    @ HPRE  = 0 << 4
    @ (SW will be set later in switch_to_pll_clock)

    ldr   r1, [r0]                               @ load current CFGR
    bic   r1, r1, #(0xF << 4)                    @ clear HPRE
    bic   r1, r1, #(0x7 << 10)                   @ clear PPRE1
    bic   r1, r1, #(0x7 << 13)                   @ clear PPRE2

    orr   r1, r1, #((4 << 13) | (4 << 10) | (0 << 4))
    str   r1, [r0]

    bx    lr


