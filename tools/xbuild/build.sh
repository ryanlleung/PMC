#!/bin/sh
# Cross-builds the firmware in the cloud and prints flash and RAM use.
# Usage: build.sh [Debug|Release]   (default Debug, the build that runs on the board)
# Needs cmake, ninja and the Ubuntu gcc-arm-none-eabi + libnewlib-arm-none-eabi
# packages. The board is flashed from the NECTO build; this one is for checks.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TYPE=${1:-Debug}
OUT="$HERE/out-$TYPE"
cmake -S "$HERE" -B "$OUT" -G Ninja -DCMAKE_BUILD_TYPE="$TYPE" \
  -DCMAKE_TOOLCHAIN_FILE="$HERE/toolchain-arm.cmake" >/dev/null
ninja -C "$OUT" >/dev/null
arm-none-eabi-size -A "$OUT/ipc.elf" | awk '
  /^\.isr_vector|^\.text|^\.rodata|^\.ARM|^\.init_array|^\.fini_array|^\.preinit_array/ {flash+=$2}
  /^\.data/ {flash+=$2; ram+=$2}
  /^\.bss|^\._user_heap_stack/ {ram+=$2}
  /^\.ccmram|^\.ccm/ {ccm+=$2}
  {print}
  END {printf "\nFlash %.1f KB of 1024, SRAM %.1f KB of 128, CCM %.1f KB of 64\n", flash/1024, ram/1024, ccm/1024}'
