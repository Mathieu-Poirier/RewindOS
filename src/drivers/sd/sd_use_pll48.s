.syntax unified
.cpu cortex-m7
.thumb

.global sd_use_pll48
.type sd_use_pll48, %function
.extern g_sd_use_pll48

sd_use_pll48:
    ldr r1, =g_sd_use_pll48
    str r0, [r1]
    bx lr
