# Third-party notices

The project's own files are licensed under the Apache License 2.0 (`LICENSE`, `NOTICE`). The repository also holds the third-party material below, each under its own licence. The licences are as stated in the files themselves.

| Material | Where | Owner | Licence stated in the files |
| --- | --- | --- | --- |
| LVGL 9.4.0 graphics library | `firmware/lvgl_940/` | LVGL Kft | MIT (`firmware/lvgl_940/LICENCE.txt`) |
| mikroSDK: headers and prebuilt libraries | `firmware/.meproject/setup/` | MikroElektronika d.o.o. | MikroE commercial licence for holders of a NECTO compiler licence (https://www.mikroe.com/legal/software-license-agreement), or LGPL 3.0 for non-commercial projects |
| CycloneTCP network stack, inside the mikroSDK | `firmware/.meproject/setup/*/include/thirdparty/ethernet/cyclonetcp/`, `lib_cyclone_tcp*.a` | Oryx Embedded SARL | GPL 2.0 or later |
| TinyUSB, inside the mikroSDK | `firmware/.meproject/setup/*/include/thirdparty/usb/tinyusb/` | Ha Thach (tinyusb.org) | MIT |
| CMSIS core headers, inside the mikroSDK | `firmware/.meproject/setup/*/include/core/common/cmsis/` | Arm Limited | Apache 2.0 |
| Montserrat Medium font, as bitmap tables | `firmware/ui_font_12.c`, `ui_font_16.c`, `ui_font_26.c`, `ui_font_40.c` | The Montserrat Project Authors | SIL Open Font License 1.1 |
| Board schematics and the Mikromedia 4 manual | `docs/hardware/schematics/` | MikroElektronika d.o.o. | Published by MikroE on each product page; copied unchanged |

## Building and distributing the firmware

The firmware image links the mikroSDK libraries, including CycloneTCP. The Apache 2.0 licence on the project's own files does not change the terms of those libraries. Anyone who builds, ships or sells a device with this firmware must hold terms from MikroE that cover the mikroSDK and CycloneTCP for that use, or meet the LGPL 3.0 and GPL 2.0 terms stated in their files.

The simulator (`simulator/`) and the PC tests compile the project's own sources and LVGL only. They do not include the mikroSDK or CycloneTCP.
