    .syntax unified
    .cpu cortex-m7
    .thumb

    .global pc6pc7_to_usart6
    .type   pc6pc7_to_usart6, %function

pc6pc7_to_usart6:
    /* GPIOC base: 0x40020800 */
    ldr r0, =0x40020800          /* GPIOC_MODER */
    ldr r1, [r0]

    /* Set MODER6 (bits 13:12) = 10b, MODER7 (bits 15:14) = 10b */
    bic r1, r1, #(3 << 12)       /* clear MODER6 */
    bic r1, r1, #(3 << 14)       /* clear MODER7 */
    orr r1, r1, #(2 << 12)       /* set MODER6 to AF */
    orr r1, r1, #(2 << 14)       /* set MODER7 to AF */
    str r1, [r0]

    /* GPIOC_AFRL at GPIOC base + 0x20 = 0x40020820 */
    ldr r0, =0x40020820
    ldr r1, [r0]

    /* AFRL6 (bits 27:24) = AF8, AFRL7 (bits 31:28) = AF8 */
    bic r1, r1, #(0xF << 24)     /* clear AFRL6 */
    bic r1, r1, #(0xF << 28)     /* clear AFRL7 */
    orr r1, r1, #(8 << 24)       /* AF8 for PC6 */
    orr r1, r1, #(8 << 28)       /* AF8 for PC7 */
    str r1, [r0]

    bx lr

    