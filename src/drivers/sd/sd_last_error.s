.syntax unified
.cpu cortex-m7
.thumb

.global sd_last_error
.type sd_last_error, %function
.extern g_sd_last_error

sd_last_error:
    ldr r1, =g_sd_last_error
    ldr r0, [r1]
    bx lr
