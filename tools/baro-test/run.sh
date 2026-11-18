#!/bin/sh
# Builds and runs the host test of the Pressure 21 (BMP581) conversion and
# plausibility window in baro.h (test_baro.c). Needs gcc.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
PROJ="$HERE/../../firmware"
mkdir -p "$HERE/out"
gcc -O1 -g -Wall -Wextra -I"$PROJ" -o "$HERE/out/test_baro" "$HERE/test_baro.c"
"$HERE/out/test_baro"
