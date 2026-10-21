#include <string.h>

#include "lvgl.h"        // lv_tick_get, lv_strlcpy
#include "link.h"
#include "clicks.h"
#include "cal.h"
#include "sysinfo.h"
#include "uncert.h"
#include "fixed.h"
#include "valve.h"
#include "trend.h"
#include "sense.h"

/*
 * Once a second from the main loop (was main_screen.c to 0.8.x): re-read
 * the boards, update the cal, uncertainty and valve fault checks, print any
 * changed status line and the DATA line. Outside LVGL, so a slow redraw
 * does not delay the reading or the fault checks.
 *
 * Once a second, one CSV line on COM3 for logging (tools/logger):
 *   DATA,t_ms,p_mbar,signal_uV,exc_mV,ratio_mV_per_V,die_C,set_mV,ok
 * p_mbar and ratio are empty when there is no valid reading (ok = 0).
 * Lines not starting with DATA are status text.
 */
#define STATUS_LINES 6
static const char *last_line[STATUS_LINES];
static char line_copy[STATUS_LINES][192];

// DATA lines once a second; DATA OFF pauses them and the live status lines.
// Off at boot; DATA ON (the logger sends it) turns them on.
static bool data_on = false;

bool sense_data_command(const char *line)
{
    if (strcmp(line, "DATA OFF") == 0)
        data_on = false;
    else if (strcmp(line, "DATA ON") == 0)
        data_on = true;
    else
        return false;
    link_printf("OK data %s\r\n", data_on ? "on" : "off");
    return true;
}

static void print_data_line(const clicks_state_t *s)
{
    char p[16] = "", r[16] = "", sig[16], die[16];

    if (s->reading_ok) {
        fixed_fmt(p, sizeof p, s->p_mmbar, 3);
        fixed_fmt(r, sizeof r, s->r_ppb, 6);
    }
    link_printf("DATA,%lu,%s,%s,%ld,%s,%s,%ld,%d\r\n",
                (unsigned long)lv_tick_get(), p,
                fixed_fmt(sig, sizeof sig, s->shunt_nv / 100, 1),   // 0.1 uV
                (long)s->bus_mv, r, fixed_fmt(die, sizeof die, s->die_mc, 3),
                (long)s->boost_set_mv, s->reading_ok ? 1 : 0);
}

static void print_changed_lines(void)
{
    const char *line[STATUS_LINES] = { sysinfo_line(), clicks_stepper3_status(), clicks_boost10_status(),
                                       clicks_powermonitor_status(), clicks_druck_status(), valve_status() };

    // A PC that connects later still gets every line once.
    bool resend = link_take_new_client();

    // The Power Monitor and Druck lines carry live readings and change every
    // second, so DATA OFF holds them back too.
    const bool live[STATUS_LINES] = { false, false, false, true, true, false };
    // In AUTO the motor line changes every second: the AUTO line says it better.
    bool in_auto = valve_auto();

    for (int i = 0; i < STATUS_LINES; i++) {
        if (!line[i][0] || (live[i] && !data_on) || (i == 1 && in_auto))
            continue;
        if (resend || last_line[i] == NULL || strcmp(line_copy[i], line[i]) != 0) {
            lv_strlcpy(line_copy[i], line[i], sizeof line_copy[i]);
            last_line[i] = line_copy[i];
            link_printf("%s\r\n", line[i]);
        }
    }
}

void sense_second(void)
{
    clicks_poll();
    print_changed_lines();

    const clicks_state_t *s = clicks_state();
    uncert_update(s);
    cal_update(s);
    valve_sense(s);
    trend_second(s);
    if (valve_auto())
        link_printf("%s\r\n", valve_auto_line());   // one readable line a second in AUTO
    if (s->pm_found && data_on && !s->pm_diag)
        print_data_line(s);
}
