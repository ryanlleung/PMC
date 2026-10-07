/* Fake clicks.c and usb_serial.c for the host render. Readings are fixed
 * per scenario and roughly match the bench on 7 Oct 2026. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "clicks.h"
#include "usb_serial.h"

static clicks_state_t st;
static char value[24];

void fake_hw_set_scenario(const char *name)
{
    memset(&st, 0, sizeof st);
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
int32_t clicks_boost10_set_mv(int32_t mv) { return st.boost_tripped ? -1 : mv; }

void usb_serial_init(void) {}
void usb_serial_task(void) {}
const char *usb_serial_status(void) { return "USB: port open"; }
// COM3 output goes to stderr so the DATA line format can be checked.
void usb_serial_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}
