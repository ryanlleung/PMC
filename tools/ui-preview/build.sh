#!/bin/sh
# Builds the host screen render and writes one PNG per scenario to out/.
# Needs gcc and python3 with Pillow. LVGL is compiled once into out/liblvgl.a.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
PROJ="$HERE/../../InitialTesting"
OUT="$HERE/out"
mkdir -p "$OUT/lvgl"
CFLAGS="-O1 -w -DPMC_LINK_ETHERNET=0 -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE \
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
# serial flash in RAM: Home states, Menu, keypad, dialogs, stage 5 and 6 pages.
gcc $CFLAGS -DREAL_CAL -o "$OUT/ui_new" "$HERE/ui_new_main.c" "$HERE/fake_hw.c" \
  "$PROJ/ui.c" "$PROJ/ui_home.c" "$PROJ/ui_valve.c" "$PROJ/ui_settings.c" "$PROJ/ui_trend.c" "$PROJ/trend.c" "$PROJ/motor_screen.c" \
  "$PROJ/cfg.c" "$PROJ/cal.c" "$PROJ/ui_font_12.c" "$PROJ/ui_font_16.c" "$PROJ/ui_font_26.c" "$PROJ/ui_font_40.c" "$PROJ/ui_img_logo.c" \
  "$PROJ/fixed.c" "$OUT/liblvgl.a" -lm
for s in splash notcal manual calib moving nodruck stop neg menu keypad confirm setpos \
         valve valvenc limithit calpage calpage2 calhelp calset handset bootpick hot faults detail diag help help3 back bootask bootmove \
         set-main set-sensor set-pick set-add set-addask set-addcal set-caldetail set-valve set-valve2 set-valve3 set-valvepos \
         set-tune set-tunekp set-tunedraft set-saveask set-saved set-review set-load motor motor-speed motor-coils motor-ask motor-held motor-moving motor-steps set-name set-named set-net set-remote set-netaddr set-netaddr2 set-netused \
         overon calsensor calvac trend trend1h trend1m notrend \
         auto autostable autoreg autofirst autoask autosp automan autovalve paused warn4 warn5 warn6 f16 trendauto; do
  "$OUT/ui_new" "$s" "$OUT/new-$s.raw"
  python3 "$HERE/render.py" "$OUT/new-$s.raw" "$OUT/new-$s.png"
  rm "$OUT/new-$s.raw"
done
echo "Wrote the screen previews to $OUT"
