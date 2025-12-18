.syntax unified
.cpu cortex-m7
.thumb

.global usart6_getc
.type   usart6_getc, %function

usart6_getc:
    ldr r1, =0x4001141C      @ USART6_ISR
    movs r3, #0x20           @ RXNE mask (bit 5)

1:  ldr r2, [r1]
    tst r2, r3
    beq 1b

    ldr r1, =0x40011424      @ USART6_RDR
    ldrb r0, [r1]            @ return received byte in r0
    bx lr
