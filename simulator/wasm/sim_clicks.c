/* Simulator stand-in for clicks.c: the Power Monitor readings come from the
 * bench model instead of I2C. The reading logic below is clicks.c's
 * (druck_update and the end of powermonitor_poll). */
#include <string.h>

#include "lvgl.h"
#include "clicks.h"
#include "cal.h"
#include "stepper3.h"
#include "bench.h"

#define DRUCK_R_MIN_PPB      5000
#define DRUCK_R_MAX_PPB      11000000

static clicks_state_t state;

static void druck_update(int32_t raw_shunt, int32_t raw_bus, int32_t bus_mv)
{
    state.reading_ok = false;
    state.druck_absent = false;
    if (bus_mv < DRUCK_VEXC_MIN_MV)
        return;
    int64_t r_ppb = (int64_t)raw_shunt * 1600000 / raw_bus;
    int64_t p = cal_pressure_mmbar(r_ppb);
    state.r_ppb = (int32_t)r_ppb;
    if (r_ppb < DRUCK_R_MIN_PPB || r_ppb > DRUCK_R_MAX_PPB) {
        state.druck_absent = true;
        return;
    }
    state.reading_ok = true;
    state.p_mmbar = (int32_t)p;
}

static void powermonitor_poll(void)
{
    if (!B.pm) {
        state.pm_found = false;
        state.reading_ok = false;
        return;
    }
    int32_t raw_shunt, raw_bus;
    int16_t raw_temp;
    bench_read(&raw_shunt, &raw_bus, &raw_temp);

    int32_t bus_mv = (int32_t)(((int64_t)raw_bus * 1953125) / 10000000);

    state.pm_found = true;
    state.shunt_nv = (int32_t)(((int64_t)raw_shunt * 3125) / 10);
    state.bus_mv = bus_mv;
    state.die_mc = (raw_temp * 125) / 16;
    druck_update(raw_shunt, raw_bus, bus_mv);
}

static void boost10_poll(void)
{
    state.boost_set_mv = 9999;
    state.boost_pg = B.exc_v > 9.0;
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

const clicks_state_t *clicks_state(void)
{
    state.cal_nominal = cal_is_default();
    return &state;
}
