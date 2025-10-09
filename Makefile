
all: firmware.hex

# If this looks strange it's because -c passes in commands see OpenOCD program command
flash:
	openocd -f interface/stlink.cfg -f target/stm32f7x.cfg -c "program build/flash/firmware.hex verify reset exit" 

firmware.hex: 
	arm-none-eabi-gcc -c -mcpu=cortex-m7 -mthumb -O0 -ffreestanding -Wall -Wextra src/main.c -o build/flash/main.o


	arm-none-eabi-as -mcpu=cortex-m7 -mthumb src/startup.s -o build/flash/startup.o
	arm-none-eabi-as -mcpu=cortex-m7 -mthumb src/systick.s -o build/flash/systick.o


	arm-none-eabi-gcc -nostartfiles -nodefaultlibs -nostdlib -ffreestanding -mcpu=cortex-m7 -mthumb -T ld/link.ld build/flash/startup.o build/flash/systick.o build/flash/main.o -Wl,-Map=build/flash/firmware.map,--cref -o build/flash/firmware.elf

	arm-none-eabi-objcopy -O ihex build/flash/firmware.elf build/flash/firmware.hex

	
	arm-none-eabi-objcopy -O binary build/flash/firmware.elf build/flash/firmware.bin
	arm-none-eabi-objdump -d build/flash/firmware.elf > build/flash/firmware.lst

clean:
	del build\flash\firmware.hex build\flash\firmware.elf build\flash\firmware.bin build\flash\firmware.lst build\flash\firmware.map build\flash\main.o build\flash\startup.o build\flash\systick.o

.PHONY: clean all flash
