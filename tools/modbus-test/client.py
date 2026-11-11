"""Talks to the controller (or the host test's server) over Modbus TCP with
pymodbus, decodes the map the way a SCADA client would, and checks it.
Usage: client.py <ip> [port] [writes 0|1]
On the bench: client.py <controller ip>   (prints the map, writes nothing)."""
import math
import struct
import sys

from pymodbus.client import ModbusTcpClient

ip = sys.argv[1]
port = int(sys.argv[2]) if len(sys.argv) > 2 else 502
writes = sys.argv[3] if len(sys.argv) > 3 else None   # only the host test passes this

MODES = ["NOT HOMED", "HOMING", "MANUAL", "AUTO"]
STATUS = ["reading OK", "position set", "homed", "moving", "closed switch pressed",
          "fault", "warning", "target set", "remote writes on", "motor supply on"]


def f32(r, i):
    return struct.unpack(">f", struct.pack(">HH", r[i], r[i + 1]))[0]


c = ModbusTcpClient(ip, port=port, timeout=3)
if not c.connect():
    sys.exit(f"cannot connect to {ip}:{port}")
rr = c.read_holding_registers(0, count=26)
if rr.isError():
    sys.exit(f"read failed: {rr}")
r = rr.registers
print(f"map version {r[0]}, heartbeat {r[1]}, mode {MODES[r[2]] if r[2] < 4 else r[2]}")
print("status:", ", ".join(n for b, n in enumerate(STATUS) if r[3] >> b & 1) or "none")
print(f"regulation {r[4]}, faults 0x{r[5]:04x}, warnings 0x{r[6]:04x}, "
      f"fault count {r[7]}, events {r[8]}")
for name, at, unit in [("pressure", 10, "mbar"), ("target", 12, "mbar"), ("valve", 14, "%"),
                       ("valve target", 16, "%"), ("range", 18, "mbar"),
                       ("excitation", 20, "V"), ("temperature", 22, "C")]:
    print(f"{name:13s} {f32(r, at):.3f} {unit}")
print(f"uptime {r[24] << 16 | r[25]} s")
co = c.read_coils(0, count=4)
st = c.read_coils(16, count=10)
print("coils 0-3:", co.bits[:4], " status coils 16-25:", st.bits[:10])

if writes is not None:
    ok = True
    def check(cond, what):
        global ok
        print(("ok   " if cond else "FAIL ") + what)
        ok &= bool(cond)
    check(r[0] == 2 and abs(f32(r, 10) - 4.625) < 1e-4, "map version and pressure")
    payload = list(struct.unpack(">HH", struct.pack(">f", 4.8)))
    w = c.write_registers(12, payload)
    if writes == "1":
        check(not w.isError(), "target write accepted")
        t = c.read_holding_registers(12, count=2).registers
        check(abs(f32(t, 0) - 4.8) < 1e-5, "target reads back 4.8")
        check(not c.write_coil(0, True).isError(), "AUTO coil accepted")
        check(c.read_coils(0, count=1).bits[0], "AUTO coil reads 1")
        check(c.write_coil(1, True).isError() is False, "STOP coil accepted")
        check(c.read_holding_registers(2, count=1).registers[0] == 2, "STOP left MANUAL")
    else:
        check(w.isError() and w.exception_code == 1, "target write refused, exception 1")
        check(c.write_coil(0, True).exception_code == 1, "AUTO coil refused, exception 1")
    e = c.read_input_registers(0, count=1)
    check(e.isError() and e.exception_code == 1, "FC4 answers exception 1")
    e = c.read_holding_registers(45, count=10)   # the map ends at 50 (version 2)
    check(e.isError() and e.exception_code == 2, "read past the map answers exception 2")
    c.close()
    sys.exit(0 if ok else 1)
c.close()
