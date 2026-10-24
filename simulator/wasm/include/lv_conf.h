/* Simulator lv_conf.h: the firmware's own configuration, with the LVGL heap
 * out of the CCM section (no such thing in WebAssembly) and an assert that
 * stops the simulated board the way the watchdog would. */
#include "../../../InitialTesting/lvgl_940/lv_conf/heavy/lv_conf.h"

#undef LV_ATTRIBUTE_LARGE_RAM_ARRAY
#define LV_ATTRIBUTE_LARGE_RAM_ARRAY

void sim_halt(const char *why);
#undef LV_ASSERT_HANDLER
#define LV_ASSERT_HANDLER sim_halt("LVGL assert");
