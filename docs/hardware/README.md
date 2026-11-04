# Hardware documents

Schematics of the boards in the controller and wiring diagrams of the valve. The README's Hardware section lists the boards, their sockets and the connections.

## Schematics (`schematics/`)

MikroE publishes a schematic PDF on each board's product page. The files as downloaded:

| Board | MikroE part | Files |
| --- | --- | --- |
| Mikromedia 4 for STM32 Capacitive | MIKROE-3618 | [`mikromedia-4-for-stm32-capacitive-schematic.pdf`](schematics/mikromedia-4-for-stm32-capacitive-schematic.pdf), manual [`mikromedia-4-stm32-capacitive-manual.pdf`](schematics/mikromedia-4-stm32-capacitive-manual.pdf) |
| Mikromedia 4 Capacitive Shield | MIKROE-3631 | [`mikromedia-4-cap-mikrobus-shield-schematic.pdf`](schematics/mikromedia-4-cap-mikrobus-shield-schematic.pdf), [`Mikromedia_4_Capacitive_shield.pdf`](schematics/Mikromedia_4_Capacitive_shield.pdf) |
| Adapter Click | MIKROE-1432 | [`Adapter_click_v100_Schematic.pdf`](schematics/Adapter_click_v100_Schematic.pdf), [`Adapter_Click.pdf`](schematics/Adapter_Click.pdf) |
| Pressure 21 Click | Pressure 21 Click | [`Pressure_21_click_v100_Schematic.pdf`](schematics/Pressure_21_click_v100_Schematic.pdf), [`Pressure_21_Click.pdf`](schematics/Pressure_21_Click.pdf) |
| Stepper 3 Click | MIKROE-2035 | [`stepper-3-click-schematic-v103.pdf`](schematics/stepper-3-click-schematic-v103.pdf), [`Stepper_3_Click.pdf`](schematics/Stepper_3_Click.pdf) |
| Boost 10 Click | MIKROE-6927 | [`boost-10-click-schematic.pdf`](schematics/boost-10-click-schematic.pdf), [`Boost_10_Click.pdf`](schematics/Boost_10_Click.pdf) |
| Power Monitor Click | MIKROE-4810 | [`power-monitor-click-v100-schematic.pdf`](schematics/power-monitor-click-v100-schematic.pdf), [`Power_Monitor_Click.pdf`](schematics/Power_Monitor_Click.pdf) |

Also there: [`um1974-stm32-nucleo144-boards-mb1137-stmicroelectronics.pdf`](schematics/um1974-stm32-nucleo144-boards-mb1137-stmicroelectronics.pdf), the ST user manual for the Nucleo-144 boards.

## Wiring (`wiring/`)

WireViz sources (`.yml`) with their renders (`.svg`, `.png`). Render with `pip install wireviz` (needs Graphviz), then `wireviz <file>.yml`.

| File | Shows |
| --- | --- |
| [`motor-to-fischer`](wiring/motor-to-fischer.svg) | Inside the valve's motor housing: the six motor leads and the limit microswitch to the motor board and the Fischer 7-pin receptacle (pins 1 to 4 coils, 5 the coil centre taps, 6 and 7 the switch) |
| [`oi-valve-cable`](wiring/oi-valve-cable.svg) | The existing Oxford Instruments valve lead, Fischer 7-pin to the Mercury iTC 15-way D (auxiliary I/O) |

Not documented by Oxford Instruments, and marked as inferred in the sources: the Fischer series and gender (102 A056 inferred from the 28 AWG leads), the core colours on the lead, and which motor lead colour lands on which motor board pin. The coil order can be set on the screen (Settings > Motor override > Coils), so a wrong guess there is fixed without rewiring.
