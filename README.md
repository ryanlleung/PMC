# MercShore iPC Pressure Controller

Pressure-based needle-valve controller for helium-flow cryostats: the MercShore iPC.

Oxford Instruments cryostats (CF-10, CF-11, HM-2, AVM-1, AVM-2) draw helium through a needle valve driven by a geared stepper motor. The controller holds the pressure measured by a Druck transducer on the pumping line by moving that valve. Pressure is a rough proxy for the helium flow; the flow itself is not measured. The controller keeps the cryostat's existing Druck transducer and stepper valve.

The controller is a MikroE Mikromedia 4 (STM32F407ZG) touchscreen board with four Click boards, built from bought modules only. It has a 4.3 in touchscreen, a text command port and Modbus TCP over Ethernet, and shows pressure to 0.1 mbar. The valve's limit switch is the closing stop and is read in firmware; there is no hardware interlock.

This repository describes firmware 0.18.0. It has been tested on the board, on a bench cryostat.

## Repository

| Folder | Holds |
| --- | --- |
| `firmware/` | The firmware: a NECTO Studio 7 project on the mikroSDK with LVGL 9.4 |
| `docs/` | The documents below, the screen captures (`screens/`) and the hardware schematics and wiring (`hardware/`) |
| `simulator/` | The firmware compiled to WebAssembly, run in a browser over a simulated bench |
| `secop/` | A reference Frappy (SECoP) driver for the text port |
| `tools/` | A Lake Shore 336 logger, the PC tests, the screen previews and a cross-build |

## Documents

| Document | Covers |
| --- | --- |
| [`docs/operation-manual.md`](docs/operation-manual.md) | Use at the cryostat: power-up, homing, MANUAL and AUTO, calibration, faults and warnings, settings, remote access, maintenance |
| [`docs/firmware.md`](docs/firmware.md) | Boards, connections and socket pins, firmware structure, pressure measurement, storage, build and flash |
| [`docs/text-port-commands.md`](docs/text-port-commands.md) | The text port on TCP 8888: commands, replies and error codes |
| [`docs/modbus-register-map.md`](docs/modbus-register-map.md) | The Modbus TCP server on port 502: registers, coils and exceptions |
| [`docs/screens/`](docs/screens/README.md) | A capture of every screen |
| [`docs/hardware/`](docs/hardware/README.md) | Board schematics and the wiring drawings |
| [`simulator/README.md`](simulator/README.md) | Running and rebuilding the simulator, and its tests |
| [`secop/README.md`](secop/README.md) | The Frappy driver and its tests |
| [`tools/README.md`](tools/README.md) | Each PC tool and test, and how to run it |
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | Branches, versions and the checks before a merge |

## Hardware

| Board | Where | What it does |
| --- | --- | --- |
| Mikromedia 4 for STM32 Capacitive, with the Mikromedia 4 Capacitive Shield | Main board and its socket shield | Runs the firmware; touchscreen, Ethernet, serial flash, real-time clock |
| Adapter Click | Socket S1 | Carries the Pressure 21 Click and brings out the limit switch input |
| Pressure 21 Click | On the Adapter Click | Room pressure, for the air point of the calibration |
| Stepper 3 Click | Socket S2 | Drives the valve's unipolar stepper motor |
| Boost 10 Click | Socket S3 | 10 V excitation for the Druck transducer |
| Power Monitor Click | Socket S4 | Reads the Druck output and the excitation |

Off the board: the Druck UNIK 5000 transducer (15 psia absolute), the Oxford Instruments needle valve with its geared stepper and limit switch, and a PoE splitter giving a regulated 12 V. Connections, pins and cautions are in `docs/firmware.md`.

## Quick start

- **Try it without hardware.** Open `simulator/index.html` in a browser (`simulator/README.md`). It runs the same firmware source as the board.
- **Build and flash.** Open `firmware/` in NECTO Studio 7, build Debug, and flash over USB-C with mikroBootloader (`docs/firmware.md`).
- **Connect.** The controller has a static IP address, 192.168.1.100/24 unless changed on Menu > System. The text port is TCP 8888 and Modbus TCP is 502.
- **Run the tests.** `tools/README.md` lists the PC tests; `node simulator/tests/run.js` runs the simulator's.

## Licence

The project's own files are under the Apache License 2.0 (`LICENSE`, `NOTICE`). The repository also holds third-party software and documents under their own licences, and the firmware image links the mikroSDK; see `THIRD-PARTY-NOTICES.md` before building or distributing it.
