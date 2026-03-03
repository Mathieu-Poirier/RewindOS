#!/usr/bin/env bash
# Automated GDB crash analysis for RewindOS bootloader

ELF=build/flash/boot/boot.elf
GDB=arm-none-eabi-gdb
TARGET=localhost:3333

# Use a GDB command file for robust, deep stepping and crash capture
$GDB $ELF --batch-silent -x gdb_crash_analyze.gdb

echo "\n--- Automated GDB session complete ---"
echo "Check above for crash location, backtrace, and register state."

# Automated address-to-source mapping for stuck PCs
ELF=build/flash/boot/boot.elf
for pc in \
 0x080020cc 0x080020ce 0x080020d0 0x080020d2 0x080020d4 0x080020d6 0x080020d8 0x080020da 0x080020dc 0x080020de 0x080020e0 0x080020e2 0x080020e4; do
	echo -n "$pc: "
	arm-none-eabi-addr2line -e $ELF -f -C $pc
done
