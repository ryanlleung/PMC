# Hardware documents

Schematics of the boards in the controller and wiring diagrams of the valve. `docs/firmware.md` lists the boards, their sockets and the connections.

## Schematics (`schematics/`)

MikroE publishes a schematic PDF on each board's product page. The files as downloaded:

| Board | MikroE part | File |
| --- | --- | --- |
| Mikromedia 4 for STM32 Capacitive | MIKROE-3618 | [`mikromedia-4-for-stm32-capacitive-schematic.pdf`](schematics/mikromedia-4-for-stm32-capacitive-schematic.pdf), manual [`mikromedia-4-stm32-capacitive-manual.pdf`](schematics/mikromedia-4-stm32-capacitive-manual.pdf) |
| Mikromedia 4 Capacitive Shield | MIKROE-3631 | [`mikromedia-4-cap-mikrobus-shield-schematic.pdf`](schematics/mikromedia-4-cap-mikrobus-shield-schematic.pdf) |
| Adapter Click | MIKROE-1432 | [`Adapter_click_v100_Schematic.pdf`](schematics/Adapter_click_v100_Schematic.pdf) |
| Pressure 21 Click | Pressure 21 Click | [`Pressure_21_click_v100_Schematic.pdf`](schematics/Pressure_21_click_v100_Schematic.pdf) |
| Stepper 3 Click | MIKROE-2035 | [`stepper-3-click-schematic-v103.pdf`](schematics/stepper-3-click-schematic-v103.pdf) |
| Boost 10 Click | MIKROE-6927 | [`boost-10-click-schematic.pdf`](schematics/boost-10-click-schematic.pdf) |
| Power Monitor Click | MIKROE-4810 | [`power-monitor-click-v100-schematic.pdf`](schematics/power-monitor-click-v100-schematic.pdf) |

The PDFs are MikroE's documents, copied unchanged (see `THIRD-PARTY-NOTICES.md`).

## Wiring (`wiring/`)

WireViz sources (`.yml`) with their renders (`.svg`). Render with `pip install wireviz` (needs Graphviz), then `wireviz <file>.yml`.

| File | Shows |
| --- | --- |
| [`motor-to-fischer`](wiring/motor-to-fischer.svg) | Inside the valve's motor housing: the six motor leads and the limit microswitch to the motor board and the Fischer 7-pin receptacle (pins 1 to 4 coils, 5 the coil centre taps, 6 and 7 the switch) |
| [`oi-valve-cable`](wiring/oi-valve-cable.svg) | The existing Oxford Instruments valve lead, Fischer 7-pin to the Mercury iTC 15-way D (auxiliary I/O) |

The firmware drives the coils in the order 0-2-1-3, found on the bench with the 26M048B2U motor. No screen or command changes it (Factory reset puts 0-2-1-3 back), so a motor wired in another order is fixed at the plug, not on the screen. Direction (Menu > Motor > Settings) reverses the motor without rewiring.
