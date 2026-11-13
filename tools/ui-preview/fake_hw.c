/* Fake clicks.c and the link for the host render. Readings are fixed
 * per scenario and roughly match the bench on 7 Oct 2026. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "clicks.h"
#include "link.h"
#include "sysinfo.h"
#include "cal.h"
#include "rtclock.h"
#include "fixed.h"

static clicks_state_t st;
static bool fake_wdt;
static char value[24];
static bool fake_saved;

void fake_hw_set_scenario(const char *name)
{
    memset(&st, 0, sizeof st);
    fake_wdt = false;
    fake_saved = false;
    st.cal_nominal = true;
    st.boost_set_mv = 9999;
    st.pm_found = true;
    st.boost_pg = true;
    st.shunt_nv = 94062500;
    st.bus_mv = 10051;
    st.r_ppb = 9358024;
    st.p_mmbar = 967819;
    st.reading_ok = true;
    st.die_mc = 23516;
    strcpy(value, "967.82");

    if (!strcmp(name, "nopm")) {
        st.pm_found = st.reading_ok = false;
        strcpy(value, "----");
    } else if (!strcmp(name, "lowexc")) {
        st.reading_ok = false;
        st.bus_mv = 0;
        st.shunt_nv = 1200;
        strcpy(value, "----");
    } else if (!strcmp(name, "cal")) {
        st.cal_nominal = false;
        fake_saved = true;
        st.p_mmbar = 1013200;
        strcpy(value, "1013.20");
    } else if (!strcmp(name, "neg")) {
        // Pumped below the vacuum point after drift: reads below zero.
        st.cal_nominal = false;
        fake_saved = true;
        st.r_ppb = 127100;
        st.p_mmbar = -123;
        strcpy(value, "-0.12");
    } else if (!strcmp(name, "nodruck")) {
        st.reading_ok = false;
        st.druck_absent = true;
        st.shunt_nv = 3100;
        strcpy(value, "----");
    } else if (!strcmp(name, "unsaved")) {
        st.cal_nominal = false;
        fake_wdt = true;
    } else if (!strcmp(name, "trip")) {
        st.boost_tripped = true;
        st.boost_trip_mv = 11612;
        st.boost_pg = true;
        st.bus_mv = 4987;
        st.reading_ok = false;
        strcpy(value, "----");
    }
}

void clicks_init(void) {}
void clicks_poll(void) {}
const clicks_state_t *clicks_state(void) { return &st; }
#ifndef REAL_CAL
bool cal_is_default(void) { return st.cal_nominal; }
bool cal_saved(void) { return fake_saved; }
void cal_update(const clicks_state_t *s) { (void)s; }
int cal_capture_left(void) { return 0; }
#endif

void link_init(void) {}
void link_task(void) {}
bool link_getline(char *buf, size_t n) { (void)buf; (void)n; return false; }
const char *sysinfo_reset_reason(void) { return fake_wdt ? "watchdog" : "power-on"; }
void sysinfo_restart(void) {}
void link_write(const char *s) { fputs(s, stderr); }   // TREND DUMP (trend.c)
void link_traffic(uint32_t *in, uint32_t *out) { *in = *out = 0; }
uint32_t link_quiet_ms(void) { return UINT32_MAX; }
bool link_client(void) { return false; }
bool rtclock_valid(void) { return !st.cal_nominal; }

/* Stepper 3: settings only, a move completes at once. */
#include "stepper3.h"
#include "limit.h"
static int32_t m_pos;
static uint32_t m_rate = 400, m_start = 200, m_accel = 3200;
static bool m_hold, m_rev;
static uint8_t m_order[4] = { 0, 2, 1, 3 };
static stepper3_stop_t m_stop = STEPPER3_STOP_NONE;

bool stepper3_move(int32_t steps) { m_pos += steps; m_stop = STEPPER3_STOP_DONE; return steps != 0; }
void stepper3_stop(void) {}
void stepper3_off(void) {}
bool fake_busy;   // set by the "moving" scenario
bool stepper3_busy(void) { return fake_busy; }
int32_t stepper3_position(void) { return m_pos; }
int32_t stepper3_target(void) { return m_pos + (fake_busy ? 1000 : 0); }   // "moving" opens
void stepper3_zero(void) { m_pos = 0; }
stepper3_stop_t stepper3_last_stop(void) { return m_stop; }
bool stepper3_limit_open(void) { return limit_state() != LIMIT_CLOSED; }
bool stepper3_limit_pressed(void) { return limit_state() != LIMIT_CLOSED; }
bool stepper3_limit_ignored(void) { return false; }
void stepper3_limit_band(bool on, int32_t lo, int32_t hi) { (void)on; (void)lo; (void)hi; }
bool stepper3_limit_fitted(void) { return true; }
const char *stepper3_refusal(void) { return "refused"; }
bool fake_override;   // set by the "overon" scenario
bool stepper3_override(void) { return fake_override; }
void stepper3_override_arm(void) { fake_override = true; }
bool stepper3_set_rate(uint32_t v) { m_rate = v; return true; }
bool stepper3_set_start(uint32_t v) { m_start = v; return true; }
bool stepper3_set_accel(uint32_t v) { m_accel = v; return true; }
bool stepper3_set_hold(bool on) { m_hold = on; return true; }
bool stepper3_set_reverse(bool on) { m_rev = on; return true; }
bool stepper3_set_order(const uint8_t o[4]) { memcpy(m_order, o, 4); return true; }
uint32_t stepper3_rate(void) { return m_rate; }
uint32_t stepper3_start_rate(void) { return m_start; }
uint32_t stepper3_accel(void) { return m_accel; }
bool stepper3_hold(void) { return m_hold; }
bool stepper3_reverse(void) { return m_rev; }
bool stepper3_energised(void) { return m_hold; }
int8_t stepper3_direction(void) { return 0; }
void stepper3_order(uint8_t o[4]) { memcpy(o, m_order, 4); }
const char *stepper3_stop_name(stepper3_stop_t s) { return s == STEPPER3_STOP_DONE ? "done" : s == STEPPER3_STOP_CMD ? "stopped" : s == STEPPER3_STOP_LIMIT ? "LIMIT" : "none"; }

/* Limit switch: closed, except in the trip scenario. */
limit_state_t limit_state(void) { return st.boost_tripped ? LIMIT_OPEN : LIMIT_CLOSED; }

/* Valve layer, as far as the screens use it. */
#include "valve.h"
const char *stepper3_locked(void) { return NULL; }
void valve_sense(const clicks_state_t *s) { (void)s; }

/* A pump-down (sim_main.c "hist"): sets the reading for step t of 600. */
void fake_hw_pumpdown(int t)
{
    static const int32_t mmbar[] = { 967819, 961000, 640000, 210000, 52000, 9100, 2100, 812, 305, 240, 236 };
    int i = t / 60, f = t % 60;
    st.p_mmbar = mmbar[i] + (mmbar[i + 1] - mmbar[i]) * f / 60;
    st.reading_ok = st.pm_found && !(t > 250 && t < 272);   // a short loss shows as a gap
    if (st.reading_ok)
        fixed_fmt(value, sizeof value, fixed_round(st.p_mmbar, 1), 2);
    else
        strcpy(value, "----");
}

/* A fast rise from 0 (sim_main.c "rise"): 0 and a little below while pumped
 * down, then up to atmosphere. Step t of 60. */
void fake_hw_rise(int t)
{
    st.reading_ok = st.pm_found;
    st.p_mmbar = t < 40 ? (t % 2 ? -400 : 0) : (t - 39) * (t - 39) * 2400;
    fixed_fmt(value, sizeof value, fixed_round(st.p_mmbar, 1), 2);
}


// No second finger in the previews.
bool touch_second(int32_t *x, int32_t *y) { (void)x; (void)y; return false; }
void touch_watch(int32_t x1, int32_t y1, int32_t x2, int32_t y2) { (void)x1; (void)y1; (void)x2; (void)y2; }
bool touch_watch_take(void) { return false; }
