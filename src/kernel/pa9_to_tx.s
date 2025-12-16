    .syntax unified
    .cpu cortex-m7
    .thumb

    .global pa9_to_tx
    .type   pa9_to_tx, %function

pa9_to_tx:
    ldr r0, =0x40020000
    ldr r1, [r0]
    @ We now have to set bit 19 and 18 to 10
    @ Clear bit 18 and 19 
    bic r1, r1, #(3 << 18) @ clear bits 19:18
    @ Set and store
    orr r1, r1, #(2 << 18) @ set MODER9 = 10b (alternate function)
    str r1, [r0]

    @ 
    ldr r0, =0x40020024
    ldr r1, [r0]

    bic r1, r1, #(0xF << 4) @ clear bits 7:4 (AFR9)
    orr r1, r1, #(7 << 4)   @ set AFR9 = 0b0111 (AF7)
    str r1, [r0]
    
    bx lr

    