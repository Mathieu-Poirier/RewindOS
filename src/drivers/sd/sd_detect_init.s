.syntax unified
.cpu cortex-m7
.thumb

.global sd_detect_init
.type sd_detect_init, %function

.extern enable_gpioc

sd_detect_init:
    push {lr}
    bl enable_gpioc

    /* GPIOC base: 0x40020800 */
    ldr r0, =0x40020800

    /* MODER13 = 00b (input) */
    ldr r1, [r0]
    ldr r2, =0x0C000000
    bic r1, r1, r2
    str r1, [r0]

    /* PUPDR13 = 01b (pull-up) */
    ldr r1, [r0, #0x0C]
    ldr r2, =0x0C000000
    bic r1, r1, r2
    ldr r2, =0x04000000
    orr r1, r1, r2
    str r1, [r0, #0x0C]

    pop {pc}
