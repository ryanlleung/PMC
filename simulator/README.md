# PMC screen simulator

Open `index.html` directly in a browser. No build or server is required. Google Fonts is optional; system fonts are used when unavailable.

Live view of the working branch: https://raw.githack.com/ryanlleung/PMC/claude/project-thread-8yhtvn/simulator/index.html

The simulator models the proposed 480 x 272 controller interface, the motor, pressure control and the cryostat flow line. It does not communicate with hardware and is not the firmware implementation. The bench controls, laptop configuration tool, serial console and physics model below the screen are simulation tools.

## UI/UX review, 9 October 2026 (v46 to v49)

- v46, operator screens and navigation: Target marked inactive or paused outside AUTO; valve card "Valve position" with Opening / Closing / Stationary and Requested; AUTO status Approaching target / Within target band / Stable; Home renamed Reference valve; Valve page without presets or the 100 % travel setting; task menu (Trend, Alarms and events, Reference valve, System information, Commissioning); Back returns one level.
- v47, commissioning access and configuration: one commissioning session (PIN 2468 in the simulator, 10 min idle timeout, shared with serial `ACCESS`); Sensor, Valve setup, Control tuning, Diagnostics and Configuration pages; every setting goes draft, Apply, Save for restart; configuration name and revision; laptop import as a draft; Target max and low alarm separate from sensor range and Target min; readings go through zero, span, cal factor or an imported table.
- v48, STOP and faults: STOPPED / FAULT in the header; STOP says the valve was not commanded closed; Reset fault replaces ACK and never starts AUTO; fault detail with what happened, controller action, what to check and reset condition. Simulator fixes: reboot loads the saved configuration and starts not referenced; device trend without the true mechanism series; event list keeps updating with 200 events stored.
- v49, follow-up review: limit changes that exclude the target move it after one confirmation; Configuration shows Not applied and Not saved lists with labels and units; calibration identity and saved state on Sensor and System information.

Page load still starts referenced at 0 % in MANUAL as a simulator convenience. Settings saved in the simulator are kept in the browser's local storage; the laptop panel has Erase simulated flash.

This folder is independent of `InitialTesting/` and `tools/ui-preview/` and is not part of the firmware build. Firmware has not been changed to match.
