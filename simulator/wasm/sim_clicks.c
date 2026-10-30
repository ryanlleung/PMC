/* Simulator stand-in for clicks.c: the Power Monitor readings come from the
 * bench model instead of I2C. The reading and status logic below is
 * clicks.c's (druck_update and the end of powermonitor_poll). */
#include <string.h>

#include "lvgl.h"
#include "clicks.h"
#include "cal.h"
#include "fixed.h"
#include "stepper3.h"
#include "bench.h"

#define STATUS_LEN 192
#define PM_ADDR_FITTED 0x4A
#define DRUCK_VEXC_MIN_MV    7000
#define DRUCK_R_MIN_PPB      5000
#define DRUCK_R_MAX_PPB      11000000

static char boost10_status[STATUS_LEN];
static char powermonitor_status[STATUS_LEN];
static char druck_status[STATUS_LEN];
static char druck_value[24];
static clicks_state_t state;

static void druck_update(int32_t raw_shunt, int32_t raw_bus, int32_t bus_mv)
{
    state.reading_ok = false;
    state.druck_absent = false;
    if (bus_mv < DRUCK_VEXC_MIN_MV) {
        lv_snprintf(druck_value, sizeof druck_value, "----");
        lv_snprintf(druck_status, STATUS_LEN,
                    "Druck: excitation %ld mV, below 7 V (VBUS not wired?)", (long)bus_mv);
        return;
    }
    int64_t r_ppb = (int64_t)raw_shunt * 1600000 / raw_bus;
    int64_t p = cal_pressure_mmbar(r_ppb);
    state.r_ppb = (int32_t)r_ppb;
    if (r_ppb < DRUCK_R_MIN_PPB || r_ppb > DRUCK_R_MAX_PPB) {
        char r[16];
        state.druck_absent = true;
        lv_snprintf(druck_value, sizeof druck_value, "----");
        lv_snprintf(druck_status, STATUS_LEN,
                    "Druck: signal %s mV/V, outside 0.005-11 mV/V (not connected?)",
                    fixed_fmt(r, sizeof r, r_ppb / 100, 4));
        return;
    }
    state.reading_ok = true;
    state.p_mmbar = (int32_t)p;
    char r[16];
    fixed_fmt(druck_value, sizeof druck_value, fixed_round(p, 1), 2);
    lv_snprintf(druck_status, STATUS_LEN,
                "Druck: %s mbar abs  r %s mV/V  exc %ld mV  %s",
                druck_value, fixed_fmt(r, sizeof r, r_ppb / 10, 5),
                (long)bus_mv, cal_is_default() ? "nominal cal" : cal_id());
}

static void powermonitor_poll(void)
{
    if (!B.pm) {
        lv_snprintf(powermonitor_status, STATUS_LEN,
                    "S4 Power Monitor: NOT FOUND (no reply at 0x40-0x4F,\n   SCL 1 SDA 1)");
        lv_snprintf(druck_value, sizeof druck_value, "----");
        lv_snprintf(druck_status, STATUS_LEN, "Druck: no reading (Power Monitor not found)");
        state.pm_found = false;
        state.reading_ok = false;
        return;
    }
    int32_t raw_shunt, raw_bus;
    int16_t raw_temp;
    bench_read(&raw_shunt, &raw_bus, &raw_temp);

    int32_t shunt_0u1 = (raw_shunt * 3125) / 1000;
    int32_t bus_mv = (int32_t)(((int64_t)raw_bus * 1953125) / 10000000);
    int32_t temp_d1 = (raw_temp * 5) / 64;

    state.pm_found = true;
    state.shunt_nv = (int32_t)(((int64_t)raw_shunt * 3125) / 10);
    state.bus_mv = bus_mv;
    state.die_mc = (raw_temp * 125) / 16;
    druck_update(raw_shunt, raw_bus, bus_mv);

    char sh[16], tc[16];
    lv_snprintf(powermonitor_status, STATUS_LEN,
                "S4 Power Monitor: INA228 found @0x%02X\n"
                "   Vshunt %s uV   Vbus %ld mV   die %s C",
                PM_ADDR_FITTED, fixed_fmt(sh, sizeof sh, shunt_0u1, 1), (long)bus_mv,
                fixed_fmt(tc, sizeof tc, temp_d1, 1));
}

static void boost10_poll(void)
{
    state.boost_set_mv = 9999;
    state.boost_pg = B.exc_v > 9.0;
    lv_snprintf(boost10_status, STATUS_LEN, "S3 Boost 10: wiper 33 (9.99 V nom), %s",
                state.boost_pg ? "PG ok" : "PG LOW");
}

void clicks_init(void)
{
    stepper3_init();
    clicks_poll();
}

// Pressure 21 (0.14.0): room pressure plus its error, 0.01 mbar steps as
// the BMP581's last digit, 23 degC.
static void baro_sim(void)
{
    state.baro_ok = B.baro;
    state.baro_mmbar = (int32_t)((B.room_mbar + B.baro_err) * 100 + 0.5) * 10;
    state.baro_mc = 23000;
}

void clicks_poll(void)
{
    powermonitor_poll();
    boost10_poll();
    baro_sim();
}

const char *clicks_stepper3_status(void)     { return stepper3_status(); }
const char *clicks_boost10_status(void)      { return boost10_status; }
const char *clicks_powermonitor_status(void) { return powermonitor_status; }
const char *clicks_druck_status(void)        { return druck_status; }
const char *clicks_druck_value(void)         { return druck_value; }
bool clicks_druck_cal_nominal(void)          { return cal_is_default(); }
const clicks_state_t *clicks_state(void)
{
    state.cal_nominal = cal_is_default();
    return &state;
}
