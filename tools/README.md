# iPC tools

PC programs and tests for the controller. The controller itself is driven from its touchscreen, the text port (`docs/text-port-commands.md`) or Modbus (`docs/modbus-register-map.md`). The Python tools use the standard library only (Python 3, nothing to install). The tests run on Linux, macOS or Git Bash on Windows with a C compiler.

## Programs

| Tool | Run | What it does |
| --- | --- | --- |
| `lakeshore/` | `python tools/lakeshore/ls336_log.py <IP>` | Logs Lake Shore 336 temperatures over Ethernet to a CSV. See `lakeshore/README.md` |
| `images/` | `python tools/images/mkimg.py` | Makes the start-up logo (`firmware/ui_img_logo.c`) from `mercshore-ipc-logo.png` |
| `modbus-test/mb.sh` | `tools/modbus-test/mb.sh` | Reads and writes Modbus registers and coils from a shell |

## Tests and builds

| Test | Run | What it checks |
| --- | --- | --- |
| `ctrl-test/` | `tools/ctrl-test/run.sh` | `ctrl.c` on a PC against a model of the flow line (helium pot, needle valve, pump, motor ramp, closed switch): setpoint steps, helium off (W-2), pump off (AUTO stays on and recovers), a slipping drive (W-2) and the entry refusals |
| `cfg-test/` | `tools/cfg-test/run.sh` | The settings store and the saved valve position against a flash held in RAM |
| `modbus-test/` | `tools/modbus-test/run.sh` | The Modbus server and the register map |
| `baro-test/` | `tools/baro-test/run.sh` | The Pressure 21 Click conversion and plausibility window |
| `ui-preview/` | `tools/ui-preview/build.sh` | Renders the screens to PNG with the real settings store over a flash held in RAM, and checks the LVGL heap returns after opening and leaving screens |
| `xbuild/` | `tools/xbuild/build.sh [Release\|Debug]` | Cross-builds the firmware on Linux (cmake, ninja, `gcc-arm-none-eabi`) with the mikroSDK libraries in `firmware/.meproject` and prints flash and RAM use. For size checks only; the flashed build is NECTO's |

The simulator's tests are in `simulator/README.md` and the Frappy driver's in `secop/README.md`.
