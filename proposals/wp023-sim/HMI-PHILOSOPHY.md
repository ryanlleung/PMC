# PMC HMI philosophy

Rev A, 13 Oct 2026. Proposed, not approved. Basis: ANSI/ISA-101.01 via Rockwell PROCES-WP023 s1 p4 and s1.1 p5; operator observation of the Mercury iTC (13 Oct 2026); Ryan's decisions recorded in the project.

## Purpose

The PMC holds the pot pressure of an Oxford Instruments cryostat at a target the operator chooses, by moving a needle valve. Pressure is a rough proxy for helium flow. Flow is not measured and the HMI never calls anything flow.

## Users

- Primary: instrument operators (WOMBAT, TAIPAN, ECHIDNA, PELICAN, SIKA sample environment). They set a target pressure from experience (typically 1 to 25 mbar, different for each cryostat), switch between MANUAL and AUTO, and move the valve by hand during cool-down. They do not tune the controller. While doing this they are reading temperature on a separate controller.
- Secondary: sample environment engineers. They commission sensors, valves, limits and tuning behind a PIN.

## Principles

1. Steadiness before precision. The operator's question is "is it holding?", so the home screen shows the last 10 minutes as well as the present value.
2. Glanceable from beside the cryostat. Readings are large, there are few words, and the layout is the same on every screen.
3. Grey is normal. Colour appears only when something is abnormal. Red means an alarm (and the STOP button, the one documented exception).
4. Alarms are noticed without looking at the PMC. A new fault blinks and sounds until it is acknowledged. Acknowledging and resetting are separate.
5. Show only the precision the measurement supports: 0.1 mbar on operator screens, 0.01 mbar for engineers.
6. Every command says what it will do. Changes of mode ask first. The PMC never picks a target for the operator.
7. STOP stops motion and AUTO. It does not close the valve and is not a safety function.
8. Operators see operator tasks. Setup lives behind Commissioning and the PIN.
9. Designs are tested with operators at physical size before they are frozen in firmware.

## Scope

The 480 × 272 capacitive touch panel on the Mikromedia 4 (LVGL 9.4), and the screen simulator that prototypes it. The style guide (`STYLE-GUIDE.md`) turns these principles into rules.
