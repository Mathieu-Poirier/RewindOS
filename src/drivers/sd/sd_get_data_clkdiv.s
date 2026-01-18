.syntax unified
.cpu cortex-m7
.thumb

.global sd_get_data_clkdiv
.type sd_get_data_clkdiv, %function
.extern g_sd_data_clkdiv

sd_get_data_clkdiv:
    ldr r0, =g_sd_data_clkdiv
    ldr r0, [r0]
    bx lr
