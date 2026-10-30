/* Fake clicks.c and usb_serial.c for the host render. Readings are fixed
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
static bool fake_saved, fake_bench;

void fake_hw_set_scenario(const char *name)
{
    memset(&st, 0, sizeof st);
    fake_wdt = false;
    fake_saved = fake_bench = false;
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
        fake_saved = fake_bench = true;
        st.p_mmbar = 1013200;
        strcpy(value, "1013.20");
    } else if (!strcmp(name, "neg")) {
        // Pumped below the vacuum point after drift: reads below zero.
        st.cal_nominal = false;
        fake_saved = fake_bench = true;
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
const char *clicks_stepper3_status(void) { return "S2 Stepper 3: pins set, coils off (no readback)"; }
const char *clicks_boost10_status(void) { return "S3 Boost 10: fake"; }
const char *clicks_powermonitor_status(void) { return "S4 Power Monitor: fake"; }
const char *clicks_druck_status(void) { return "Druck: fake"; }
const char *clicks_druck_value(void) { return value; }
bool clicks_druck_cal_nominal(void) { return st.cal_nominal; }
const clicks_state_t *clicks_state(void) { return &st; }
#ifndef REAL_CAL
bool cal_is_default(void) { return st.cal_nominal; }
const char *cal_id(void) { return fake_bench ? "5880156 bench" : "5880156"; }
bool cal_saved(void) { return fake_saved; }
void cal_update(const clicks_state_t *s) { (void)s; }
uint32_t cal_old_version(void) { return 0; }
int cal_capture_left(void) { return 0; }
bool cal_bench(int32_t *r_vac, int32_t *p_vac, int32_t *r_air, int32_t *p_air)
{
    if (!fake_bench) return false;
    *r_vac = 127900; *p_vac = 0;
    *r_air = 9798300; *p_air = 1013250;
    return true;
}
#endif

void link_init(void) {}
void link_task(void) {}
bool link_getline(char *buf, size_t n) { (void)buf; (void)n; return false; }
const char *link_status(void) { return "Ethernet: 192.168.1.23 port 8888, client connected"; }
const char *link_chip_text(void) { return "192.168.1.23"; }
bool link_chip_ok(void) { return true; }
bool link_take_new_client(void) { return false; }
const char *sysinfo_reset_reason(void) { return fake_wdt ? "watchdog" : "power-on"; }
bool sysinfo_reset_abnormal(void) { return fake_wdt; }
const char *sysinfo_line(void) { return "PMC firmware 0.3.0 (preview), last reset: power-on"; }
void sysinfo_restart(void) {}
// Link output goes to stderr so the DATA line format can be checked.
size_t link_tx_room(void) { return 4096; }
void link_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}
bool rtclock_valid(void) { return !st.cal_nominal; }
const char *rtclock_screen_text(void) { return st.cal_nominal ? "time not set" : "08 Oct  12:34"; }

/* Stepper 3: settings only, a move completes at once. */
#include "stepper3.h"
#include "limit.h"
static int32_t m_pos;
static uint32_t m_rate = 400, m_start = 200, m_accel = 3200;
static stepper3_mode_t m_mode = STEPPER3_FULL;
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
bool stepper3_limit_fitted(void) { return true; }
const char *stepper3_refusal(void) { return "refused"; }
bool fake_override;   // set by the "overon" scenario
bool stepper3_override(void) { return fake_override; }
void stepper3_override_arm(void) { fake_override = true; }
bool stepper3_set_rate(uint32_t v) { m_rate = v; return true; }
bool stepper3_set_start(uint32_t v) { m_start = v; return true; }
bool stepper3_set_accel(uint32_t v) { m_accel = v; return true; }
bool stepper3_set_mode(stepper3_mode_t m) { m_mode = m; return true; }
bool stepper3_set_hold(bool on) { m_hold = on; return true; }
bool stepper3_set_reverse(bool on) { m_rev = on; return true; }
bool stepper3_set_order(const uint8_t o[4]) { memcpy(m_order, o, 4); return true; }
uint32_t stepper3_rate(void) { return m_rate; }
uint32_t stepper3_start_rate(void) { return m_start; }
uint32_t stepper3_accel(void) { return m_accel; }
stepper3_mode_t stepper3_mode(void) { return m_mode; }
int32_t stepper3_scale(void) { return m_mode == STEPPER3_HALF ? 2 : 1; }
bool stepper3_hold(void) { return m_hold; }
bool stepper3_reverse(void) { return m_rev; }
bool stepper3_energised(void) { return m_hold; }
int8_t stepper3_direction(void) { return 0; }
void stepper3_order(uint8_t o[4]) { memcpy(o, m_order, 4); }
const char *stepper3_mode_name(stepper3_mode_t m) { return m == STEPPER3_WAVE ? "WAVE" : m == STEPPER3_FULL ? "FULL" : "HALF"; }
const char *stepper3_stop_name(stepper3_stop_t s) { return s == STEPPER3_STOP_DONE ? "done" : s == STEPPER3_STOP_CMD ? "stopped" : s == STEPPER3_STOP_LIMIT ? "LIMIT" : "none"; }

/* Limit switch: closed, except in the trip scenario. */
limit_state_t limit_state(void) { return st.boost_tripped ? LIMIT_OPEN : LIMIT_CLOSED; }
const char *limit_text(void) { return limit_state() == LIMIT_OPEN ? "OPEN" : "closed"; }

/* Valve layer, as far as the screens use it. */
#include "valve.h"
const char *stepper3_locked(void) { return NULL; }
void valve_sense(const clicks_state_t *s) { (void)s; }
const char *valve_status(void) { return "Valve: MANUAL"; }

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

// W-3 (sense.h): set by the "hot" scenario.
bool fake_hot;
bool sense_hot(int32_t *die_mc) { *die_mc = 57300; return fake_hot; }

// No second finger in the previews.
bool touch_second(int32_t *x, int32_t *y) { (void)x; (void)y; return false; }
