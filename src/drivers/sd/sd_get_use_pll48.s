.syntax unified
.cpu cortex-m7
.thumb

.global sd_get_use_pll48
.type sd_get_use_pll48, %function
.extern g_sd_use_pll48

sd_get_use_pll48:
    ldr r0, =g_sd_use_pll48
    ldr r0, [r0]
    bx lr
