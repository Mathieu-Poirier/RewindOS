#!/usr/bin/env bash
# Catch crash in main firmware (requires OpenOCD running on localhost:3333)
# Usage: ./gdb_main_crash.sh
set -e
arm-none-eabi-gdb build/flash/main/main.elf --batch -x gdb_main_crash.gdb
