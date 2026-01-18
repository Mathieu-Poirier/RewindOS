.syntax unified
.cpu cortex-m7
.thumb

.global sd_get_init_clkdiv
.type sd_get_init_clkdiv, %function
.extern g_sd_init_clkdiv

sd_get_init_clkdiv:
    ldr r0, =g_sd_init_clkdiv
    ldr r0, [r0]
    bx lr
