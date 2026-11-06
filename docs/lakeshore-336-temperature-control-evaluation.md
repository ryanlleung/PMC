# Temperature control with the Lake Shore 336 and the needle valve: evaluation

Revision A, 6 November 2026. Written against firmware 0.16.0 and the control test GUI at commit 7126679. Evaluation only: nothing in the firmware, the GUI or the Frappy driver changes with this document.

## 1. The question

Today the mm4 holds a **pressure** setpoint by moving the needle valve, and the operator picks the pressure that gives the cryostat the temperature they want. The Lake Shore 336 on the same switch reads the heat exchanger (input A) and the cold head (input D) and has two heater outputs that are not controlling anything at the moment. The control test GUI already polls the 336 for the two temperatures.

The question is whether a system could talk to the 336 and set a **temperature** directly, using both actuators the cryostat has: the needle valve (cooling, through the helium flow) and the heater (warming). This document says what that would take, where the coordination should live, and what has to be found out on the bench first.

Short answer: yes, it is feasible, and the hard part is not the communication. Both instruments already speak a one-line text protocol over TCP and both are reachable from the same subnet. The hard part is the control scheme, because one temperature has two actuators, and the right scheme has been known since the Oxford Instruments ITC503 and the Mercury iTC: the heater holds the temperature fast, and the valve is moved slowly so the heater sits at a comfortable power. The recommendation is to build that scheme first on the PC (GUI) to prove it on the bench, then put it in the Frappy (SECoP) layer the team runs, and to leave the firmware a pressure controller. A direct 336 link in the firmware is possible and is described, but it is the most work and the least flexible place for it.

## 2. What exists

### 2.1 mm4 firmware (0.16.0)

- A PI loop on the pressure, 1 s period, moving the valve in steps. The gains are scheduled on the pressure (three PID sets, boundaries 6 and 20 mbar by default), because the loop gain of the cryostat is 3 to 9 times higher at low pressure than at high (`docs/mm4-operation-manual.md`, section 10).
- Text port 8888: `PRES SP <mbar>`, `MODE AUTO`, `MODE MANUAL`, `PRES?`, `VALV?`, `MODE?`, `ALRM?`, `STOP` and the tuning commands. One client at a time. Nothing is sent unasked. Modbus TCP on port 502 carries the same operator set for a supervisory system (two clients).
- The pressure is a rough proxy for the helium flow. It is not a flow measurement, and it is not a temperature. Setting a pressure sets a temperature only indirectly, and only in a steady state.
- Static IP, default 192.168.1.100/24, no gateway. The board can reach only its own subnet.
- Ethernet is CycloneTCP without an RTOS, served from the main loop with zero-timeout socket calls (`InitialTesting/ethlink.c`). The firmware is a TCP **server** only; it has no client socket today.

### 2.2 Lake Shore 336

Facts used here are from the Model 336 manual and the product page, checked on 4 and 6 November 2026. Items marked (confirm) should be checked on the instrument itself.

- Four sensor inputs A to D. On this rig A is the heat exchanger and D the cold head (Ryan, 5 November).
- Four control outputs. Outputs 1 and 2 are heater outputs, 100 W and 50 W, into 25 or 50 ohm, each with three ranges (High, Medium, Low, a factor of 10 in power each step) and an Off. Outputs 3 and 4 are plus or minus 10 V analog outputs, usable as a PID output for a voltage-driven heater or as a monitor output scaled from any input.
- Each output has its own PID loop (P 0.1 to 1000, I 0.1 to 1000, D 0 to 200), a setpoint, a setpoint ramp in K/min, a manual output, up to ten zones (a PID set, heater range and manual output per temperature band, switched on the setpoint), and an autotune. The loop updates at 10 Hz (confirm). The output can be driven by any input (`OUTMODE <output>,<mode>,<input>,<powerup>`; mode 0 Off, 1 closed loop PID, 2 zone, 3 open loop, 4 monitor out, 5 warm-up supply).
- Commands relevant to this work, all one line, terminated CR LF, several per line separated by `;`: `KRDG? A` (kelvin), `RDGST? A` (sensor status), `SETP 1,<K>` / `SETP? 1`, `RANGE 1,<0 to 3>` / `RANGE? 1`, `HTR? 1` (heater output in % of the range in use), `HTRST? 1` (heater fault status), `PID 1,<P>,<I>,<D>`, `MOUT 1,<%>`, `RAMP 1,<on>,<K/min>` and `RAMPST? 1`, `OUTMODE`, `HTRSET 1,<25|50 ohm>,<max current>,<max user current>,<display>`, `TLIMIT <input>,<K>` (the loop's heater turns off above this reading), `ATUNE 1,<mode>` and `TUNEST?`. Set commands answer nothing; a query answers one line.
- Ethernet: TCP port 7777, **at most two simultaneous connections** (a third is refused), and only one remote interface active at a time (`INTSEL`: USB, Ethernet or IEEE-488). The GUI's example address is 192.168.1.165, on the mm4's subnet.
- Two relays, driven by the alarms (30 V DC, 3 A, NO and NC contacts).
- Nothing in the 336 drives a stepper motor or a valve. Its only outputs are heater power, analog voltage and relays.

### 2.3 The control test GUI (`tools/pmc-gui`)

Python, standard library, one page in a browser. It holds one text-port connection to the mm4 and, when given `--ls336 <IP>`, a second thread that reads `KRDG? A` and `KRDG? D` once a second and logs them beside the pressure, valve and mode. It already runs the gain table, setpoint step and autotune tests, each a sequence of `PRES SP` and mode changes over minutes. Adding `HTR? 1`, `SETP 1` and `RANGE 1` to the 336 thread is a small change to one class. The GUI is a bench tool: it runs on the laptop, which sleeps, and its history buffer is 4 h.

### 2.4 Frappy (SECoP)

Ryan's team runs a Frappy server. A reference mm4 driver is in `secop/` (pressure module controlling a valve module, SECoP controller pattern); the production driver is being written by a colleague from the handover page. Frappy core ships a Lake Shore 336 driver as well. SECoP has a named pattern for exactly this shape: a `controlled_by` / `control_active` chain where a temperature module controls a pressure module which controls a valve module.

### 2.5 What the Mercury iTC and ITC503 do

The Oxford Instruments auto needle valve handbook for the ITC502/503 (in the project files, "OI - Auto Needle Valve.pdf") describes the scheme the Mercury iTC inherited. In its words: the flow is adjusted "so that the amount of power supplied to the temperature control heater reaches the target value set in a table held in the temperature controller". The heater loop holds the temperature; a second, slow algorithm moves the valve until the heater voltage equals an "ideal heater voltage" looked up against the set temperature. Its tuning parameters are a temperature error sensitivity, a heater error sensitivity, a gas flow scaling factor and a heater voltage limit, and the handbook's own advice is that only the scaling factor normally needs touching while running.

Two things follow for this project. First, the Mercury iTC that the mm4 replaces never controlled the valve from the temperature alone: it balanced the heater. Second, the iTC's needle valve "flow" was the motor position with a flow proxy; this project has a better proxy (the pressure) and a controller that already holds it, so the slow algorithm only has to produce a pressure setpoint.

## 3. The physics that decides the scheme

### 3.1 One temperature, two actuators

The heat exchanger temperature is set by the balance of cooling (helium flow, set by the valve) and heating (the heater, plus the experiment's heat load). Any temperature above base can be held with many combinations of flow and heater power: lots of flow and lots of heater, or little of each. A controller with one setpoint and two actuators needs a second objective to pick between them. The industrial and the Oxford answer is the same: pick the flow so that the heater runs at a chosen fraction of its range (the ITC's target voltage, typically 30 to 50 %). That leaves the heater room to act in both directions, which is what makes the fast loop work, and it uses less helium than running the heater hot against a wide-open valve.

### 3.2 Wet and dry heat exchanger

The 5 November bench run ("Analyse bench gain table data" thread, report and figures in the project files under `bench-data/2026-11-05/`) showed the two regimes of this heat exchanger and why they matter here:

| Regime | What is happening | Pressure versus valve | Temperature |
| --- | --- | --- | --- |
| Dry (warm, no liquid in the heat exchanger) | Helium through the valve boils at once; gas out equals what the valve lets in | Fast, large gain (3 to 5 mbar per % at 1 to 8 mbar) | Set by gas flow against the heat load. Less flow means warmer |
| Wet (cold, liquid pool) | Helium joins a pool; gas out is what the heat load boils off | Slower, smaller gain, drifts with the pool level | Near the saturation temperature of the pressure. Lower pressure means colder (3 to 8 mbar is about 1.5 to 1.6 K on the CF-11) |

The boundary between them is set by the heat load, not the pressure: the same setpoint is wet with the heater off and can be dry with the heater on. In the 5 November run, the steady AUTO blocks read:

| Setpoint, mbar | Valve, % | Input A, K | Input D, K | Saturation T of the pressure, K |
| --- | --- | --- | --- | --- |
| 2 | 4.0 | 30.9 | 3.11 | 1.40 |
| 5 | 4.0 | 20.5 | 3.02 | 1.62 |
| 8 | 4.6 | 15.7 | 3.09 | 1.77 |
| 12 | 4.6 | 3.8 | 3.49 | 1.92 |
| 18 | 8.3 | 4.6 | 3.88 | 2.08 |
| 25 | 11.6 | 5.6 | 4.54 | 2.25 |
| 50 | 31.8 | 8.1 | 7.58 | 2.59 |

The 1 to 8 mbar blocks were dry (the heat exchanger was still cooling from 36 K); liquid arrived during the 8 mbar block; 12 to 50 mbar were wet. Even wet, input A read 2 to 5 K above the saturation temperature, so A is not a liquid thermometer and a temperature-to-pressure table has to be measured, not computed. The CF-11 baseline of about 1.6 K at 3 to 8 mbar, with the heater off and the heat exchanger cold, was not reached in that run.

For temperature control the two regimes map onto two ways of working:

- **Base regime** (setpoint at or near the lowest the cryostat reaches, heater off): the pressure sets the temperature through saturation. The controller should take the pressure setpoint from a measured temperature-to-pressure table and trim it slowly on the temperature error. The heater has no authority here because it would only boil the pool.
- **Heater regime** (setpoint above base, so the heater is doing work): the heater holds the temperature at 10 Hz on the 336, and the valve, through the pressure setpoint, is moved slowly so the heater output sits near its target fraction. This is the ITC scheme. The direction is unambiguous in this regime: more pressure means more flow means more cooling means the heater has to work harder. Heater too high, lower the pressure setpoint; heater near zero (temperature above setpoint with no heating), raise it.

A helium-starved cryostat is a third state, not a regime: on 6 November, with the dewar dry, opening the valve warmed the cold head. In that state neither actuator can hold the temperature, the heater-balance loop would raise the pressure setpoint to its limit, and the heater would sit at 100 %. The scheme has to recognise and stop on it (section 5.4).

### 3.3 Time scales

The pressure loop settles a setpoint change in 1 to 3 minutes (10 mbar in 3 min at KP 0.40 TI 0.75 on 4 November; the fitted time constants are 7 to 24 s). The heater loop on the 336 is faster than that on the heat exchanger and slower on the cold head, which is coupled through the exchanger. The slow coordination loop must therefore be much slower than the pressure loop: one correction every 30 to 60 s, with a time constant of several minutes, so that the pressure loop is always settled when the coordinator looks. That is standard cascade practice (the outer loop 5 to 10 times slower than the inner), and it is also why the coordination does not need to run on the board: a 30 s decision can come over the network.

## 4. The control scheme

This is the scheme any of the three homes in section 5 would run.

```
operator temperature setpoint T_sp
        |
        v
+-------------------+        SETP 1,T_sp ; RANGE 1,r         +-----------------+
| coordinator       | ------------------------------------>  | Lake Shore 336  |--> heater (output 1)
| every 30 to 60 s  | <-----------------------------------   | PID at 10 Hz    |<-- input A (or D)
|                   |        KRDG? A ; HTR? 1 ; HTRST? 1     +-----------------+
|  heater-balance   |
|  or T->p table    |        PRES SP p_sp ; MODE AUTO        +-----------------+
|                   | ------------------------------------>  | mm4             |--> needle valve
|                   | <-----------------------------------   | PI at 1 Hz      |<-- Druck pressure
+-------------------+        PRES? ; MODE? ; ALRM?            +-----------------+
```

1. **Fast loop, in the 336.** Output 1 (or 2, whichever heater is on the heat exchanger) in closed loop PID on the input nearest the heater, with the operator's temperature as `SETP`, a heater range chosen for the temperature band, a setpoint ramp so a large step does not slam both loops, and `TLIMIT` on the controlling input as the heater's own protection. The 336 autotune and zones can be used as on any 336; nothing new.

2. **Inner loop, in the mm4.** AUTO on a pressure setpoint, unchanged. The PID table on the pressure handles the gain change across 1 to 25 mbar. The coordinator is just another remote client writing `PRES SP`.

3. **Slow loop, the coordinator.** Every 30 to 60 s:
   - Read `HTR? 1`, `KRDG? A`, `PRES?`, `MODE?`, `ALRM?`.
   - In the heater regime (heater on and the temperature within a band of its setpoint), move the pressure setpoint by an integral-only rule towards the heater target: `p_sp += Ki * (H_target - H)`, with a deadband of about plus or minus 10 % of range on the heater error so a steady heater does not keep the valve moving, a rate limit (0.5 mbar per step, to be set from the bench), and clamps at the cryostat's working range (1 to 25 mbar by default).
   - In the base regime (operator setpoint at or below what the heater-off table gives, or the heater output pinned at 0 % while the temperature is above setpoint for several minutes), take `p_sp` from the temperature-to-pressure table and trim it on the temperature error, slowly.
   - Keep a single writer: while the coordinator is active the operator sets a temperature, not a pressure, and the pressure setpoint on the mm4 screen is the coordinator's. The SECoP `controlled_by` pattern expresses exactly this.

4. **Tuning knobs**, following the ITC's four: the heater target fraction (start at 40 %), the integral gain of the slow loop (mbar per % of heater error per step), the pressure clamps, and the temperature-to-pressure table for the base regime. Nothing else should be exposed.

The first version does not need gain scheduling or anything cleverer, because the slow loop is integral-only and rate-limited and the heater takes up every fast disturbance. The number that has to be measured before any of this can be set is the **heater percent per mbar of pressure setpoint**, at two or three temperatures (section 6).

## 5. Where the coordination should live

| | A. Control test GUI (PC) | B. Frappy / SECoP (server) | C. mm4 firmware |
| --- | --- | --- | --- |
| Links needed | Both exist today (8888, 7777) | 336 driver in Frappy core; mm4 driver being written | New TCP client to 7777 in CycloneTCP |
| Work to a first run | 1 to 2 days: `HTR?`, `SETP`, `RANGE` in the 336 class, a card with the four knobs, logging | 3 to 5 days once the mm4 driver exists: one Frappy module holding the slow loop and the controlled_by chain, configuration | 2 to 3 weeks plus bench: client socket, 336 IP setting, temperature tile and setpoint, a TEMP mode, cfg record, link-lost rules, manual, simulator, Modbus and text port additions |
| Runs when | The laptop is on and awake | The server is up (always, with its logging) | Always, no PC |
| Who else sees it | Whoever has the GUI page | Every SECoP client, SICS/NICOS, the log | The screen and the mm4's own ports |
| Risk to the pressure controller | None; one more remote client | None | The firmware stops being a pressure controller and depends on another vendor's instrument |
| 336 sockets used (of 2) | 1 while the GUI runs | 1 (Frappy may already hold it) | 1 permanently |
| Fit with the project | Bench proving ground, as the autotune was | The team's supervisory layer; the natural production home | Possible; contradicts the current scope and the feature freeze |

**Recommendation: A then B.** Prove the scheme on the bench in the GUI, because the GUI already talks to both instruments and logs everything, and the bench is where the heater-per-mbar number and the table have to come from anyway. Then write it as a Frappy module, where it runs unattended with the site's logging and is visible to every client, and where a `temperature` module controlling `pressure` controlling `valve` is the pattern SECoP already defines. The firmware needs no change for A or B: the coordinator uses `PRES SP`, `MODE AUTO` and the queries that exist.

**Option C** is kept for one case: a requirement that the cryostat hold a temperature with no PC or server running. If that is ever wanted, this is what it takes and what to watch:

- CycloneTCP can open a client socket from the main loop in the same zero-timeout style as the servers (`socketOpen`, `socketConnect` polled through `tcpGetState`, `socketReceive` with a zero timeout). Memory cost is one socket plus buffers, a few kilobytes; flash is at 661 KB of 1 MB. This is the same proposal as the one made for the GXC on 4 November (read `KRDG?` over 7777) and has not been tried on either board.
- The 336's two-connection limit becomes a site rule: the mm4 would hold one slot permanently, leaving one for Frappy or a terminal.
- The board would need a 336 address setting, a controlling-input setting, a temperature setpoint and tile, a TEMP mode beside MANUAL and AUTO with its own refusals, a decision for every link loss (hold the last pressure setpoint and show it), heater configuration or a rule that the heater is configured on the 336 only, and all of it in the manual, the simulator and both remote maps.
- It couples the firmware to one instrument model. A 340 on another cryostat has no Ethernet.

Two non-options, for the record. The 336 alone cannot do it: it has no way to move the valve. Its analog outputs 3 and 4 cannot feed the mm4 either, because the mm4 has no spare analog input wired (the Adapter Click's AN pad is not routed) and an analog pressure setpoint would need isolation (the 336's output minus is chassis).

### 5.4 Protection the coordinator must have

- **Helium lost**: heater at or near 100 % of range and the pressure setpoint at its upper clamp for more than a few minutes, or input A rising while the valve opens, is the 6 November signature. Stop adjusting, set the heater range to Off (`RANGE 1,0`), leave the mm4 holding its pressure, and raise an alarm. `TLIMIT` on the 336 is the backstop for the sample, independent of the network.
- **336 link lost**: freeze the pressure setpoint and tell the operator. The 336 keeps its heater loop running on its own, the mm4 keeps its pressure; nothing moves to a worse state.
- **mm4 leaves AUTO** (fault, W-2, W-4, W-5, operator STOP): stop the slow loop, keep the heater loop, alarm. Do not restart AUTO from the coordinator after a fault; a person decides (as the handover page already says for drivers).
- **Operator takes the mm4 screen** (changes the setpoint or the mode): the coordinator sees the setpoint it did not write and drops to monitoring. The mm4 screen stays live for a reason.
- **Heater range changes** change what 100 % means; the coordinator should set the range itself from the temperature band and treat a range it did not set as a hand-over to the operator.

## 6. What to find out on the bench before building

These need no firmware change. The GUI can log all of it if `HTR? 1` is added to its 336 poll (a small change, not made here).

1. **Which heater, which sensor.** Confirm on the instrument: which 336 output is wired to the heat exchanger heater and which (if any) to a sample heater, the heater resistance and maximum current (`HTRSET? 1`, `HTRSET? 2`), and which input the loop should control from (A is nearest the exchanger). Confirm the 336 is on Ethernet (`INTSEL`), its address, and whether SICS or Frappy already holds one of its two connections.
2. **Heater loop alone.** With the mm4 holding 8 mbar in AUTO and the heat exchanger cold and steady (A below 5 K, moving less than 0.5 K in 10 min), set `OUTMODE 1,1,A,0`, `RANGE 1,2`, `SETP 1,<A + 1 K>` and watch the heater percentage settle. Run the 336 autotune if it hunts. This tells us the heater has authority at that flow and gives the first PID set.
3. **Heater percent per mbar.** Holding the same temperature, step the pressure setpoint 8 to 5 to 12 mbar and record the steady heater percentage at each. The slope (percent of range per mbar) and how long it takes to settle are the two numbers the slow loop's gain and period come from. Repeat at a second temperature (say 10 K) to see how much the slope changes.
4. **Temperature-to-pressure table for the base regime.** Heater off, cold and wet, hold 2, 3, 5, 8, 12 mbar for 10 min each and record A and D. This replaces the saturation-temperature column above with measured values.
5. **Helium-lost signature.** No test needed: the 6 November log already shows it. Write the thresholds from it.

With those numbers the GUI card (option A) can be built and tried in a day, and the Frappy module (option B) specified with real limits.

## 7. Open questions for Ryan

1. Is the goal a temperature setpoint for operators (replacing "set a pressure") across the cryostat's range, or only better behaviour at base? That decides whether the heater regime matters at all.
2. Which heater outputs are wired, to what, and does the 336 heater loop work today when turned on by hand?
3. Does the site want this to work with no PC or server running (that is the only case for option C)?
4. May the Frappy module own the 336's heater output (setpoint and range), or does another system set the temperature on the 336 today?
5. Should the mm4 screen eventually show "controlled by temperature" and the temperature, so an operator at the cryostat sees why the pressure setpoint is changing? That is the one small firmware item options A and B might want later, and it is not needed for a first run.

## 8. Summary

- Feasible. Both instruments are already on the network with text protocols the GUI and Frappy speak; the 336 has the heater loop built in and the mm4 has the pressure loop.
- The scheme is the ITC/Mercury one: the heater holds the temperature, the valve (through the mm4's pressure setpoint) is moved slowly so the heater sits at a target fraction; at base, a measured temperature-to-pressure table.
- Build it on the PC first to measure the three numbers it needs, then as a Frappy module. Leave the firmware a pressure controller; a direct 336 link in the firmware is possible but is the most work and only needed for a PC-less requirement.
- Before any of it: confirm the heater wiring and the 336 configuration, and run the four bench measurements in section 6.
