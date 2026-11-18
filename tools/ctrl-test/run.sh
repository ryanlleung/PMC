#!/bin/sh
# Builds and runs the host test of the AUTO control step (ctrl.c) against a
# C port of the reference model's flow-line model (model.c). Needs gcc. -v prints
# a line a second.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
PROJ="$HERE/../../firmware"
mkdir -p "$HERE/out"
gcc -O2 -g -Wall -Wextra -I"$PROJ" -I"$HERE" -o "$HERE/out/test_ctrl" \
  "$HERE/test_ctrl.c" "$HERE/model.c" "$PROJ/ctrl.c" -lm
"$HERE/out/test_ctrl" "$@"
