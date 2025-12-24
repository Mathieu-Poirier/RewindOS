#include "../../include/stdint.h"
#include "../../include/jump.h"

#define SCB_VTOR (*(volatile uint32_t *)0xE000ED08)

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

        SCB_VTOR = base; /* vector table = app base */
        __asm volatile("msr msp, %0" ::"r"(new_msp) : "memory");

        dsb_isb();

        __asm volatile("cpsie i" ::: "memory"); /* IRQs on for app */

        ((entry_t)reset_pc)(); /* jump to app Reset_Handler */
        for (;;)
        {
        }
}
