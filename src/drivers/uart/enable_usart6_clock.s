.syntax unified
.cpu cortex-m7
.thumb

.global enable_usart6_clock
.type   enable_usart6_clock, %function

enable_usart6_clock:
    ldr r0, =0x40023844      @ RCC_APB2ENR
    ldr r1, [r0]
    orr r1, r1, #(1 << 5)   @ USART6EN = 1
    str r1, [r0]
    bx lr

    