.syntax unified
.cpu cortex-m7
.thumb

.global usart6_flush_tx
.type   usart6_flush_tx, %function

/* USART6 base + registers (STM32F7) */
.equ USART6_BASE, 0x40011400
.equ USART_ISR,   0x1C
.equ USART_ICR,   0x20

.equ ISR_TC,      (1 << 6)
.equ ICR_TCCF,    (1 << 6)

usart6_flush_tx:
    /* Clear TC so we wait for *current* transmission */
    ldr r0, =USART6_BASE
    movs r1, #ICR_TCCF
    str  r1, [r0, #USART_ICR]

1:  /* Wait for Transmission Complete */
    ldr  r1, [r0, #USART_ISR]
    tst  r1, #ISR_TC
    beq  1b

    bx   lr
