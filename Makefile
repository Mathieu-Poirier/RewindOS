# ===== Toolchain =====
CC      := arm-none-eabi-gcc
AS      := arm-none-eabi-as
OBJCOPY := arm-none-eabi-objcopy
OBJDUMP := arm-none-eabi-objdump

# ===== Paths =====
BUILD   := build/flash
LDSCRIPT:= ld/link.ld
ELF     := $(BUILD)/firmware.elf
HEX     := $(BUILD)/firmware.hex
BIN     := $(BUILD)/firmware.bin
LST     := $(BUILD)/firmware.lst
MAP     := $(BUILD)/firmware.map

# ===== Flags =====
CPUFLAGS := -mcpu=cortex-m7 -mthumb
CFLAGS   := -g -O0 -ffreestanding -Wall -Wextra $(CPUFLAGS) -Iinclude
ASFLAGS  := -g $(CPUFLAGS)
LDFLAGS  := -g -nostartfiles -nodefaultlibs -nostdlib -ffreestanding $(CPUFLAGS) \
            -T $(LDSCRIPT) -Wl,-Map=$(MAP),--cref

# ===== Source discovery =====
# C sources
CSRCS := \
	src/main.c \
	$(wildcard src/kernel/*.c) \
	$(wildcard src/drivers/*/*.c)

# ASM sources
ASRCS := \
	$(wildcard src/startup/*.s) \
	$(wildcard src/kernel/*.s) \
	$(wildcard src/drivers/*/*.s)

# Objects
COBJS  := $(patsubst src/%.c,$(BUILD)/%.o,$(CSRCS))
ASOBJS := $(patsubst src/%.s,$(BUILD)/%.o,$(ASRCS))
OBJS   := $(COBJS) $(ASOBJS)

# ===== Default =====
all: $(HEX)

# Ensure build tree exists (including subdirs mirroring src/)
$(BUILD):
	@if not exist $(BUILD) mkdir $(BUILD)

# Create subdirectories for object outputs (Windows cmd)
# Example: build/flash/drivers/uart/
define MKDIRP
	@if not exist "$(dir $@)" mkdir "$(dir $@)"
endef

# ===== Compile rules =====
$(BUILD)/%.o: src/%.c | $(BUILD)
	$(MKDIRP)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: src/%.s | $(BUILD)
	$(MKDIRP)
	$(AS) $(ASFLAGS) $< -o $@

# ===== Link =====
$(ELF): $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) -o $@

# ===== Artifacts =====
$(HEX): $(ELF)
	$(OBJCOPY) -O ihex   $(ELF) $(HEX)
	$(OBJCOPY) -O binary $(ELF) $(BIN)
	$(OBJDUMP) -d        $(ELF) > $(LST)

# ===== Flash =====
flash: $(HEX)
	openocd -f interface/stlink.cfg -f target/stm32f7x.cfg -c "program $(HEX) verify reset exit"

# ===== Debug =====
debug_host:
	openocd -f interface/stlink.cfg -f target/stm32f7x.cfg

debug_connect: $(ELF)
	arm-none-eabi-gdb $(ELF)

# ===== Clean =====
clean:
	@if exist build rmdir /S /Q build

.PHONY: all flash clean debug_host debug_connect

