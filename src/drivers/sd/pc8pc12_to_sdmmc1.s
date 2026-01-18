.syntax unified
.cpu cortex-m7
.thumb

.global pc8pc12_to_sdmmc1
.type   pc8pc12_to_sdmmc1, %function

pc8pc12_to_sdmmc1:
    /* GPIOC base: 0x40020800 */
    ldr r0, =0x40020800

    /* MODER8-12 = 10b (AF) */
    ldr r1, [r0]
    ldr r2, =0x03FF0000
    bic r1, r1, r2
    ldr r2, =0x02AA0000
    orr r1, r1, r2
    str r1, [r0]

    /* OTYPER8-12 = 0 (push-pull) */
    ldr r1, [r0, #0x04]
    ldr r2, =0x00001F00
    bic r1, r1, r2
    str r1, [r0, #0x04]

    /* OSPEEDR8-12 = 11b (high speed) */
    ldr r1, [r0, #0x08]
    ldr r2, =0x03FF0000
    bic r1, r1, r2
    orr r1, r1, r2
    str r1, [r0, #0x08]

    /* PUPDR8-11 = 01b (pull-up), PUPDR12 = 00b (no pull) */
    ldr r1, [r0, #0x0C]
    ldr r2, =0x03FF0000
    bic r1, r1, r2
    ldr r2, =0x00550000
    orr r1, r1, r2
    str r1, [r0, #0x0C]

    /* AFRH8-12 = AF12 */
    ldr r1, [r0, #0x24]
    ldr r2, =0x000FFFFF
    bic r1, r1, r2
    ldr r2, =0x000CCCCC
    orr r1, r1, r2
    str r1, [r0, #0x24]

    bx lr
