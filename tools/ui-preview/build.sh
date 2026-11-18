#!/bin/sh
# Builds the host screen render and writes one PNG per scenario to out/.
# Needs gcc and python3 with Pillow. LVGL is compiled once into out/liblvgl.a.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
PROJ="$HERE/../../firmware"
OUT="$HERE/out"
mkdir -p "$OUT/lvgl"
CFLAGS="-O1 -w -DIPC_LINK_ETHERNET=0 -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE \
  -I$PROJ/lvgl_940 -I$PROJ/lvgl_940/lv_conf/heavy \
  -I$PROJ -I$PROJ/build-MM4/generated -I$PROJ/driver"

if [ ! -f "$OUT/liblvgl.a" ]; then
  echo "Compiling LVGL (once)..."
  find "$PROJ/lvgl_940/src" -name '*.c' | while read -r f; do
    o="$OUT/lvgl/$(echo "$f" | sed "s|$PROJ/lvgl_940/src/||; s|/|_|g").o"
    gcc $CFLAGS -c "$f" -o "$o"
  done
  ar rcs "$OUT/liblvgl.a" "$OUT"/lvgl/*.o
fi

# The screens (ui*.c) with the real settings store (cfg.c, cal.c) over a
# serial flash in RAM: Home states, Menu, keypad, dialogs and every page.
gcc $CFLAGS -DREAL_CAL -o "$OUT/preview" "$HERE/preview_main.c" "$HERE/fake_hw.c" \
  "$PROJ/ui.c" "$PROJ/ui_home.c" "$PROJ/ui_valve.c" "$PROJ/ui_settings.c" "$PROJ/ui_trend.c" "$PROJ/trend.c" "$PROJ/motor_screen.c" \
  "$PROJ/cfg.c" "$PROJ/cal.c" "$PROJ/ui_font_12.c" "$PROJ/ui_font_16.c" "$PROJ/ui_font_26.c" "$PROJ/ui_font_40.c" "$PROJ/ui_img_logo.c" \
  "$PROJ/fixed.c" "$OUT/liblvgl.a" -lm
for s in splash notcal manual calib moving nodruck stop neg menu keypad confirm setpos \
         valve valvenc limithit calpage calpage2 calhelp calset handset bootpick faults detail diag help help3 back bootask bootmove \
         valve-travel valve-homing valve-limit valve-limiton \
         set-tune set-tunezone3 set-tunezones set-tuneauto set-tunehelp set-tunekp set-tunedraft set-saved set-tuneundo reset-ask reset-done \
         motor motor-settings motor-speed motor-held motor-moving \
         set-net set-remote set-up-h set-up-d set-up-max set-netaddr set-netaddr2 set-netused \
         overon calsensor calvac trend trend1h trend1m notrend \
         auto autozone autostable autoreg autofirst autoask autosp automan autovalve paused homefail trendauto \
         shutask shutclose shutleave shutting off; do
  "$OUT/preview" "$s" "$OUT/$s.raw"
  python3 "$HERE/render.py" "$OUT/$s.raw" "$OUT/$s.png"
  rm "$OUT/$s.raw"
done
echo "Wrote the screen previews to $OUT"
