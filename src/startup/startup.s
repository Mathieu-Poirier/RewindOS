.syntax unified
.cpu cortex-m7
.thumb

.global g_pfnVectors
.global Reset_Handler

/* ----------------- Vector table ----------------- */
.section .isr_vector, "a", %progbits
.type g_pfnVectors, %object
g_pfnVectors:
    .word _estack
    .word Reset_Handler
    .word NMI_Handler
    .word HardFault_Handler
    .word MemManage_Handler
    .word BusFault_Handler
    .word UsageFault_Handler
    .word 0, 0, 0, 0
    .word SVC_Handler
    .word DebugMon_Handler
    .word 0
    .word PendSV_Handler
    .word SysTick_Handler
.size g_pfnVectors, . - g_pfnVectors

/* ----------------- Default handlers ----------------- */
.section .text.Default_Handler, "ax", %progbits
.thumb_func
Default_Handler:
    b .

.weak NMI_Handler, HardFault_Handler, MemManage_Handler, BusFault_Handler
.weak UsageFault_Handler, SVC_Handler, DebugMon_Handler, PendSV_Handler, SysTick_Handler
.set NMI_Handler, Default_Handler
.set HardFault_Handler, Default_Handler
.set MemManage_Handler, Default_Handler
.set BusFault_Handler, Default_Handler
.set UsageFault_Handler, Default_Handler
.set SVC_Handler, Default_Handler
.set DebugMon_Handler, Default_Handler
.set PendSV_Handler, Default_Handler
.set SysTick_Handler, Default_Handler

/* ----------------- Reset handler ----------------- */
.section .text.Reset_Handler, "ax", %progbits
.thumb_func
Reset_Handler:
    /* Enable FPU */
    ldr r0, =0xE000ED88
    ldr r1, [r0]
    orr r1, r1, #(0xF << 20)
    str r1, [r0]
    dsb sy
    isb

    /* Optionally set VTOR */
    ldr r0, =0xE000ED08
    ldr r1, =g_pfnVectors
    str r1, [r0]
    dsb sy
    isb

    /* Copy .data (FLASH → RAM) */
    ldr r0, =_sidata
    ldr r1, =_sdata
    ldr r2, =_edata
1:  cmp r1, r2
    itt lt
    ldrlt r3, [r0], #4
    strlt r3, [r1], #4
    blt 1b

    /* Copy .sdram_data if any */
    ldr r0, =_sisdram_data
    ldr r1, =_ssdram_data
    ldr r2, =_esdram_data
2:  cmp r1, r2
    itt lt
    ldrlt r3, [r0], #4
    strlt r3, [r1], #4
    blt 2b

    /* Zero .bss */
    ldr r1, =_sbss
    ldr r2, =_ebss
    movs r3, #0
3:  cmp r1, r2
    itt lt
    strlt r3, [r1], #4
    blt 3b

    /* Jump directly to main */
    bl main

4:  b 4b
