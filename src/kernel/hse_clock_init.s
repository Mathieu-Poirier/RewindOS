    .syntax unified
    .cpu cortex-m7
    .thumb

    .global hse_clock_init      @ export the symbol so C can link to it
    .type   hse_clock_init, %function

hse_clock_init:
    ldr   r0, =0x40023800        @ RCC_CR
    ldr   r1, [r0]               @ load RCC_CR
    orr   r1, r1, #(1 << 16)     @ set HSEON
    str   r1, [r0]               @ write RCC_CR back

wait_hse_ready:
    ldr   r1, [r0]               @ read RCC_CR again
    tst   r1, #(1 << 17)         @ HSERDY?
    beq   wait_hse_ready         @ loop until stable
    bx    lr
