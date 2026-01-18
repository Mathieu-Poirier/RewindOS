.syntax unified
.cpu cortex-m7
.thumb

.global sd_last_cmd
.type sd_last_cmd, %function
.extern g_sd_last_cmd

sd_last_cmd:
    ldr r0, =g_sd_last_cmd
    ldr r0, [r0]
    bx lr
