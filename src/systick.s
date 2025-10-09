    .syntax unified
    .cpu cortex-m7
    .thumb

    .global systick_init        @ export the symbol so C can link to it
    .type   systick_init, %function

systick_init:
    ldr  r1, =0xE000E014    @ load into r1 address of SYST_RVR 32 immeadiate
    str  r0, [r1]           @ *SYST_RVR = ticks (r0)
    bx   lr                 @ return
