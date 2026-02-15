# ===== Toolchain =====
CC      := arm-none-eabi-gcc
AS      := arm-none-eabi-as
OBJCOPY := arm-none-eabi-objcopy
OBJDUMP := arm-none-eabi-objdump

# ===== Output dirs =====
BUILD_BOOT := build/flash/boot
BUILD_MAIN := build/flash/main

# ===== Linker scripts =====
LDS_BOOT := ld/LinkerScript_boot.ld
LDS_MAIN := ld/LinkerScript_main.ld

# ===== Flags =====
CPUFLAGS := -mcpu=cortex-m7 -mthumb
CFLAGS   := -g -O0 -ffreestanding -Wall -Wextra $(CPUFLAGS) -Iinclude
ASFLAGS  := -g $(CPUFLAGS)
LDFLAGS_BASE := -g -nostartfiles -nodefaultlibs -nostdlib -ffreestanding $(CPUFLAGS)

# ===== Shared sources (compiled into BOTH images) =====
CSRCS_COMMON := \
	$(wildcard src/drivers/*/*.c) \
	$(wildcard src/kernel/*.c) \
	$(wildcard src/scheduler/*.c) \
	$(wildcard src/allocators/*.c) \
	$(wildcard src/common_io/*.c)

ASRCS_COMMON := \
	$(wildcard src/startup/*.s) \
	$(wildcard src/kernel/*.s) \
	$(wildcard src/drivers/*/*.s)

# ===== Boot image sources =====
CSRCS_BOOT := \
	$(CSRCS_COMMON) \
	$(wildcard src/bootmenu/*.c)

ASRCS_BOOT := $(ASRCS_COMMON)

# ===== Main image sources =====
CSRCS_MAIN := \
	$(CSRCS_COMMON) \
	$(wildcard src/terminal/*.c)

ASRCS_MAIN := $(ASRCS_COMMON)

# ===== Objects =====
BOOT_COBJS  := $(patsubst src/%.c,$(BUILD_BOOT)/%.o,$(CSRCS_BOOT))
BOOT_ASOBJS := $(patsubst src/%.s,$(BUILD_BOOT)/%.o,$(ASRCS_BOOT))
BOOT_OBJS   := $(BOOT_COBJS) $(BOOT_ASOBJS)

MAIN_COBJS  := $(patsubst src/%.c,$(BUILD_MAIN)/%.o,$(CSRCS_MAIN))
MAIN_ASOBJS := $(patsubst src/%.s,$(BUILD_MAIN)/%.o,$(ASRCS_MAIN))
MAIN_OBJS   := $(MAIN_COBJS) $(MAIN_ASOBJS)

# ===== Artifacts =====
BOOT_ELF := $(BUILD_BOOT)/boot.elf
BOOT_HEX := $(BUILD_BOOT)/boot.hex
BOOT_BIN := $(BUILD_BOOT)/boot.bin
BOOT_LST := $(BUILD_BOOT)/boot.lst
BOOT_MAP := $(BUILD_BOOT)/boot.map

MAIN_ELF := $(BUILD_MAIN)/main.elf
MAIN_HEX := $(BUILD_MAIN)/main.hex
MAIN_BIN := $(BUILD_MAIN)/main.bin
MAIN_LST := $(BUILD_MAIN)/main.lst
MAIN_MAP := $(BUILD_MAIN)/main.map

# ===== mkdir helper (POSIX) =====
define MKDIRP
	@mkdir -p "$(dir $@)"
endef

# ===== Default (must be first target) =====
all: $(BOOT_HEX) $(MAIN_HEX)

# ===== Serial console =====
PICOCOM ?= picocom
TTY ?= /dev/ttyUSB0
BAUD ?= 115200

.PHONY: connect
connect:
	@command -v $(PICOCOM) >/dev/null 2>&1 || { echo "$(PICOCOM) is not installed; install picocom or set PICOCOM to another terminal program."; exit 1; }
	@printf 'waiting for %s' $(TTY)
	@while [ ! -c "$(TTY)" ]; do printf '.'; sleep 0.2; done
	@printf '\nlaunching %s\n' $(PICOCOM)
	@$(PICOCOM) -b $(BAUD) $(TTY)

# ===== Compile rules =====
$(BUILD_BOOT)/%.o: src/%.c
	$(MKDIRP)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_BOOT)/%.o: src/%.s
	$(MKDIRP)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD_MAIN)/%.o: src/%.c
	$(MKDIRP)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_MAIN)/%.o: src/%.s
	$(MKDIRP)
	$(AS) $(ASFLAGS) $< -o $@

# ===== Link =====
$(BOOT_ELF): $(BOOT_OBJS)
	$(CC) $(LDFLAGS_BASE) -T $(LDS_BOOT) -Wl,-Map=$(BOOT_MAP),--cref $(BOOT_OBJS) -o $@

$(MAIN_ELF): $(MAIN_OBJS)
	$(CC) $(LDFLAGS_BASE) -T $(LDS_MAIN) -Wl,-Map=$(MAIN_MAP),--cref $(MAIN_OBJS) -o $@

# ===== Convert =====
$(BOOT_HEX): $(BOOT_ELF)
	$(OBJCOPY) -O ihex   $(BOOT_ELF) $(BOOT_HEX)
	$(OBJCOPY) -O binary $(BOOT_ELF) $(BOOT_BIN)
	$(OBJDUMP) -d        $(BOOT_ELF) > $(BOOT_LST)

$(MAIN_HEX): $(MAIN_ELF)
	$(OBJCOPY) -O ihex   $(MAIN_ELF) $(MAIN_HEX)
	$(OBJCOPY) -O binary $(MAIN_ELF) $(MAIN_BIN)
	$(OBJDUMP) -d        $(MAIN_ELF) > $(MAIN_LST)

# ===== Flash both images =====
flash: all
	openocd -f interface/stlink.cfg -f target/stm32f7x.cfg \
		-c "program $(BOOT_HEX) verify reset; \
		    program $(MAIN_HEX) verify reset; \
		    reset run; exit"

clean:
	@rm -rf build

.PHONY: all flash clean connect
