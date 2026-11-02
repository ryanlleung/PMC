#include <string.h>

#include "lvgl.h"        // lv_tick_get, lv_strlcpy
#include "pmc_config.h"
#include "link.h"
#include "clicks.h"
#include "cal.h"
#include "stepper3.h"
#include "fixed.h"
#include "valve.h"
#include "trend.h"
#include "sense.h"

/*
 * Once a second from the main loop (was main_screen.c to 0.8.x): re-read
 * the boards, update the cal and valve fault checks. Outside LVGL, so a
 * slow redraw does not delay the reading or the fault checks.
 * The remote set (remote.c) has PRES?, VALV?, ALRM? and the rest.
 */

static bool hot;
static int32_t hot_mc;

static void hot_check(const clicks_state_t *s)
{
    if (!s->pm_found)
        return;   // no reading: as it was
    hot_mc = s->die_mc;
    char t[16];
    if (!hot && s->die_mc >= SENSE_HOT_MC) {
        hot = true;
        valve_event("W-3", "Controller hot, %s C", fixed_fmt(t, sizeof t, s->die_mc / 100, 1));
    } else if (hot && s->die_mc < SENSE_COOL_MC) {
        hot = false;
        valve_event("W-3", "Controller cooled, %s C", fixed_fmt(t, sizeof t, s->die_mc / 100, 1));
    }
}

bool sense_hot(int32_t *die_mc)
{
    *die_mc = hot_mc;
    return hot;
}

void sense_second(void)
{
    clicks_poll();

    const clicks_state_t *s = clicks_state();
    cal_update(s);
    valve_sense(s);
    trend_second(s);
    hot_check(s);
}
