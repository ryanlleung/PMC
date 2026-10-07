# PMC

Pressure-based needle-valve controller firmware for the MikroE Mikromedia 4 (STM32F407ZG), built in NECTO Studio 7 (mikroSDK, LVGL 9.4). The project is in `InitialTesting/`.

## Build notes

- `InitialTesting/pmc_config.h` sets the PC link: `PMC_LINK_ETHERNET 1` gives TCP port 5000 (DHCP, or 169.254.10.50 if no DHCP server answers), and `0` gives the USB COM port. The two can't run together, because RMII needs 50 MHz from MCO1 and USB needs 48 MHz from the same PLL.
- The linker script `.meproject/setup/FileLinker/stm32f407zg.ld` is patched so `.ccmram` is `(NOLOAD)`, which keeps the LVGL heap that lives there out of the flash image. If NECTO ever regenerates this file, re-apply the patch.
- Build Release in NECTO, not Debug (-O0). The display is written one pixel at a time, so an unoptimised build makes redraws slow.
- `build-MM4/` is ignored, except `generated/` (screen code from `main_screen.mscr`) and `build.ninja`.

## Tools

- `tools/logger/log_druck.py <IP or COM>` logs the once-a-second DATA lines to CSV and reconnects on its own.
- `tools/cal/druck_cal.py <IP or COM> show|zs|load|erase` manages the Druck calibration table in serial flash.
- `tools/ui-preview/build.sh` renders the main screen to PNG on a PC.
