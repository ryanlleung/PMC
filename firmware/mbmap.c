#include <math.h>
#include <string.h>

#include "ipc_config.h"
#include "cal.h"      // cal_capture_cancel: STOP ends a capture
#include "clicks.h"
#include "fixed.h"
#include "lvgl.h"   // lv_tick_get, lv_strlcpy
#include "cfg.h"
#include "modbus.h"
#include "sense.h"
#include "stepper3.h"
#include "valve.h"
#include "ui.h"   // ui_uptime_s

/* --------------------------------------------------------------------------
 * Modbus register map (MAP_VERSION). docs/modbus-register-map.md is the
 * reference; keep the two in step. Floats are IEEE 754 single in two
 * registers, high word first (ABCD). NaN means no value (no reading, no
 * target set).
 *
 * Writes go through the same calls as the screen buttons, so every check
 * and refusal is the same; they are refused while Network control is Off.
 * Each accepted write is logged as a NET event with the client's address.
 * ------------------------------------------------------------------------ */

#define MAP_VERSION 4

enum {
    HR_VERSION = 0, HR_HEARTBEAT, HR_MODE, HR_STATUS, HR_REGULATION,
    HR_FAULTS, HR_WARNINGS, HR_FAULT_COUNT, HR_EVENTS, HR_RESERVED,
    HR_PRESSURE = 10, HR_TARGET = 12, HR_VALVE = 14, HR_VALVE_TARGET = 16,
    HR_RANGE = 18, HR_EXCITATION = 20, HR_TEMP = 22, HR_UPTIME = 24,
    // The PID table: the zone in use (read only), then the settings,
    // written as the PID page's Save (tuning only, works in AUTO, one
    // settings save per write). Register 27 is reserved.
    HR_PID_SET = 26, HR_RESERVED_27 = 27, HR_B1 = 28, HR_B2 = 30, HR_HYST = 32,
    HR_GAINS = 33,   // KP1, TI1, KD1, KP2, TI2, KD2, KP3, TI3, KD3, KP4, TI4, KD4: floats
    HR_B3 = HR_GAINS + 24,
    HR_COUNT = HR_B3 + 2
};

// The settings behind the writable PID table registers: the key, 0.01 or
// 0.1 per unit (the cfg scale), and whether it is a float. Register 27 is
// reserved (key CFG_N): it reads 2 and takes only 2, so a client that
// writes the block back as it read it is not refused.
#define RESERVED_27_VALUE 2
static bool table_reg(uint16_t a, cfg_key_t *key, int *dec, bool *flt)
{
    if (a == HR_RESERVED_27) { *key = CFG_N; *dec = 0; *flt = false; return true; }
    if (a == HR_HYST)  { *key = CFG_HYST; *dec = 0; *flt = false; return true; }
    if (a == HR_B1)    { *key = CFG_B1; *dec = 1; *flt = true; return true; }
    if (a == HR_B2)    { *key = CFG_B2; *dec = 1; *flt = true; return true; }
    if (a == HR_B3)    { *key = CFG_B3; *dec = 1; *flt = true; return true; }
    if (a >= HR_GAINS && a < HR_B3 && (a - HR_GAINS) % 2 == 0) {
        int g = (a - HR_GAINS) / 2;
        *key = cfg_pid_key(g / 3 + 1, g % 3);
        *dec = 2;
        *flt = true;
        return true;
    }
    return false;
}

// Command coils (read and write) and status coils (read only).
enum { CO_AUTO = 0, CO_STOP, CO_RESET, CO_ACK, CO_STATUS = 16, CO_COUNT = CO_STATUS + 10 };

// Bits of HR_STATUS, and coils CO_STATUS + bit.
enum {
    ST_READING_OK, ST_POSITION_SET, ST_HOMED, ST_MOVING, ST_SWITCH,
    ST_FAULT, ST_WARNING, ST_TARGET_SET, ST_WRITES, ST_RESERVED_9
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

// Writes need the Network control switch On.
bool mbmap_writes_allowed(void)
{
    return cfg_remote();
}

void mbmap_begin(void)
{
    const clicks_state_t *s = clicks_state();
    int32_t sp;
    bool sp_set = valve_target(&sp);
    const char *w = valve_warning();
    uint32_t up_s = ui_uptime_s();   // past the 49.7 day tick wrap

    uint16_t st = 0;
    if (s->reading_ok)                         st |= 1u << ST_READING_OK;
    if (valve_calibrated())                    st |= 1u << ST_POSITION_SET;
    // Homed, also kept through a Shut down (PARKED is homed only).
    if (valve_reference() == VALVE_REF_HOMED || valve_reference() == VALVE_REF_PARKED)
        st |= 1u << ST_HOMED;
    if (stepper3_busy())                       st |= 1u << ST_MOVING;
    if (stepper3_limit_open())                 st |= 1u << ST_SWITCH;
    if (valve_fault_count() > 0)               st |= 1u << ST_FAULT;   // F-1 too
    if (w || valve_question(NULL))             st |= 1u << ST_WARNING;
    if (sp_set)                                st |= 1u << ST_TARGET_SET;
    if (mbmap_writes_allowed())                st |= 1u << ST_WRITES;
    st |= 1u << ST_RESERVED_9;   // reserved: always 1

    // Bit 0 F-1, bit 1 F-2 limit not found, bit 2 F-3 limit pressed; bits
    // 3 to 5 always 0.
    static const char *const ftag[] = { "F-1", "F-2", "F-3" };
    uint16_t f = 0;
    for (int i = 0; i < 3; i++)
        if (valve_fault_is(ftag[i]))
            f |= (uint16_t)(1u << i);

    // Bit 0 W-1, the switch question; bit 1 W-2, no pressure response (the
    // strip warning, by its number). Bits 2 to 5 always 0.
    uint16_t wb = 0;
    if (valve_question(NULL))
        wb |= 1u << 0;
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
    reg[HR_PID_SET] = (uint16_t)valve_pid_set();
    for (uint16_t a = HR_RESERVED_27; a < HR_COUNT; a++) {
        cfg_key_t key;
        int dec;
        bool flt;
        if (!table_reg(a, &key, &dec, &flt))
            continue;
        if (flt)
            put_f32(a, (float)cfg_tune(key) / (dec == 2 ? 100.0f : 10.0f));
        else
            reg[a] = key == CFG_N ? RESERVED_27_VALUE : (uint16_t)cfg_tune(key);
    }
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
        case CO_RESET: *v = valve_fault_count() > 0; break;
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

// A master that writes every scan would fill the event log with the same
// refusal: the same one is logged again only after 60 s.
static uint8_t refused(const char *what, const char *why)
{
    static const char *last_what, *last_why;
    static uint32_t last_ms;
    if (what != last_what || why != last_why || lv_tick_get() - last_ms >= 60000) {
        valve_event("NET", "Modbus %s: %s refused, %s", peer, what, why ? why : "refused");
        last_what = what;
        last_why = why;
        last_ms = lv_tick_get();
    }
    return MODBUS_EX_REFUSED;
}

// A write to the PID table registers (27 to 58): every register in it
// writable, floats whole; values checked as the screen does, then one
// tuning save (nothing written when nothing changes).
static uint8_t write_table(uint16_t addr, uint16_t n, const uint16_t *v)
{
    int32_t val[CFG_N] = { 0 };
    bool on[CFG_N] = { false }, same = true;
    for (uint16_t i = 0; i < n;) {
        cfg_key_t key;
        int dec;
        bool flt;
        if (!table_reg((uint16_t)(addr + i), &key, &dec, &flt) || (flt && i + 1 >= n))
            return MODBUS_EX_ADDRESS;
        if (key == CFG_N) {   // reserved: 2 only
            if (v[i] != RESERVED_27_VALUE)
                return MODBUS_EX_VALUE;
            i++;
            continue;
        }
        int32_t x, lo, hi;
        if (flt) {
            float f = get_f32(v + i);
            if (!isfinite(f) || fabsf(f) > 1.0e6f)
                return MODBUS_EX_VALUE;
            x = (int32_t)lroundf(f * (dec == 2 ? 100.0f : 10.0f));
        } else {
            x = v[i];
        }
        cfg_range(key, &lo, &hi);
        if (x < lo || x > hi)
            return MODBUS_EX_VALUE;
        val[key] = x;
        on[key] = true;
        same = same && x == cfg_tune(key);
        i = (uint16_t)(i + (flt ? 2 : 1));
    }
    if (same)
        return 0;
    const char *why;
    if (!cfg_save_with(val, on, &why))
        return refused("PID table", why);
    valve_event("NET", "Modbus %s: PID table saved", peer);
    return 0;
}

uint8_t mbmap_write_regs(uint16_t addr, uint16_t n, const uint16_t *v)
{
    if (addr >= HR_RESERVED_27)
        return write_table(addr, n, v);
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
            int32_t mm = (int32_t)lroundf(f * 1000.0f), now;
            // The same target again (a master writing every scan) changes
            // nothing: no event, and the W-2 and Stable timers run on.
            if (valve_target(&now) && now == (int32_t)fixed_round(mm, 2) * 100)
                continue;
            fixed_fmt(t, sizeof t, fixed_round(mm, 2), 1);
            if (!valve_set_target(mm, &why))
                return refused("setpoint", why);
            valve_event("NET", "Modbus %s: setpoint %s mbar", peer, t);
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
        valve_event("NET", "Modbus %s: %s", peer, on ? "AUTO started" : "switched to MANUAL");
        return 0;
    case CO_STOP:
        if (on) {
            valve_event("NET", "Modbus %s: STOP", peer);
            // A sensor capture ends too, as the screen's STOP and the text
            // port's.
            (void)cal_capture_cancel();
            valve_stop();
        }
        return 0;
    case CO_RESET:
        if (on) {
            valve_reset(left, sizeof left);
            valve_event("NET", "Modbus %s: fault reset%s%s", peer, left[0] ? ", kept " : "", left);
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
