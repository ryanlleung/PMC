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
#include "uncert.h"

static clicks_state_t st;
static bool fake_wdt;
static char value[24];
static bool fake_saved, fake_atm;

void fake_hw_set_scenario(const char *name)
{
    memset(&st, 0, sizeof st);
    fake_wdt = false;
    fake_saved = fake_atm = false;
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
        fake_saved = fake_atm = true;
        st.p_mmbar = 1013200;
        strcpy(value, "1013.20");
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
bool cal_is_default(void) { return st.cal_nominal; }
const char *cal_id(void) { return fake_atm ? "5880156+atm" : "5880156"; }
int32_t cal_atm_ppm(void) { return fake_atm ? 957300 : 1000000; }
int32_t cal_zero_ppb(void) { return st.cal_nominal ? 0 : 133700; }
int32_t cal_span_ppb(void) { return st.cal_nominal ? 10000000 : 10099600; }
bool cal_saved(void) { return fake_saved; }
bool cal_atm_applied(void) { return fake_atm; }
int32_t clicks_boost10_set_mv(int32_t mv) { return st.boost_tripped ? -1 : mv; }

void link_init(void) {}
void link_task(void) {}
bool link_getline(char *buf, size_t n) { (void)buf; (void)n; return false; }
const char *link_status(void) { return "Ethernet: 192.168.1.23 port 5000, client connected"; }
const char *link_chip_text(void) { return "192.168.1.23"; }
bool link_chip_ok(void) { return true; }
bool link_take_new_client(void) { return false; }
const char *sysinfo_reset_reason(void) { return fake_wdt ? "watchdog" : "power-on"; }
bool sysinfo_reset_abnormal(void) { return fake_wdt; }
const char *i2c_sdk_test_result(void) { return ""; }
const char *sysinfo_line(void) { return "PMC firmware 0.3.0 (preview), last reset: power-on"; }
// Link output goes to stderr so the DATA line format can be checked.
void link_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}
bool rtclock_valid(void) { return !st.cal_nominal; }
const char *rtclock_screen_text(void) { return st.cal_nominal ? "time not set" : "08 Oct  12:34"; }
void uncert_update(const clicks_state_t *s) { (void)s; }
const char *uncert_screen_text(bool *warn)
{
    *warn = st.cal_nominal;
    if (!st.reading_ok) return "";
    return st.cal_nominal ? "+/- ? (not calibrated)" : "+/- 1.08 mbar";
}

/* Stepper 3: settings only, a move completes at once. */
#include "stepper3.h"
static int32_t m_pos;
static uint32_t m_rate = 400, m_start = 200, m_accel = 3200;
static stepper3_mode_t m_mode = STEPPER3_FULL;
static bool m_hold, m_rev;
static uint8_t m_order[4] = { 0, 2, 1, 3 };
static stepper3_stop_t m_stop = STEPPER3_STOP_NONE;

bool stepper3_move(int32_t steps) { m_pos += steps; m_stop = STEPPER3_STOP_DONE; return steps != 0; }
void stepper3_stop(void) {}
void stepper3_off(void) {}
bool stepper3_busy(void) { return false; }
int32_t stepper3_position(void) { return m_pos; }
void stepper3_zero(void) { m_pos = 0; }
stepper3_stop_t stepper3_last_stop(void) { return m_stop; }
bool stepper3_limit_open(void) { return false; }
bool stepper3_limit_fitted(void) { return false; }
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
bool stepper3_hold(void) { return m_hold; }
bool stepper3_reverse(void) { return m_rev; }
bool stepper3_energised(void) { return m_hold; }
int8_t stepper3_direction(void) { return 0; }
void stepper3_order(uint8_t o[4]) { memcpy(o, m_order, 4); }
const char *stepper3_mode_name(stepper3_mode_t m) { return m == STEPPER3_WAVE ? "WAVE" : m == STEPPER3_FULL ? "FULL" : "HALF"; }
const char *stepper3_stop_name(stepper3_stop_t s) { return s == STEPPER3_STOP_DONE ? "done" : s == STEPPER3_STOP_CMD ? "stopped" : s == STEPPER3_STOP_LIMIT ? "LIMIT" : "none"; }
