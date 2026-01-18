.syntax unified
.cpu cortex-m7
.thumb

.global sd_set_init_clkdiv
.type sd_set_init_clkdiv, %function
.extern g_sd_init_clkdiv

sd_set_init_clkdiv:
    ldr r1, =g_sd_init_clkdiv
    str r0, [r1]
    bx lr
