    .syntax unified
    .cpu cortex-m7
    .thumb

    .global systick_init        @ export the symbol so C can link to it
    .type   systick_init, %function

systick_init:
    ldr  r1, =0xE000E014    @ load into r1 address of SYST_RVR 32 immeadiate
    str  r0, [r1]           @ *SYST_RVR = ticks (r0)
    @ Need to initializae SYST_CVR to clear the register
    ldr  r1, =0xE000E018
    movs r2, #0
    str r2, [r1]
    @ CSR to start the Systick timer
    ldr r1, =0xE000E010
    movs r3, #7
    str r3, [r1]
    
    bx   lr                 @ return

    
