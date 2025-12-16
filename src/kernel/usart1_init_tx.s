.global usart1_init_tx
.type   usart1_init_tx, %function

usart1_init_tx:
    @ Set baud rate
    ldr r1, =0x40011008      @ USART1_BRR
    str r0, [r1]

    @ Enable TX and USART
    ldr r1, =0x4001100C      @ USART1_CR1
    ldr r2, [r1]
    orr r2, r2, #(1 << 3)    @ TE
    orr r2, r2, #(1 << 13)   @ UE
    str r2, [r1]

    bx lr
