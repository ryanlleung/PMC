# Hardware documents

Schematics of the boards in the controller and wiring diagrams of the valve. The README's Hardware section lists the boards, their sockets and the connections.

## Schematics (`schematics/`)

MikroE publishes a schematic PDF on each board's product page. One file per board, named after the board:

| Board | MikroE part | File |
| --- | --- | --- |
| Mikromedia 4 for STM32 Capacitive | MIKROE-3618 | `mikromedia-4-stm32-capacitive-schematic.pdf` (to add) |
| Mikromedia 4 Capacitive Shield | MIKROE-3631 | `mikromedia-4-capacitive-shield-schematic.pdf` (to add) |
| Adapter Click | MIKROE-1432 | `adapter-click-schematic.pdf` (to add) |
| Pressure 21 Click | Pressure 21 Click | `pressure-21-click-schematic.pdf` (to add) |
| Stepper 3 Click | MIKROE-2035 | `stepper-3-click-schematic.pdf` (to add) |
| Boost 10 Click | MIKROE-6927 | `boost-10-click-schematic.pdf` (to add) |
| Power Monitor Click | MIKROE-4810 | [`power-monitor-click-v100-schematic.pdf`](schematics/power-monitor-click-v100-schematic.pdf) |

## Wiring (`wiring/`)

WireViz sources (`.yml`) with their renders (`.svg`, `.png`). Render with `pip install wireviz` (needs Graphviz), then `wireviz <file>.yml`.

| File | Shows |
| --- | --- |
| [`motor-to-fischer`](wiring/motor-to-fischer.svg) | Inside the valve's motor housing: the six motor leads and the limit microswitch to the motor board and the Fischer 7-pin receptacle (pins 1 to 4 coils, 5 the coil centre taps, 6 and 7 the switch) |
| [`oi-valve-cable`](wiring/oi-valve-cable.svg) | The existing Oxford Instruments valve lead, Fischer 7-pin to the Mercury iTC 15-way D (auxiliary I/O) |

Not documented by Oxford Instruments, and marked as inferred in the sources: the Fischer series and gender (102 A056 inferred from the 28 AWG leads), the core colours on the lead, and which motor lead colour lands on which motor board pin. The coil order can be set on the screen (Settings > Motor override > Coils), so a wrong guess there is fixed without rewiring.
