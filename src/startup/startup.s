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
    /* External interrupts (IRQ 0-48) */
    .word Default_Handler       /* IRQ 0:  WWDG */
    .word Default_Handler       /* IRQ 1:  PVD */
    .word Default_Handler       /* IRQ 2:  TAMP_STAMP */
    .word Default_Handler       /* IRQ 3:  RTC_WKUP */
    .word Default_Handler       /* IRQ 4:  FLASH */
    .word Default_Handler       /* IRQ 5:  RCC */
    .word Default_Handler       /* IRQ 6:  EXTI0 */
    .word Default_Handler       /* IRQ 7:  EXTI1 */
    .word Default_Handler       /* IRQ 8:  EXTI2 */
    .word Default_Handler       /* IRQ 9:  EXTI3 */
    .word Default_Handler       /* IRQ 10: EXTI4 */
    .word Default_Handler       /* IRQ 11: DMA1_Stream0 */
    .word Default_Handler       /* IRQ 12: DMA1_Stream1 */
    .word Default_Handler       /* IRQ 13: DMA1_Stream2 */
    .word Default_Handler       /* IRQ 14: DMA1_Stream3 */
    .word Default_Handler       /* IRQ 15: DMA1_Stream4 */
    .word Default_Handler       /* IRQ 16: DMA1_Stream5 */
    .word Default_Handler       /* IRQ 17: DMA1_Stream6 */
    .word Default_Handler       /* IRQ 18: ADC */
    .word Default_Handler       /* IRQ 19: CAN1_TX */
    .word Default_Handler       /* IRQ 20: CAN1_RX0 */
    .word Default_Handler       /* IRQ 21: CAN1_RX1 */
    .word Default_Handler       /* IRQ 22: CAN1_SCE */
    .word Default_Handler       /* IRQ 23: EXTI9_5 */
    .word Default_Handler       /* IRQ 24: TIM1_BRK_TIM9 */
    .word Default_Handler       /* IRQ 25: TIM1_UP_TIM10 */
    .word Default_Handler       /* IRQ 26: TIM1_TRG_COM_TIM11 */
    .word Default_Handler       /* IRQ 27: TIM1_CC */
    .word Default_Handler       /* IRQ 28: TIM2 */
    .word Default_Handler       /* IRQ 29: TIM3 */
    .word Default_Handler       /* IRQ 30: TIM4 */
    .word Default_Handler       /* IRQ 31: I2C1_EV */
    .word Default_Handler       /* IRQ 32: I2C1_ER */
    .word Default_Handler       /* IRQ 33: I2C2_EV */
    .word Default_Handler       /* IRQ 34: I2C2_ER */
    .word Default_Handler       /* IRQ 35: SPI1 */
    .word Default_Handler       /* IRQ 36: SPI2 */
    .word Default_Handler       /* IRQ 37: USART1 */
    .word Default_Handler       /* IRQ 38: USART2 */
    .word Default_Handler       /* IRQ 39: USART3 */
    .word Default_Handler       /* IRQ 40: EXTI15_10 */
    .word Default_Handler       /* IRQ 41: RTC_Alarm */
    .word Default_Handler       /* IRQ 42: OTG_FS_WKUP */
    .word Default_Handler       /* IRQ 43: TIM8_BRK_TIM12 */
    .word Default_Handler       /* IRQ 44: TIM8_UP_TIM13 */
    .word Default_Handler       /* IRQ 45: TIM8_TRG_COM_TIM14 */
    .word Default_Handler       /* IRQ 46: TIM8_CC */
    .word Default_Handler       /* IRQ 47: DMA1_Stream7 */
    .word Default_Handler       /* IRQ 48: FMC */
    .word SDMMC1_IRQHandler     /* IRQ 49: SDMMC1 */
    .word Default_Handler       /* IRQ 50: TIM5 */
    .word Default_Handler       /* IRQ 51: SPI3 */
    .word Default_Handler       /* IRQ 52: UART4 */
    .word Default_Handler       /* IRQ 53: UART5 */
    .word Default_Handler       /* IRQ 54: TIM6_DAC */
    .word Default_Handler       /* IRQ 55: TIM7 */
    .word Default_Handler       /* IRQ 56: DMA2_Stream0 */
    .word Default_Handler       /* IRQ 57: DMA2_Stream1 */
    .word Default_Handler       /* IRQ 58: DMA2_Stream2 */
    .word Default_Handler       /* IRQ 59: DMA2_Stream3 */
    .word Default_Handler       /* IRQ 60: DMA2_Stream4 */
    .word Default_Handler       /* IRQ 61: ETH */
    .word Default_Handler       /* IRQ 62: ETH_WKUP */
    .word Default_Handler       /* IRQ 63: CAN2_TX */
    .word Default_Handler       /* IRQ 64: CAN2_RX0 */
    .word Default_Handler       /* IRQ 65: CAN2_RX1 */
    .word Default_Handler       /* IRQ 66: CAN2_SCE */
    .word Default_Handler       /* IRQ 67: OTG_FS */
    .word Default_Handler       /* IRQ 68: DMA2_Stream5 */
    .word Default_Handler       /* IRQ 69: DMA2_Stream6 */
    .word Default_Handler       /* IRQ 70: DMA2_Stream7 */
    .word USART6_IRQHandler     /* IRQ 71: USART6 */
.size g_pfnVectors, . - g_pfnVectors

/* ----------------- Default handlers ----------------- */
.section .text.Default_Handler, "ax", %progbits
.thumb_func
Default_Handler:
    b .

.weak NMI_Handler, HardFault_Handler, MemManage_Handler, BusFault_Handler
.weak UsageFault_Handler, SVC_Handler, DebugMon_Handler, PendSV_Handler, SysTick_Handler
.weak SDMMC1_IRQHandler, USART6_IRQHandler
.set NMI_Handler, Default_Handler
.set HardFault_Handler, Default_Handler
.set MemManage_Handler, Default_Handler
.set BusFault_Handler, Default_Handler
.set UsageFault_Handler, Default_Handler
.set SVC_Handler, Default_Handler
.set DebugMon_Handler, Default_Handler
.set PendSV_Handler, Default_Handler
.set SysTick_Handler, Default_Handler
.set SDMMC1_IRQHandler, Default_Handler
.set USART6_IRQHandler, Default_Handler

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
