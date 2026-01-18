.syntax unified
.cpu cortex-m7
.thumb

.global sd_is_detected
.type sd_is_detected, %function

sd_is_detected:
    /* GPIOC IDR: 0x40020810 */
    ldr r0, =0x40020810
    ldr r1, [r0]
    lsrs r1, r1, #13
    and r1, r1, #1
    cmp r1, #0
    beq sd_present
    movs r0, #0
    bx lr
sd_present:
    movs r0, #1
    bx lr
