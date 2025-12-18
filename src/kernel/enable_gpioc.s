    .syntax unified
    .cpu cortex-m7
    .thumb

    .global enable_gpioc
    .type   enable_gpioc, %function

enable_gpioc:
    ldr r0, =0x40023830      @ RCC_AHB1ENR
    ldr r1, [r0]
    orr r1, r1, #(1 << 2)   @ GPIOCEN = 1
    str r1, [r0]
    bx lr
