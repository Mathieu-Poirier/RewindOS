.syntax unified
.cpu cortex-m7
.thumb

.global sd_get_info
.type sd_get_info, %function
.extern g_sd_info

sd_get_info:
    ldr r0, =g_sd_info
    bx lr
