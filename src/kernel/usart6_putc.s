.global usart6_putc
.type   usart6_putc, %function

usart6_putc:
    ldr r1, =0x4001141C      @ USART6_ISR
    movs r3, #0x80           @ TXE mask (bit 7)

1:  ldr r2, [r1]
    tst r2, r3
    beq 1b

    ldr r1, =0x40011428      @ USART6_TDR
    strb r0, [r1]
    bx lr

