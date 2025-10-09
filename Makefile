
all: firmware.hex

# If this looks strange it's because -c passes in commands see OpenOCD program command
flash:
	openocd -f interface/stlink.cfg -f target/stm32f7x.cfg -c "program build/flash/firmware.hex verify reset exit" 

firmware.hex: 
	arm-none-eabi-gcc -mcpu=cortex-m7 -mthumb -c src/main.c -o build/flash/main.o
	arm-none-eabi-gcc -mcpu=cortex-m7 -mthumb -c src/systick.c -o build/flash/systick.o
	arm-none-eabi-gcc -nostartfiles -nostdlib -ffreestanding -mcpu=cortex-m7 -mthumb -T ld/link.ld build/flash/main.o build/flash/systick.o -o build/flash/firmware.elf
	arm-none-eabi-objcopy -O ihex build/flash/firmware.elf build/flash/firmware.hex

clean:
	del build\flash\firmware.hex

.PHONY: clean all flash
