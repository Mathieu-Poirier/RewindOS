.syntax unified
.cpu cortex-m7
.thumb

.global usart6_init
.type   usart6_init, %function

usart6_init:
    @ r0 = BRR

    @ BRR
    ldr r1, =0x4001140C      @ USART6_BRR
    str r0, [r1]

    @ CR1: UE|RE|TE
    ldr r1, =0x40011400      @ USART6_CR1
    ldr r2, [r1]
    movs r3, #0x0D           @ (1<<0)|(1<<2)|(1<<3)
    orr  r2, r2, r3
    str  r2, [r1]
    bx lr
