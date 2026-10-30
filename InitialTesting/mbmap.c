#include <math.h>
#include <string.h>

#include "pmc_config.h"
#include "clicks.h"
#include "fixed.h"
#include "lvgl.h"   // lv_tick_get, lv_strlcpy
#include "cfg.h"
#include "modbus.h"
#include "sense.h"
#include "stepper3.h"
#include "valve.h"

/* --------------------------------------------------------------------------
 * Modbus register map, version 1. docs/modbus-register-map.md is the
 * reference; keep the two in step. Floats are IEEE 754 single in two
 * registers, high word first (ABCD). NaN means no value (no reading, no
 * target set).
 *
 * Writes go through the same calls as the screen buttons, so every check
 * and refusal is the same; they are refused while PMC_MODBUS_WRITE is 0.
 * Each accepted write is logged as a NET event with the client's address.
 * ------------------------------------------------------------------------ */

#define MAP_VERSION 1

enum {
    HR_VERSION = 0, HR_HEARTBEAT, HR_MODE, HR_STATUS, HR_REGULATION,
    HR_FAULTS, HR_WARNINGS, HR_FAULT_COUNT, HR_EVENTS, HR_RESERVED,
    HR_PRESSURE = 10, HR_TARGET = 12, HR_VALVE = 14, HR_VALVE_TARGET = 16,
    HR_RANGE = 18, HR_EXCITATION = 20, HR_TEMP = 22, HR_UPTIME = 24,
    HR_COUNT = 26
};

// Command coils (read and write) and status coils (read only).
enum { CO_AUTO = 0, CO_STOP, CO_RESET, CO_ACK, CO_STATUS = 16, CO_COUNT = CO_STATUS + 10 };

// Bits of HR_STATUS, and coils CO_STATUS + bit.
enum {
    ST_READING_OK, ST_POSITION_SET, ST_HOMED, ST_MOVING, ST_SWITCH,
    ST_FAULT, ST_WARNING, ST_TARGET_SET, ST_WRITES, ST_MOTOR_SUPPLY
};

static uint16_t reg[HR_COUNT];
static char peer[16] = "?";

void mbmap_set_peer(const char *ip)
{
    lv_strlcpy(peer, ip && ip[0] ? ip : "?", sizeof peer);
}

static void put_f32(int at, float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    reg[at] = (uint16_t)(u >> 16);
    reg[at + 1] = (uint16_t)u;
}

static float get_f32(const uint16_t *v)
{
    uint32_t u = (uint32_t)v[0] << 16 | v[1];
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

// PMC_MODBUS_WRITE 0 refuses writes whatever the Remote control switch
// says; with 1, writes need the switch On (0.13.4).
bool mbmap_writes_allowed(void)
{
    return PMC_MODBUS_WRITE != 0 && cfg_remote();
}

void mbmap_begin(void)
{
    const clicks_state_t *s = clicks_state();
    int32_t sp, die;
    bool sp_set = valve_target(&sp);
    const char *w = valve_warning();
    bool hot = sense_hot(&die);
    uint32_t up_s = lv_tick_get() / 1000u;

    uint16_t st = 0;
    if (s->reading_ok)                         st |= 1u << ST_READING_OK;
    if (valve_calibrated())                    st |= 1u << ST_POSITION_SET;
    if (valve_reference() == VALVE_REF_HOMED)  st |= 1u << ST_HOMED;
    if (stepper3_busy())                       st |= 1u << ST_MOVING;
    if (stepper3_limit_open())                 st |= 1u << ST_SWITCH;
    if (valve_fault_latched())                 st |= 1u << ST_FAULT;
    if (w || hot || valve_question(NULL))      st |= 1u << ST_WARNING;
    if (sp_set)                                st |= 1u << ST_TARGET_SET;
    if (mbmap_writes_allowed())                st |= 1u << ST_WRITES;
    st |= 1u << ST_MOTOR_SUPPLY;   // no supply sense from 0.14.3: kept at 1 so the map does not change

    static const char *const ftag[] = { "F-1", "F-2", "F-3", "F-4", "F-5", "F-6" };
    uint16_t f = 0;
    for (int i = 0; i < 6; i++)
        if (valve_fault_is(ftag[i]))
            f |= (uint16_t)(1u << i);

    // W-1 is the switch question, W-3 the temperature; the others are the
    // strip warning, one at a time.
    uint16_t wb = 0;
    if (valve_question(NULL))
        wb |= 1u << 0;
    if (hot)
        wb |= 1u << 2;
    if (w && w[0] == 'W' && w[1] == '-' && w[2] >= '1' && w[2] <= '6' && w[3] == ' ')
        wb |= (uint16_t)(1u << (w[2] - '1'));

    memset(reg, 0, sizeof reg);
    reg[HR_VERSION] = MAP_VERSION;
    reg[HR_HEARTBEAT] = (uint16_t)up_s;
    reg[HR_MODE] = (uint16_t)valve_mode();
    reg[HR_STATUS] = st;
    reg[HR_REGULATION] = valve_auto() ? (uint16_t)valve_regulation() : 0;
    reg[HR_FAULTS] = f;
    reg[HR_WARNINGS] = wb;
    reg[HR_FAULT_COUNT] = (uint16_t)valve_fault_count();
    reg[HR_EVENTS] = (uint16_t)valve_event_total();
    put_f32(HR_PRESSURE, s->reading_ok ? s->p_mmbar / 1000.0f : NAN);
    put_f32(HR_TARGET, sp_set ? sp / 1000.0f : NAN);
    put_f32(HR_VALVE, valve_calibrated() ? valve_pct_milli() / 1000.0f : NAN);
    put_f32(HR_VALVE_TARGET, valve_calibrated() ? valve_target_pct_milli() / 1000.0f : NAN);
    put_f32(HR_RANGE, valve_range_mmbar() / 1000.0f);
    put_f32(HR_EXCITATION, s->pm_found ? s->bus_mv / 1000.0f : NAN);
    put_f32(HR_TEMP, s->pm_found ? s->die_mc / 1000.0f : NAN);
    reg[HR_UPTIME] = (uint16_t)(up_s >> 16);
    reg[HR_UPTIME + 1] = (uint16_t)up_s;
}

bool mbmap_read_reg(uint16_t addr, uint16_t *v)
{
    if (addr >= HR_COUNT)
        return false;
    *v = reg[addr];
    return true;
}

bool mbmap_read_coil(uint16_t addr, bool *v)
{
    if (addr < CO_STATUS) {
        if (addr > CO_ACK)
            return false;
        switch (addr) {
        case CO_AUTO:  *v = valve_auto(); break;
        case CO_RESET: *v = valve_fault_latched(); break;
        case CO_ACK:   *v = valve_warning() != NULL; break;
        default:       *v = false; break;
        }
        return true;
    }
    if (addr >= CO_COUNT)
        return false;
    *v = (reg[HR_STATUS] >> (addr - CO_STATUS)) & 1u;
    return true;
}

static uint8_t refused(const char *what, const char *why)
{
    valve_event("NET", "Modbus %s: %s refused, %s", peer, what, why ? why : "refused");
    return MODBUS_EX_REFUSED;
}

uint8_t mbmap_write_regs(uint16_t addr, uint16_t n, const uint16_t *v)
{
    // Only the two writable floats, each written whole.
    for (uint16_t i = 0; i < n; i += 2) {
        uint16_t a = (uint16_t)(addr + i);
        if ((a != HR_TARGET && a != HR_VALVE_TARGET) || i + 1 >= n)
            return MODBUS_EX_ADDRESS;
    }
    for (uint16_t i = 0; i < n; i += 2) {
        uint16_t a = (uint16_t)(addr + i);
        float f = get_f32(v + i);
        const char *why = NULL;
        char t[16];
        if (!isfinite(f))
            return MODBUS_EX_VALUE;
        if (a == HR_TARGET) {
            if (f < 0.0f || f > valve_range_mmbar() / 1000.0f)
                return MODBUS_EX_VALUE;
            int32_t mm = (int32_t)lroundf(f * 1000.0f);
            fixed_fmt(t, sizeof t, fixed_round(mm, 2), 1);
            if (!valve_set_target(mm, &why))
                return refused("target", why);
            valve_event("NET", "Modbus %s: target %s mbar", peer, t);
        } else {
            if (f < 0.0f || f > 100.0f)
                return MODBUS_EX_VALUE;
            int32_t p = (int32_t)lroundf(f * 100.0f);
            fixed_fmt(t, sizeof t, p, 2);
            if (!valve_goto_pct100(p, &why))
                return refused("valve %", why);
            valve_event("NET", "Modbus %s: valve to %s %%", peer, t);
        }
    }
    return 0;
}

uint8_t mbmap_write_coil(uint16_t addr, bool on)
{
    const char *why = NULL;
    char left[40];

    switch (addr) {
    case CO_AUTO:
        if (on == valve_auto())
            return 0;
        if (on ? !valve_start_auto(&why) : !valve_to_manual(&why))
            return refused(on ? "AUTO" : "MANUAL", why);
        valve_event("NET", "Modbus %s: %s", peer, on ? "AUTO" : "MANUAL");
        return 0;
    case CO_STOP:
        if (on) {
            valve_event("NET", "Modbus %s: STOP", peer);
            valve_stop();
        }
        return 0;
    case CO_RESET:
        if (on) {
            valve_reset(left, sizeof left);
            valve_event("NET", "Modbus %s: RESET%s%s", peer, left[0] ? ", kept " : "", left);
        }
        return 0;
    case CO_ACK:
        if (on && valve_warning()) {
            valve_warning_clear();
            valve_event("NET", "Modbus %s: warning OK", peer);
        }
        return 0;
    default:
        return MODBUS_EX_ADDRESS;   // status coils are read only
    }
}
