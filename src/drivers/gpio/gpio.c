#include "../../../include/gpio.h"

extern void enable_gpioc(void);
extern void enable_gpiod(void);

void enable_gpio_clock(void){
        enable_gpioc();
        enable_gpiod();
}