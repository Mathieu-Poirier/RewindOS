#include "../include/stdint.h"
#include "../include/clock.h"
#include "../include/gpio.h"
#include "../include/uart.h"
#include "../include/boot.h"

extern int _sdata, _edata, _sbss, _ebss;


int main(void)
{
        enable_gpio_clock();
        uart_init(16000000u, 115200u);
        boot_main();
        for(;;){}
}
