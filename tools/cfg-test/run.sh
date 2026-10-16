#!/bin/sh
# Builds and runs the host test of the settings store and the saved valve
# position and the trend slots (test_cfg.c). Needs gcc; uses the LVGL library tools/ui-preview
# builds (for lv_snprintf and the tick). -v prints the link and event lines.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
PROJ="$HERE/../../InitialTesting"
LVGL="$HERE/../ui-preview/out/liblvgl.a"
[ -f "$LVGL" ] || { echo "Build tools/ui-preview first (it compiles LVGL once)"; exit 1; }
mkdir -p "$HERE/out"
gcc -O1 -g -Wall -Wextra -Wno-unused-parameter -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE \
  -I"$PROJ/lvgl_940" -I"$PROJ/lvgl_940/lv_conf/heavy" -I"$PROJ" \
  -o "$HERE/out/test_cfg" "$HERE/test_cfg.c" \
  "$PROJ/cfg.c" "$PROJ/cal.c" "$PROJ/valvepos.c" "$PROJ/trend.c" "$PROJ/fixed.c" "$LVGL" -lm
"$HERE/out/test_cfg" "$@"
