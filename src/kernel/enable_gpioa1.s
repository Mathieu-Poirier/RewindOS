    .syntax unified
    .cpu cortex-m7
    .thumb

    .global enable_gpioa1     @ export the symbol so C can link to it
    .type   enable_gpioa1, %function

enable_gpioa1:
    ldr r0, =0x40023830      @ RCC_AHB1ENR
    ldr r1, [r0]
    orr r1, r1, #(1 << 0)    @ GPIOAEN = 1
    str r1, [r0]
    bx lr

    