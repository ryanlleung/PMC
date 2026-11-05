#!/bin/sh
# Builds and runs the host test of the Modbus TCP server (modbus.c, mbmap.c)
# twice: remote writes off and on (the default). Needs gcc only.
# mb.sh (bash only) then talks to it over TCP, and client.py too when
# python3 and pymodbus are installed.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
PROJ="$HERE/../../InitialTesting"
mkdir -p "$HERE/out"
for W in 0 1; do
  gcc -O1 -g -Wall -Wextra -Wno-unused-parameter -DPMC_MODBUS_WRITE=$W \
    -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE \
    -I"$PROJ/lvgl_940" -I"$PROJ/lvgl_940/lv_conf/heavy" -I"$PROJ" \
    -o "$HERE/out/test_modbus_w$W" "$HERE/test_modbus.c" \
    "$PROJ/modbus.c" "$PROJ/mbmap.c" "$PROJ/fixed.c" -lm
  "$HERE/out/test_modbus_w$W"
done
# mb.sh (the Git Bash client) against the host server, writes off then on.
for W in 0 1; do
  "$HERE/out/test_modbus_w$W" serve 15030 > "$HERE/out/serve.log" &
  PID=$!
  sleep 0.3
  OUT=$("$HERE/mb.sh" 127.0.0.1 15030 writetest; "$HERE/mb.sh" 127.0.0.1 15030)
  kill $PID
  echo "$OUT" | grep -q "pressure      4.625 mbar" || { echo "mb.sh FAIL: map"; echo "$OUT"; exit 1; }
  if [ $W = 0 ]; then WANT="write target 4.8:  90 01"; else WANT="write target 4.8:  10 00 0c 00 02"; fi
  echo "$OUT" | grep -q "$WANT" || { echo "mb.sh FAIL: writes $W"; echo "$OUT"; exit 1; }
  echo "mb.sh, writes $W: ok"
done
# The write commands, writes on: AUTO refuses a valve write, MANUAL takes it.
"$HERE/out/test_modbus_w1" serve 15031 > "$HERE/out/serve.log" &
PID=$!
sleep 0.3
OUT=$(for c in "target 4.8" "target 5000" auto "valve 20" manual "valve 20" stop reset ok \
         "sched pv 6.5 22 10" "pid 3 1.2 0.3 0.05" "sched pv 20 6 15" "pid 4 1 1 1"; do
  echo "$c: $("$HERE/mb.sh" 127.0.0.1 15031 $c)"; done)
MAP=$("$HERE/mb.sh" 127.0.0.1 15031)
kill $PID
WANT="target 4.8:  10 00 0c 00 02   accepted
target 5000:  90 03   exception 3
auto:  05 00 00 ff 00   accepted
valve 20:  90 04   exception 4
manual:  05 00 00 00 00   accepted
valve 20:  10 00 10 00 02   accepted
stop:  05 00 01 ff 00   accepted
reset:  05 00 02 ff 00   accepted
ok:  05 00 03 ff 00   accepted
sched pv 6.5 22 10:  10 00 1b 00 06   accepted
pid 3 1.2 0.3 0.05:  10 00 2d 00 06   accepted
sched pv 20 6 15:  90 04   exception 4
pid 4 1 1 1: set 1, 2 or 3"
[ "$OUT" = "$WANT" ] || { echo "mb.sh FAIL: write commands"; echo "$OUT"; exit 1; }
echo "$MAP" | grep -q "gain schedule pressure, B1 6.500 mbar, B2 22.000 mbar, hysteresis 10 %, set in use 1" &&
  echo "$MAP" | grep -q "set 3        KP 1.200, TI 0.300, KD 0.050" || { echo "mb.sh FAIL: schedule in the map"; echo "$MAP"; exit 1; }
echo "mb.sh write commands: ok"
if python3 -c "import pymodbus" 2>/dev/null; then
  for W in 0 1; do
    "$HERE/out/test_modbus_w$W" serve 15020 > "$HERE/out/serve.log" &
    PID=$!
    sleep 0.3
    python3 "$HERE/client.py" 127.0.0.1 15020 $W || { kill $PID; exit 1; }
    kill $PID
  done
else
  echo "pymodbus not installed: TCP client check skipped"
fi
