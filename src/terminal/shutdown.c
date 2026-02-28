#include "../../include/shutdown.h"
#include "../../include/uart_async.h"
#include "../../include/uart.h"
#include "../../include/nvic.h"
#include "../../include/panic.h"

/* ---- RCC ----------------------------------------------------------------- */
#define RCC_BASE            0x40023800u
#define RCC_APB1ENR         (*(volatile uint32_t *)(RCC_BASE + 0x40u))
#define RCC_APB1ENR_PWREN   (1u << 28)

/* ---- PWR ----------------------------------------------------------------- */
#define PWR_BASE            0x40007000u
#define PWR_CR1             (*(volatile uint32_t *)(PWR_BASE + 0x00u))
#define PWR_CR2             (*(volatile uint32_t *)(PWR_BASE + 0x08u))
#define PWR_CSR2            (*(volatile uint32_t *)(PWR_BASE + 0x0Cu))

/* PWR_CR1 bits */
#define PWR_CR1_PDDS        (1u << 1)   /* 1 = Standby (vs Stop) on SLEEPDEEP */
#define PWR_CR1_CWUF        (1u << 2)   /* Write 1 to clear all wakeup flags   */
#define PWR_CR1_CSBF        (1u << 3)   /* Write 1 to clear standby flag       */

/* PWR_CR2 bits - wakeup-pin enables */
#define PWR_CR2_EWUP6       (1u << 5)   /* Enable WKUP6 = PI11 (user button,   */
                                        /* 32F746GDISCOVERY)                   */

/* PWR_CSR2 bits - clear wakeup-pin flags (write-1-to-clear, bits 13:8) */
#define PWR_CSR2_CWUPF6     (1u << 13)  /* Clear WUPF6 flag for PI11           */

/* ---- SCB System Control Register ---------------------------------------- */
#define SCB_SCR             (*(volatile uint32_t *)0xE000ED10u)
#define SCB_SCR_SLEEPDEEP   (1u << 2)

void shutdown_now(void)
{
    PANIC_IF(g_uart_ctx.tx_buf == 0, "shutdown: uart not initialised");

    /*
     * 1. Drain the async UART TX ring-buffer.
     *    IRQs are still enabled so the TXEIE/TCIE ISR can drain the
     *    hardware shift register.  uart_tx_done() returns true only when
     *    the ring-buffer is empty AND the TC flag has fired (DRV_IDLE).
     */
    while (!uart_tx_done()) {
        __asm__ volatile("wfi");
    }

    /* 2. Globally mask all interrupts from here on. */
    __asm__ volatile("cpsid i" ::: "memory");

    /*
     * 3. Belt-and-suspenders: wait for the UART hardware TC flag so
     *    the last byte is fully shifted out before we cut power to
     *    peripherals.
     */
    uart_flush_tx();

    /* 4. Gate off SDMMC1 and USART6 interrupts in the NVIC so they
     *    cannot fire if interrupts are re-enabled by the core during
     *    the standby entry sequence.
     */
    nvic_disable_irq(USART6_IRQn);
    nvic_disable_irq(SDMMC1_IRQn);

    /* 5. Enable the PWR interface clock (RCC APB1 bit 28). */
    RCC_APB1ENR |= RCC_APB1ENR_PWREN;

    /*
     * 6. Configure wakeup source: WKUP6 = PI11 (USER button on
     *    32F746GDISCOVERY).  Clear any stale wakeup and standby flags
     *    first to prevent an immediate spurious wakeup.
     */
    PWR_CSR2 |= PWR_CSR2_CWUPF6;   /* clear PI11 wakeup pin flag  */
    PWR_CR1  |= PWR_CR1_CWUF;       /* clear consolidated WUF flag */
    PWR_CR1  |= PWR_CR1_CSBF;       /* clear standby flag (SBF)    */

    PWR_CR2  |= PWR_CR2_EWUP6;      /* enable PI11 as wakeup pin   */

    /* 7. Select Standby mode (PDDS=1 means Standby, not Stop). */
    PWR_CR1 |= PWR_CR1_PDDS;

    /* 8. Set SLEEPDEEP in the Cortex-M7 System Control Register. */
    SCB_SCR |= SCB_SCR_SLEEPDEEP;

    /* 9. Memory barriers before WFI. */
    __asm__ volatile("dsb" ::: "memory");
    __asm__ volatile("isb");

    /* 10. Enter Standby.  Should not return. */
    __asm__ volatile("wfi");

    for (;;) {}
}
