.syntax unified
.cpu cortex-m7
.thumb

.global pd2_to_sdmmc1
.type   pd2_to_sdmmc1, %function

pd2_to_sdmmc1:
    /* GPIOD base: 0x40020C00 */
    ldr r0, =0x40020C00

    /* MODER2 = 10b (AF) */
    ldr r1, [r0]
    bic r1, r1, #(3 << 4)
    orr r1, r1, #(2 << 4)
    str r1, [r0]

    /* OTYPER2 = 0 (push-pull) */
    ldr r1, [r0, #0x04]
    bic r1, r1, #(1 << 2)
    str r1, [r0, #0x04]

    /* OSPEEDR2 = 11b (high speed) */
    ldr r1, [r0, #0x08]
    bic r1, r1, #(3 << 4)
    orr r1, r1, #(3 << 4)
    str r1, [r0, #0x08]

    /* PUPDR2 = 01b (pull-up) */
    ldr r1, [r0, #0x0C]
    bic r1, r1, #(3 << 4)
    orr r1, r1, #(1 << 4)
    str r1, [r0, #0x0C]

    /* AFRL2 = AF12 */
    ldr r1, [r0, #0x20]
    bic r1, r1, #(0xF << 8)
    orr r1, r1, #(12 << 8)
    str r1, [r0, #0x20]

    bx lr
