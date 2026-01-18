.syntax unified
.cpu cortex-m7
.thumb

.global sd_set_data_clkdiv
.type sd_set_data_clkdiv, %function
.extern g_sd_data_clkdiv

sd_set_data_clkdiv:
    ldr r1, =g_sd_data_clkdiv
    str r0, [r1]
    bx lr
