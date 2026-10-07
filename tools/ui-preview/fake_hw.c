/* Fake clicks.c and usb_serial.c for the host render. Readings are fixed
 * per scenario and roughly match the bench on 7 Oct 2026. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "clicks.h"
#include "link.h"
#include "sysinfo.h"
#include "cal.h"

static clicks_state_t st;
static bool fake_wdt;
static char value[24];

void fake_hw_set_scenario(const char *name)
{
    memset(&st, 0, sizeof st);
    fake_wdt = false;
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
const char *cal_id(void) { return "cert 1234567"; }
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
const char *sysinfo_line(void) { return "PMC firmware 0.3.0 (preview), last reset: power-on"; }
// Link output goes to stderr so the DATA line format can be checked.
void link_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}
