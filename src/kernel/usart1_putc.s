.global usart1_putc
.type   usart1_putc, %function

usart1_putc:
    ldr r1, =0x40011000      @ USART1_SR
1:
    ldr r2, [r1]
    tst r2, #(1 << 7)        @ TXE
    beq 1b

    ldr r1, =0x40011004      @ USART1_DR
    str r0, [r1]
    bx lr
