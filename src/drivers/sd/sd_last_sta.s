.syntax unified
.cpu cortex-m7
.thumb

.global sd_last_sta
.type sd_last_sta, %function
.extern g_sd_last_sta

sd_last_sta:
    ldr r0, =g_sd_last_sta
    ldr r0, [r0]
    bx lr
