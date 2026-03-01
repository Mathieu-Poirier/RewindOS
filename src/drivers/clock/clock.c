// src/drivers/clock/clock.c
#include "../../../include/clock.h"
#define RCC_BASE 0x40023800u
#define RCC_CR (*(volatile uint32_t *)(RCC_BASE + 0x00u))
#define RCC_PLLCFGR (*(volatile uint32_t *)(RCC_BASE + 0x04u))
#define RCC_CFGR (*(volatile uint32_t *)(RCC_BASE + 0x08u))
#define FLASH_ACR (*(volatile uint32_t *)0x40023C00u)

#define RCC_CR_HSEON (1u << 16)
#define RCC_CR_HSERDY (1u << 17)
#define RCC_CR_PLLON (1u << 24)
#define RCC_CR_PLLRDY (1u << 25)

#define RCC_CFGR_SW_MASK (3u << 0)
#define RCC_CFGR_SWS_MASK (3u << 2)
#define RCC_CFGR_SW_PLL (2u << 0)
#define RCC_CFGR_SWS_PLL (2u << 2)

#define RCC_CFGR_HPRE_MASK (0xFu << 4)
#define RCC_CFGR_PPRE1_MASK (0x7u << 10)
#define RCC_CFGR_PPRE2_MASK (0x7u << 13)

#define RCC_PLLCFGR_DESIRED ((9u << 24) | (1u << 22) | (432u << 6) | 25u)

#define FLASH_ACR_LATENCY_7WS 7u
#define FLASH_ACR_PRFTEN (1u << 8)
#define FLASH_ACR_ICEN (1u << 9)
#define FLASH_ACR_DCEN (1u << 10)

#define CLOCK_TIMEOUT 5000000u
#define HSI_HZ 16000000u
#define PLL_SYS_HZ 216000000u

static uint32_t apb_presc_div(uint32_t ppre_bits)
{
        if (ppre_bits < 4u)
                return 1u;
        if (ppre_bits == 4u)
                return 2u;
        if (ppre_bits == 5u)
                return 4u;
        if (ppre_bits == 6u)
                return 8u;
        return 16u;
}

uint32_t clock_apb2_hz(void)
{
        uint32_t sys_hz = ((RCC_CFGR & RCC_CFGR_SWS_MASK) == RCC_CFGR_SWS_PLL) ? PLL_SYS_HZ : HSI_HZ;
        uint32_t ppre2_bits = (RCC_CFGR >> 13) & 0x7u;
        return sys_hz / apb_presc_div(ppre2_bits);
}

void full_clock_init(void)
{
        uint32_t t = CLOCK_TIMEOUT;
        RCC_CR |= RCC_CR_HSEON;
        while (((RCC_CR & RCC_CR_HSERDY) == 0u) && t--)
        {
        }
        if ((RCC_CR & RCC_CR_HSERDY) == 0u)
                return; /* fallback: keep current clock (typically HSI) */

        FLASH_ACR = FLASH_ACR_LATENCY_7WS | FLASH_ACR_PRFTEN | FLASH_ACR_ICEN | FLASH_ACR_DCEN;

        uint32_t cfgr = RCC_CFGR;
        cfgr &= ~(RCC_CFGR_HPRE_MASK | RCC_CFGR_PPRE1_MASK | RCC_CFGR_PPRE2_MASK);
        cfgr |= (4u << 10) | (4u << 13); /* APB1=/4, APB2=/2 */
        RCC_CFGR = cfgr;

        if ((RCC_CR & RCC_CR_PLLON) != 0u)
        {
                if ((RCC_CFGR & RCC_CFGR_SWS_MASK) == RCC_CFGR_SWS_PLL)
                        return; /* already on PLL */

                RCC_CR &= ~RCC_CR_PLLON;
                t = CLOCK_TIMEOUT;
                while (((RCC_CR & RCC_CR_PLLRDY) != 0u) && t--)
                {
                }
                if ((RCC_CR & RCC_CR_PLLRDY) != 0u)
                        return;
        }

        RCC_PLLCFGR = RCC_PLLCFGR_DESIRED;
        RCC_CR |= RCC_CR_PLLON;
        t = CLOCK_TIMEOUT;
        while (((RCC_CR & RCC_CR_PLLRDY) == 0u) && t--)
        {
        }
        if ((RCC_CR & RCC_CR_PLLRDY) == 0u)
                return;

        RCC_CFGR = (RCC_CFGR & ~RCC_CFGR_SW_MASK) | RCC_CFGR_SW_PLL;
        t = CLOCK_TIMEOUT;
        while (((RCC_CFGR & RCC_CFGR_SWS_MASK) != RCC_CFGR_SWS_PLL) && t--)
        {
        }
}
