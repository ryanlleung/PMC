#!/bin/sh
# Builds the host screen render and writes one PNG per scenario to out/.
# Needs gcc and python3 with Pillow. LVGL is compiled once into out/liblvgl.a.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
PROJ="$HERE/../../InitialTesting"
OUT="$HERE/out"
mkdir -p "$OUT/lvgl"
CFLAGS="-O1 -w -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE \
  -I$PROJ/lvgl_940 -I$PROJ/lvgl_940/lv_conf/heavy \
  -I$PROJ -I$PROJ/build-MM4/generated"

if [ ! -f "$OUT/liblvgl.a" ]; then
  echo "Compiling LVGL (once)..."
  find "$PROJ/lvgl_940/src" -name '*.c' | while read -r f; do
    o="$OUT/lvgl/$(echo "$f" | sed "s|$PROJ/lvgl_940/src/||; s|/|_|g").o"
    gcc $CFLAGS -c "$f" -o "$o"
  done
  ar rcs "$OUT/liblvgl.a" "$OUT"/lvgl/*.o
fi

gcc $CFLAGS -o "$OUT/ui_preview" \
  "$HERE/sim_main.c" "$HERE/fake_hw.c" \
  "$PROJ/main_screen.c" \
  "$PROJ/build-MM4/generated/scr_main_screen.c" \
  "$PROJ/build-MM4/generated/screens.c" \
  "$OUT/liblvgl.a" -lm

for s in ok nopm lowexc trip cal unsaved nodruck; do
  "$OUT/ui_preview" "$s" "$OUT/$s.raw"
  python3 "$HERE/render.py" "$OUT/$s.raw" "$OUT/$s.png"
  rm "$OUT/$s.raw"
  "$OUT/ui_preview" "$s" "$OUT/$s-chip.raw" raw
  python3 "$HERE/render.py" "$OUT/$s-chip.raw" "$OUT/$s-chip.png"
  rm "$OUT/$s-chip.raw"
done
echo "Wrote calibration and chip-reading previews for all six scenarios to $OUT"
