#!/bin/sh
# Builds the common HMI template preview of the simulator: a copy of the
# firmware in InitialTesting with hmi-template.patch applied, compiled by
# simulator/wasm/build.sh. InitialTesting itself is not changed.
#   sh simulator/hmi-preview/build.sh          writes pmc-wasm.js here
#   sh simulator/hmi-preview/build.sh --test   then runs simulator/tests/run.js on it
# Needs what simulator/wasm/build.sh needs (pip install ziglang==0.13.0).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
TREE="$HERE/out/tree"
rm -rf "$TREE"
mkdir -p "$TREE/InitialTesting" "$TREE/simulator" "$HERE/out/wasm"

# The firmware sources, copied; LVGL and the generated screen code by link.
(cd "$ROOT/InitialTesting" && tar cf - --exclude=lvgl_940 --exclude='build-*' --exclude=.cache --exclude=.meproject .) |
  (cd "$TREE/InitialTesting" && tar xf -)
ln -s "$ROOT/InitialTesting/lvgl_940" "$TREE/InitialTesting/lvgl_940"
ln -s "$ROOT/InitialTesting/build-MM4" "$TREE/InitialTesting/build-MM4"
# The simulator build and tests; LVGL compiles once into out/wasm.
(cd "$ROOT/simulator" && tar cf - --exclude=wasm/out wasm tests) | (cd "$TREE/simulator" && tar xf -)
ln -s "$HERE/out/wasm" "$TREE/simulator/wasm/out"
ln -s "$ROOT/tools" "$TREE/tools"
# The template changes: screens, plus the test taps for the pages that moved.
patch -s -d "$TREE" -p1 < "$HERE/hmi-template.patch"
sh "$TREE/simulator/wasm/build.sh"
sed '1s|.*|// Built by simulator/hmi-preview/build.sh: InitialTesting plus hmi-template.patch. Do not edit.|' \
  "$TREE/simulator/pmc-wasm.js" > "$HERE/pmc-wasm.js"
sed -e 's|<title>PMC simulator</title>|<title>PMC simulator, HMI template preview</title>|' \
    -e 's|<h1>PMC simulator</h1>|<h1>PMC simulator, HMI template preview</h1>|' \
  "$ROOT/simulator/index.html" > "$HERE/index.html"
ls -l "$HERE/pmc-wasm.js"

if [ "$1" = "--test" ]; then
  node "$TREE/simulator/tests/run.js"
fi
