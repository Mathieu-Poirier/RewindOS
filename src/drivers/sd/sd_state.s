.syntax unified
.cpu cortex-m7
.thumb

.section .bss
.align 4

.global g_sd_info
.global g_sd_last_error
.global g_sd_last_cmd
.global g_sd_last_sta
.global g_sd_init_clkdiv
.global g_sd_data_clkdiv
.global g_sd_use_pll48
.type g_sd_info, %object
.type g_sd_last_error, %object
.type g_sd_last_cmd, %object
.type g_sd_last_sta, %object
.type g_sd_init_clkdiv, %object
.type g_sd_data_clkdiv, %object
.type g_sd_use_pll48, %object

g_sd_info:
    .space 48
g_sd_last_error:
    .space 4
g_sd_last_cmd:
    .space 4
g_sd_last_sta:
    .space 4
g_sd_init_clkdiv:
    .space 4
g_sd_data_clkdiv:
    .space 4
g_sd_use_pll48:
    .space 4

.size g_sd_info, 48
.size g_sd_last_error, 4
.size g_sd_last_cmd, 4
.size g_sd_last_sta, 4
.size g_sd_init_clkdiv, 4
.size g_sd_data_clkdiv, 4
.size g_sd_use_pll48, 4
