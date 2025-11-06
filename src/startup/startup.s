    .syntax unified
    .cpu cortex-m7
    .thumb

    .section .isr_vector, "a", %progbits
    .type   g_pfnVectors, %object

/* Interrupt vector table at 0x08000000 */
g_pfnVectors:
    .word   _estack             /* Initial stack pointer */
    .word   Reset_Handler       /* Reset handler */
    .word   NMI_Handler
    .word   HardFault_Handler
    .word   MemManage_Handler
    .word   BusFault_Handler
    .word   UsageFault_Handler
    .word   0                   /* Reserved */
    .word   0
    .word   0
    .word   0
    .word   SVC_Handler
    .word   DebugMon_Handler
    .word   0                   /* Reserved */
    .word   PendSV_Handler
    .word   SysTick_Handler

/* You can extend this table with peripheral IRQ handlers later */

    .size g_pfnVectors, . - g_pfnVectors

/* Reset handler: initializes RAM and jumps to main */
    .text
    .thumb_func
    .global Reset_Handler
Reset_Handler:
    /* Copy .data from FLASH to RAM */
    ldr r0, =_sidata   /* source in flash */
    ldr r1, =_sdata    /* dest in RAM */
    ldr r2, =_edata
1:
    cmp r1, r2
    ittt lt
    ldrlt r3, [r0], #4
    strlt r3, [r1], #4
    blt 1b

    /* Zero initialize .bss */
    ldr r1, =_sbss
    ldr r2, =_ebss
    movs r3, #0
2:
    cmp r1, r2
    itt lt
    strlt r3, [r1], #4
    blt 2b

    /* Call main */
    bl main

    /* If main ever returns, loop forever */
LoopForever:
    b LoopForever

/* Default handlers (just loop) */
    .thumb_func
NMI_Handler:          b .
HardFault_Handler:    b .
MemManage_Handler:    b .
BusFault_Handler:     b .
UsageFault_Handler:   b .
SVC_Handler:          b .
DebugMon_Handler:     b .
PendSV_Handler:       b .
SysTick_Handler:      b .
