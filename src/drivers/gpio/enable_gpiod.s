.syntax unified
.cpu cortex-m7
.thumb

.global enable_gpiod
.type   enable_gpiod, %function

enable_gpiod:
    ldr r0, =0x40023830      @ RCC_AHB1ENR
    ldr r1, [r0]
    orr r1, r1, #(1 << 3)   @ GPIODEN = 1
    str r1, [r0]
    bx lr
