# PMC screen simulator

Open `index.html` directly in a browser. No build or server is required. Google Fonts is optional; system fonts are used when unavailable.

This is the standalone browser simulator reviewed on 9 October 2026. It models the proposed 480 × 272 controller interface, motor, pressure control, and cryostat flow line. It does not communicate with hardware and is not the firmware implementation.

The surrounding bench controls, serial console, and physics model are simulation tools. The exported hosting script has been removed; simulator behaviour is otherwise preserved. The UX review recommendations have not been applied.

This folder is independent of `InitialTesting/` and `tools/ui-preview/` and is not part of the firmware build.
