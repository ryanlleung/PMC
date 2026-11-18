# iPC tools

PC programs, tests and builds for the controller.

The controller itself is driven from its touchscreen, the text port (`docs/text-port-commands.md`) or Modbus (`docs/modbus-register-map.md`). Nothing here is needed to run it.

## What you need

| For | Needs |
| --- | --- |
| `ls336_log.py` | Python 3.8 or later. Standard library only, nothing to install |
| `mkimg.py` and the screen previews | Python 3 with Pillow (`pip install pillow`) |
| The tests | A C compiler and a shell: Linux, macOS, or Git Bash on Windows |
| The cross-build | Linux with cmake, ninja and `gcc-arm-none-eabi` |

Run every command from the repository root.

## Programs

| Folder | Run | What it does |
| --- | --- | --- |
| `lakeshore/` | `python tools/lakeshore/ls336_log.py <IP>` | Logs Lake Shore 336 temperatures over Ethernet to a CSV. See [`lakeshore/README.md`](lakeshore/README.md) |
| `images/` | `python tools/images/mkimg.py` | Makes the start-up logo, `firmware/ui_img_logo.c`, from `mercshore-ipc-logo.png` |
| `modbus-test/` | `tools/modbus-test/mb.sh` | Reads and writes Modbus registers and coils from a shell |

## Tests

Each test compiles the firmware's own source file on the PC and runs it. None needs a board.

| Folder | Run | What it checks |
| --- | --- | --- |
| `ctrl-test/` | `tools/ctrl-test/run.sh` | The AUTO control step, `ctrl.c`, against a model of the flow line: setpoint steps, helium off (W-2), pump off (AUTO stays on and recovers), a slipping drive (W-2) and the refusals on starting AUTO |
| `cfg-test/` | `tools/cfg-test/run.sh` | The settings store and the saved valve position, against a flash held in RAM |
| `modbus-test/` | `tools/modbus-test/run.sh` | The Modbus server and the register map |
| `baro-test/` | `tools/baro-test/run.sh` | The Pressure 21 Click conversion and its plausibility window |

The simulator's tests are in [`simulator/README.md`](../simulator/README.md) and the Frappy driver's in [`secop/README.md`](../secop/README.md).

## Builds

| Folder | Run | What it does |
| --- | --- | --- |
| `ui-preview/` | `tools/ui-preview/build.sh` | Renders the screens to PNG files in `tools/ui-preview/out/`, with the real settings store over a flash held in RAM. It also checks that the LVGL heap returns after opening and leaving screens |
| `xbuild/` | `tools/xbuild/build.sh [Release\|Debug]` | Cross-builds the firmware with the mikroSDK libraries in `firmware/.meproject` and prints the flash and RAM use. It is for size checks only; the build that is flashed is NECTO's |
