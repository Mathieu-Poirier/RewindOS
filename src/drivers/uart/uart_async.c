#include "../../../include/uart_async.h"
#include "../../../include/bump.h"
#include "../../../include/nvic.h"
#include "../../../include/panic.h"

#define USART6_BASE     0x40011400
#define USART6_CR1      (*(volatile uint32_t *)(USART6_BASE + 0x00))
#define USART6_ISR      (*(volatile uint32_t *)(USART6_BASE + 0x1C))
#define USART6_ICR      (*(volatile uint32_t *)(USART6_BASE + 0x20))
#define USART6_RDR      (*(volatile uint32_t *)(USART6_BASE + 0x24))
#define USART6_TDR      (*(volatile uint32_t *)(USART6_BASE + 0x28))

#define USART_CR1_RXNEIE    (1 << 5)
#define USART_CR1_TCIE      (1 << 6)
#define USART_CR1_TXEIE     (1 << 7)

#define USART_ISR_ORE       (1 << 3)
#define USART_ISR_RXNE      (1 << 5)
#define USART_ISR_TC        (1 << 6)
#define USART_ISR_TXE       (1 << 7)

#define USART_ICR_ORECF     (1 << 3)
#define USART_ICR_TCCF      (1 << 6)

uart_context_t g_uart_ctx;
static scheduler_t *g_uart_sched;
static uint8_t g_uart_ao_id;
static uint16_t g_uart_rx_sig;
static scheduler_t *g_uart_tx_sched;
static uint8_t g_uart_tx_ao_id;
static uint16_t g_uart_tx_sig;

static int uart_ctx_valid(void)
{
    if (g_uart_ctx.tx_buf == 0 || g_uart_ctx.rx_buf == 0) {
        return 0;
    }
    if (g_uart_ctx.tx_size == 0 || g_uart_ctx.rx_size == 0) {
        return 0;
    }
    if (g_uart_ctx.tx_head >= g_uart_ctx.tx_size || g_uart_ctx.tx_tail >= g_uart_ctx.tx_size) {
        return 0;
    }
    if (g_uart_ctx.rx_head >= g_uart_ctx.rx_size || g_uart_ctx.rx_tail >= g_uart_ctx.rx_size) {
        return 0;
    }
    return 1;
}

void USART6_IRQHandler(void)
{
    PANIC_IF(!uart_ctx_valid(), "uart ctx invalid in irq");

    uint32_t status = USART6_ISR;
    uint32_t cr1 = USART6_CR1;

    /* Framing / noise errors: discard the corrupt byte to avoid
     * processing garbage characters (e.g. SD-bus crosstalk on PC7). */
#define USART_ISR_FE  (1 << 1)
#define USART_ISR_NF  (1 << 2)
#define USART_ICR_FECF (1 << 1)
#define USART_ICR_NCF  (1 << 2)
    if ((status & USART_ISR_RXNE) && (status & (USART_ISR_FE | USART_ISR_NF))) {
        (void)(USART6_RDR);                       /* read clears RXNE */
        USART6_ICR = USART_ICR_FECF | USART_ICR_NCF;
        g_uart_ctx.rx_overrun_count++;             /* reuse counter for stats */
    }
    else if (status & USART_ISR_RXNE) {
        uint8_t data = (uint8_t)(USART6_RDR & 0xFF);
        uint16_t next_head = (g_uart_ctx.rx_head + 1) % g_uart_ctx.rx_size;

        if (next_head != g_uart_ctx.rx_tail) {
            g_uart_ctx.rx_buf[g_uart_ctx.rx_head] = data;
            g_uart_ctx.rx_head = next_head;
            g_uart_ctx.rx_status = DRV_IN_PROGRESS;

            if (g_uart_sched != 0 && g_uart_ctx.rx_event_pending == 0u) {
                PANIC_IF(g_uart_rx_sig == 0u, "uart scheduler bound with zero signal");
                g_uart_ctx.rx_event_pending = 1u;
                if (sched_post_isr(g_uart_sched, g_uart_ao_id,
                                   &(event_t){ .sig = g_uart_rx_sig }) != SCHED_OK) {
                    g_uart_ctx.rx_event_post_fail++;
                    g_uart_ctx.rx_event_pending = 0u;
                }
            }
        } else {
            g_uart_ctx.rx_overrun_count++;
        }
    }

    if (status & USART_ISR_ORE) {
        USART6_ICR = USART_ICR_ORECF;
        g_uart_ctx.rx_overrun_count++;
    }

    if ((status & USART_ISR_TXE) && (cr1 & USART_CR1_TXEIE)) {
        if (g_uart_ctx.tx_head != g_uart_ctx.tx_tail) {
            USART6_TDR = g_uart_ctx.tx_buf[g_uart_ctx.tx_tail];
            g_uart_ctx.tx_tail = (g_uart_ctx.tx_tail + 1) % g_uart_ctx.tx_size;
        } else {
            USART6_CR1 = cr1 & ~USART_CR1_TXEIE;
            USART6_CR1 |= USART_CR1_TCIE;
        }
    }

    if ((status & USART_ISR_TC) && (cr1 & USART_CR1_TCIE)) {
        USART6_ICR = USART_ICR_TCCF;
        USART6_CR1 &= ~USART_CR1_TCIE;
        g_uart_ctx.tx_status = DRV_IDLE;
        if (g_uart_tx_sched != 0) {
            PANIC_IF(g_uart_tx_sig == 0u, "uart tx notifier zero signal");
            (void)sched_post_isr(g_uart_tx_sched, g_uart_tx_ao_id,
                                 &(event_t){ .sig = g_uart_tx_sig });
        }
    }
}

void uart_async_init(void)
{
    g_uart_ctx.tx_buf = (uint8_t *)bump_alloc(UART_TX_BUF_SIZE);
    g_uart_ctx.rx_buf = (uint8_t *)bump_alloc(UART_RX_BUF_SIZE);
    PANIC_IF(g_uart_ctx.tx_buf == 0 || g_uart_ctx.rx_buf == 0, "uart async alloc failed");
    g_uart_ctx.tx_size = UART_TX_BUF_SIZE;
    g_uart_ctx.rx_size = UART_RX_BUF_SIZE;
    g_uart_ctx.tx_head = 0;
    g_uart_ctx.tx_tail = 0;
    g_uart_ctx.rx_head = 0;
    g_uart_ctx.rx_tail = 0;
    g_uart_ctx.tx_status = DRV_IDLE;
    g_uart_ctx.rx_status = DRV_IDLE;
    g_uart_ctx.rx_event_pending = 0;
    g_uart_ctx.rx_event_post_fail = 0;
    g_uart_ctx.rx_overrun_count = 0;
    g_uart_sched = 0;
    g_uart_ao_id = 0;
    g_uart_rx_sig = 0;
    g_uart_tx_sched = 0;
    g_uart_tx_ao_id = 0;
    g_uart_tx_sig = 0;

    USART6_CR1 |= USART_CR1_RXNEIE;

    nvic_set_priority(USART6_IRQn, 2);
    nvic_clear_pending(USART6_IRQn);
    nvic_enable_irq(USART6_IRQn);
}

int uart_async_putc(char c)
{
    PANIC_IF(!uart_ctx_valid(), "uart ctx invalid in putc");

    uint16_t next_head = (g_uart_ctx.tx_head + 1) % g_uart_ctx.tx_size;

    if (next_head == g_uart_ctx.tx_tail) {
        return 0;
    }

    nvic_disable_irq(USART6_IRQn);

    g_uart_ctx.tx_buf[g_uart_ctx.tx_head] = (uint8_t)c;
    g_uart_ctx.tx_head = next_head;
    g_uart_ctx.tx_status = DRV_IN_PROGRESS;

    USART6_CR1 |= USART_CR1_TXEIE;

    nvic_enable_irq(USART6_IRQn);

    return 1;
}

int uart_async_puts(const char *s)
{
    int count = 0;
    while (*s) {
        if (uart_async_putc(*s)) {
            count++;
            s++;
        } else {
            break;
        }
    }
    return count;
}

int uart_async_write(const uint8_t *data, uint16_t n)
{
    int count = 0;
    for (uint16_t i = 0; i < n; i++) {
        if (uart_async_putc((char)data[i])) {
            count++;
        } else {
            break;
        }
    }
    return count;
}

int uart_async_getc(void)
{
    PANIC_IF(!uart_ctx_valid(), "uart ctx invalid in getc");

    if (g_uart_ctx.rx_head == g_uart_ctx.rx_tail) {
        return -1;
    }

    nvic_disable_irq(USART6_IRQn);

    uint8_t c = g_uart_ctx.rx_buf[g_uart_ctx.rx_tail];
    g_uart_ctx.rx_tail = (g_uart_ctx.rx_tail + 1) % g_uart_ctx.rx_size;

    if (g_uart_ctx.rx_head == g_uart_ctx.rx_tail) {
        g_uart_ctx.rx_status = DRV_IDLE;
    }

    nvic_enable_irq(USART6_IRQn);

    return (int)c;
}

int uart_async_read(uint8_t *data, uint16_t max_n)
{
    int count = 0;
    for (uint16_t i = 0; i < max_n; i++) {
        int c = uart_async_getc();
        if (c < 0) {
            break;
        }
        data[i] = (uint8_t)c;
        count++;
    }
    return count;
}

int uart_async_inject_rx(const uint8_t *data, uint16_t n)
{
    PANIC_IF(!uart_ctx_valid(), "uart ctx invalid in inject_rx");
    PANIC_IF(data == 0 && n > 0u, "uart inject null data");

    if (n == 0u) {
        return 0;
    }

    int written = 0;

    nvic_disable_irq(USART6_IRQn);
    for (uint16_t i = 0u; i < n; i++) {
        uint16_t next_head = (g_uart_ctx.rx_head + 1u) % g_uart_ctx.rx_size;
        if (next_head == g_uart_ctx.rx_tail) {
            g_uart_ctx.rx_overrun_count++;
            break;
        }
        g_uart_ctx.rx_buf[g_uart_ctx.rx_head] = data[i];
        g_uart_ctx.rx_head = next_head;
        g_uart_ctx.rx_status = DRV_IN_PROGRESS;
        written++;
    }

    if (written > 0 && g_uart_sched != 0 && g_uart_ctx.rx_event_pending == 0u) {
        PANIC_IF(g_uart_rx_sig == 0u, "uart inject zero rx signal");
        g_uart_ctx.rx_event_pending = 1u;
        if (sched_post(g_uart_sched, g_uart_ao_id, &(event_t){ .sig = g_uart_rx_sig }) != SCHED_OK) {
            g_uart_ctx.rx_event_post_fail++;
            g_uart_ctx.rx_event_pending = 0u;
        }
    }
    nvic_enable_irq(USART6_IRQn);

    return written;
}

int uart_tx_done(void)
{
    return (g_uart_ctx.tx_head == g_uart_ctx.tx_tail) &&
           (g_uart_ctx.tx_status == DRV_IDLE);
}

int uart_rx_available(void)
{
    return g_uart_ctx.rx_head != g_uart_ctx.rx_tail;
}

void uart_async_bind_scheduler(scheduler_t *sched, uint8_t ao_id, uint16_t rx_sig)
{
    PANIC_IF(sched == 0, "uart bind null scheduler");
    PANIC_IF(rx_sig == 0u, "uart bind zero rx signal");

    nvic_disable_irq(USART6_IRQn);
    g_uart_sched = sched;
    g_uart_ao_id = ao_id;
    g_uart_rx_sig = rx_sig;
    g_uart_ctx.rx_event_pending = 0;
    nvic_enable_irq(USART6_IRQn);
}

void uart_async_unbind_scheduler(void)
{
    nvic_disable_irq(USART6_IRQn);
    g_uart_sched = 0;
    g_uart_ao_id = 0;
    g_uart_rx_sig = 0;
    g_uart_ctx.rx_event_pending = 0;
    nvic_enable_irq(USART6_IRQn);
}

int uart_async_rx_event_finish(void)
{
    int still_pending;

    nvic_disable_irq(USART6_IRQn);
    still_pending = (g_uart_ctx.rx_head != g_uart_ctx.rx_tail);
    if (!still_pending) {
        g_uart_ctx.rx_event_pending = 0u;
    }
    nvic_enable_irq(USART6_IRQn);

    return still_pending;
}

void uart_async_bind_tx_notifier(scheduler_t *sched, uint8_t ao_id, uint16_t tx_sig)
{
    PANIC_IF(sched == 0, "uart tx bind null scheduler");
    PANIC_IF(tx_sig == 0u, "uart tx bind zero signal");

    nvic_disable_irq(USART6_IRQn);
    g_uart_tx_sched = sched;
    g_uart_tx_ao_id = ao_id;
    g_uart_tx_sig = tx_sig;
    nvic_enable_irq(USART6_IRQn);
}

void uart_async_unbind_tx_notifier(void)
{
    nvic_disable_irq(USART6_IRQn);
    g_uart_tx_sched = 0;
    g_uart_tx_ao_id = 0;
    g_uart_tx_sig = 0;
    nvic_enable_irq(USART6_IRQn);
}

int uart_async_resume_after_restore(void)
{
    if (!uart_ctx_valid()) {
        return 0;
    }

    /* Discard stale TX bytes that were already sent before the reboot.
     * Keeping them would race with the sync uart_puts diagnostics and
     * re-send characters that already appeared on the terminal.        */
    g_uart_ctx.tx_head = 0;
    g_uart_ctx.tx_tail = 0;
    g_uart_ctx.tx_status = DRV_IDLE;

    /* Discard stale RX bytes — they were already processed before the
     * snapshot.  Fresh input arrives via live UART or journal inject.  */
    g_uart_ctx.rx_head = 0;
    g_uart_ctx.rx_tail = 0;
    g_uart_ctx.rx_event_pending = 0u;

    /* NOTE: do NOT enable IRQ here — caller must call
     * uart_async_resume_enable_irq() after cleanup is complete.    */
    return 1;
}

void uart_async_resume_enable_irq(void)
{
    uint32_t cr1 = USART6_CR1;
    cr1 |= USART_CR1_RXNEIE;
    USART6_CR1 = cr1;

    nvic_set_priority(USART6_IRQn, 2);
    nvic_clear_pending(USART6_IRQn);
    nvic_enable_irq(USART6_IRQn);
}
