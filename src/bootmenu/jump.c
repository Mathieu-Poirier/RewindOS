#include "../../include/stdint.h"
#include "../../include/jump.h"

#define SCB_VTOR (*(volatile uint32_t *)0xE000ED08)
#define SCB_ICSR (*(volatile uint32_t *)0xE000ED04)
#define SYST_CSR (*(volatile uint32_t *)0xE000E010)
#define SYST_RVR (*(volatile uint32_t *)0xE000E014)
#define SYST_CVR (*(volatile uint32_t *)0xE000E018)
#define NVIC_ICER_BASE ((volatile uint32_t *)0xE000E180)
#define NVIC_ICPR_BASE ((volatile uint32_t *)0xE000E280)
#define NVIC_WORDS 8u
#define ICSR_PENDSTCLR (1u << 25)
#define ICSR_PENDSVCLR (1u << 27)

typedef void (*entry_t)(void);

static inline void dsb_isb(void)
{
        __asm volatile("dsb 0xF" ::: "memory");
        __asm volatile("isb 0xF" ::: "memory");
}

void jump_to_image(uint32_t base)
{
        uint32_t new_msp = *(volatile uint32_t *)(base + 0x0);
        uint32_t reset_pc = *(volatile uint32_t *)(base + 0x4);

        __asm volatile("cpsid i" ::: "memory"); /* IRQs off for handoff */

        /* Stop SysTick and clear pending core/peripheral interrupts inherited from boot. */
        SYST_CSR = 0u;
        SYST_RVR = 0u;
        SYST_CVR = 0u;
        SCB_ICSR = ICSR_PENDSTCLR | ICSR_PENDSVCLR;
        for (uint32_t i = 0u; i < NVIC_WORDS; i++)
        {
                NVIC_ICER_BASE[i] = 0xFFFFFFFFu;
                NVIC_ICPR_BASE[i] = 0xFFFFFFFFu;
        }

        SCB_VTOR = base; /* vector table = app base */
        dsb_isb();
        /* Never access C stack locals after MSP switch.
         * Branch directly in asm to the app reset vector. */
        __asm volatile(
                "msr msp, %0\n"
                "cpsie i\n"
                "bx  %1\n"
                :
                : "r"(new_msp), "r"(reset_pc)
                : "memory");
        __builtin_unreachable();
}
