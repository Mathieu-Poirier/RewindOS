// src/drivers/clock/clock.c
#include "../../../include/clock.h"

extern void hse_clock_init(void);
extern void flash_latency_init(void);
extern void set_bus_prescalers(void);
extern void pll_init(void);
extern void pll_enable(void);
extern void switch_to_pll_clock(void);

void full_clock_init(void)
{
        flash_latency_init();
        set_bus_prescalers();
        pll_init();
        pll_enable();
        switch_to_pll_clock();
}
