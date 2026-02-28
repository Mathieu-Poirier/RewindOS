#pragma once

/*
 * shutdown_now - flush UART output then enter STM32F7 Standby mode.
 *
 * Call only after all desired output has been queued.  The function
 * spins until the async UART TX ring-buffer is empty (IRQs still
 * enabled), then disables all IRQs, waits for the UART hardware TC
 * flag, configures PWR for Standby, and executes WFI.
 *
 * On the 32F746GDISCOVERY the USER button (PI11 = WKUP6) is enabled
 * as a wakeup source.  Wakeup from Standby is indistinguishable from
 * a cold reset at the application level (SRAM is not retained).
 */
void shutdown_now(void);
