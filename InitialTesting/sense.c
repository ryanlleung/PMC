#include <string.h>

#include "lvgl.h"        // lv_tick_get, lv_strlcpy
#include "pmc_config.h"
#include "link.h"
#include "clicks.h"
#include "cal.h"
#include "stepper3.h"
#include "valve.h"
#include "trend.h"
#include "sense.h"

/*
 * Once a second from the main loop (was main_screen.c to 0.8.x): re-read
 * the boards, update the cal and valve fault checks. Outside LVGL, so a
 * slow redraw does not delay the reading or the fault checks.
 * The remote set (remote.c) has PRESSURE?, VALVE?, ALARM? and the rest.
 */

void sense_second(void)
{
    clicks_poll();

    const clicks_state_t *s = clicks_state();
    cal_update(s);
    valve_sense(s);
    trend_second(s);
}
