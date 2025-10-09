
all: firmware.hex

# If this looks strange it's because -c passes in commands see OpenOCD program command
flash:
	openocd -f interface/stlink.cfg -f target/stm32f7x.cfg -c "program firmware.hex verify reset exit" 

firmware.hex: 
	arm-none-eabi-gcc -mcpu=cortex-m7 -mthumb -g -O0 -T ld/link.ld -nostartfiles -nostdlib -ffreestanding src/startup.s src/main.c -o build/flash/firmware.hex

clean:
	del build\flash\firmware.hex

.PHONY: clean all flash
