#include "../../../include/gpio.h"

extern void enable_gpioc(void);

void enable_gpio_clock(void){
        enable_gpioc();
}