    .syntax unified
    .cpu cortex-m7
    .thumb

    .global flash_latency_init     @ export the symbol so C can link to it
    .type   flash_latency_init, %function

flash_latency_init:
    ldr   r0, =0x40023C00        @ FLASH_ACR

    @ Build FLASH_ACR value:
    @ LATENCY = 7     (bits 3:0)
    @ PRFTEN  = 1     (bit 8)
    @ ICEN    = 1     (bit 9)
    @ DCEN    = 1     (bit 10)

    ldr   r1, =( (7) | (1 << 8) | (1 << 9) | (1 << 10) )
    str   r1, [r0]

    bx    lr




