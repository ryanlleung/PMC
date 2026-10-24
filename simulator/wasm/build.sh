#!/bin/sh
# Builds simulator/pmc-wasm.js (the WebAssembly, base64): the firmware in InitialTesting (screens, valve,
# AUTO, settings, calibration, the link commands) for the browser, over the
# simulated board in this folder. Needs zig 0.13 (pip install ziglang==0.13.0,
# then ZIG="python3 -m ziglang") for its WebAssembly C library.
#
# Only stepper3.c and supply.c are changed on the way in, and only where they
# write fixed hardware addresses (NVIC, PRIMASK, the VREFINT factory value).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
PROJ="$HERE/../../InitialTesting"
OUT="$HERE/out"
ZIG=${ZIG:-"python3 -m ziglang"}
mkdir -p "$OUT/lvgl" "$OUT/patched"

CFLAGS="-target wasm32-wasi -O2 -w -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE \
  -I$HERE/include -I$PROJ/lvgl_940 -I$PROJ -I$PROJ/build-MM4/generated -I$HERE"

if [ ! -f "$OUT/liblvgl.a" ]; then
  echo "Compiling LVGL (once)..."
  find "$PROJ/lvgl_940/src" -name '*.c' | while read -r f; do
    o="$OUT/lvgl/$(echo "$f" | sed "s|$PROJ/lvgl_940/src/||; s|/|_|g").o"
    $ZIG cc $CFLAGS -c "$f" -o "$o"
  done
  $ZIG ar rcs "$OUT/liblvgl.a" "$OUT"/lvgl/*.o
fi

sed -e 's|^#define NVIC_ISER(n).*|#define NVIC_ISER(n) sim_nvic_dummy|' \
    -e 's|^#define NVIC_ICPR(n).*|#define NVIC_ICPR(n) sim_nvic_dummy|' \
    -e 's|^#define NVIC_IPR8(n).*|#define NVIC_IPR8(n) sim_nvic_dummy|' \
    -e 's|__asm volatile ("mrs %0, primask\\n\\tcpsid i" : "=r" (pm) :: "memory");|pm = 0;|' \
    -e 's|__asm volatile ("msr primask, %0" :: "r" (pm) : "memory");|(void)pm;|' \
    -e '1i #include <stdint.h>\nextern volatile uint32_t sim_nvic_dummy;' \
    "$PROJ/stepper3.c" > "$OUT/patched/stepper3.c"
sed -e 's|^#define VREFINT_CAL .*|extern const uint16_t sim_vrefint_cal;\n#define VREFINT_CAL sim_vrefint_cal|' \
    "$PROJ/supply.c" > "$OUT/patched/supply.c"
if grep -q "0xE000E\|primask" "$OUT/patched/stepper3.c" || grep -q "0x1FFF7A2A" "$OUT/patched/supply.c"; then
  echo "stepper3.c or supply.c changed: update the sed lines in build.sh" >&2
  exit 1
fi

FW="limit.c rtclock.c sysinfo.c link.c cal.c cfg.c valve.c ctrl.c valvepos.c trend.c sense.c fixed.c \
    ui.c ui_home.c ui_valve.c ui_settings.c ui_trend.c motor_screen.c \
    ui_font_12.c ui_font_16.c ui_font_26.c ui_font_40.c"
SRC=""
for f in $FW; do SRC="$SRC $PROJ/$f"; done

$ZIG cc $CFLAGS -mexec-model=reactor -o "$OUT/pmc.wasm" \
  "$HERE/sim_main.c" "$HERE/sim_clicks.c" "$HERE/bench.c" \
  "$OUT/patched/stepper3.c" "$OUT/patched/supply.c" $SRC "$OUT/liblvgl.a" \
  -Wl,--export=malloc -Wl,--strip-all -lm
# The page loads it from a script, so it also opens from a file:// path.
{ printf '// Built by simulator/wasm/build.sh from InitialTesting (PMC_FW_VERSION in pmc_config.h). Do not edit.\n'
  printf 'window.PMC_WASM = "'; base64 -w0 "$OUT/pmc.wasm"; printf '";\n'; } > "$HERE/../pmc-wasm.js"
ls -l "$OUT/pmc.wasm" "$HERE/../pmc-wasm.js"
