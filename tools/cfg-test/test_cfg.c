/*
 * Host test of the settings store (cfg.c, cal.c) and the saved valve
 * position (valvepos.c) against a serial flash held in RAM. Run with run.sh.
 *
 * Covers: a version 1 record (0.6.0 to 0.8.x) loading as one sensor; a save
 * from the screens (cfg_save_with) going to the other sector as version 2;
 * a restart; Load settings > Previous save; a combination refused as a
 * whole; sensors added, switched and removed with their own cals; a motor
 * direction change clearing the calibration; a spoiled record; and the
 * saved valve position written at standstill, withdrawn by a move, and
 * found again after the log wraps to the next sector.
 */
#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "cal.h"
#include "cfg.h"
#include "clicks.h"
#include "extflash.h"
#include "stepper3.h"
#include "valve.h"
#include "valvepos.h"
#include "trend.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* Serial flash: 8 MB, erased to 0xFF; programming only clears bits. */
#define FLASH_SIZE (8u << 20)
static uint8_t *flash;
static int erases;
bool extflash_init(void) { return true; }
uint32_t extflash_jedec_id(void) { return 0xBF2643; }
void extflash_read(uint32_t a, void *b, size_t n) { memcpy(b, flash + a, n); }
bool extflash_erase_4k(uint32_t a) { memset(flash + (a & ~0xFFFu), 0xFF, 4096); erases++; return true; }
static int write_fails;   // the next n writes do nothing
bool extflash_write(uint32_t a, const void *b, size_t n)
{
    const uint8_t *p = b;
    if (write_fails > 0) { write_fails--; return true; }
    for (size_t i = 0; i < n; i++)
        flash[a + i] &= p[i];
    return true;
}

/* Link, clock */
static int quiet = 1;
void link_printf(const char *fmt, ...)
{
    va_list a;
    if (quiet) return;
    va_start(a, fmt);
    vprintf(fmt, a);
    va_end(a);
}
const char *rtclock_iso(void) { return "2026-10-16 08:15:00"; }

/* Druck reading */
static clicks_state_t cs = { .pm_found = true, .reading_ok = true, .bus_mv = 10000 };
const clicks_state_t *clicks_state(void) { return &cs; }

/* Stepper 3 */
static uint32_t m_rate = 400, m_start = 200, m_accel = 3200;
static stepper3_mode_t m_mode = STEPPER3_FULL;
static bool m_hold, m_rev, m_busy;
static int32_t m_pos;
static uint8_t m_order[4] = { 0, 2, 1, 3 };
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
void stepper3_order(uint8_t o[4]) { memcpy(o, m_order, 4); }
bool stepper3_busy(void) { return m_busy; }
int32_t stepper3_position(void) { return m_pos; }
static bool (*move_hook)(const char **why);
void stepper3_on_move_start(bool (*cb)(const char **why)) { move_hook = cb; }
// A move starting from rest: stepper3.c calls the hook first and refuses
// the move if it says no.
static bool start_move(int32_t to)
{
    const char *why = NULL;
    if (move_hook && !move_hook(&why))
        return false;
    m_busy = true;
    m_pos = to;
    return true;
}
const char *stepper3_mode_name(stepper3_mode_t m) { return m == STEPPER3_WAVE ? "WAVE" : m == STEPPER3_FULL ? "FULL" : "HALF"; }

/* Valve */
static int32_t v_set[VALVE_N_SETTINGS] = { 99072, 400, 25, 400, 800, 0 };
static const char *const v_name[VALVE_N_SETTINGS] = { "POSMAX", "FHOME", "FCREEP", "BACKOFF", "CLEAR", "OFFSET" };
static const int32_t v_lo[VALVE_N_SETTINGS] = { 1000, 16, 16, 10, 10, 0 }, v_hi[VALVE_N_SETTINGS] = { 300000, 1000, 1000, 5000, 10000, 5000 };
static valve_mode_t v_mode = VALVE_NOT_CAL;
static bool v_cal;
static int uncal_calls;
const char *valve_setting_name(int i) { return v_name[i]; }
void valve_settings(int32_t v[VALVE_N_SETTINGS]) { memcpy(v, v_set, sizeof v_set); }
const char *valve_settings_check(const int32_t v[VALVE_N_SETTINGS])
{
    for (int i = 0; i < VALVE_N_SETTINGS; i++)
        if (v[i] < v_lo[i] || v[i] > v_hi[i])
            return v_name[i];
    return NULL;
}
bool valve_set_settings(const int32_t v[VALVE_N_SETTINGS]) { memcpy(v_set, v, sizeof v_set); return true; }
valve_mode_t valve_mode(void) { return v_mode; }
static bool v_auto;            // test_auto(): AUTO on
static int32_t v_sp = 5000;
bool valve_auto(void) { return v_auto; }
bool valve_target(int32_t *mmbar) { *mmbar = v_sp; return true; }
int32_t valve_pct_milli(void) { return 34240; }
size_t link_tx_room(void) { return 4096; }
bool valve_calibrated(void) { return v_cal; }
int32_t valve_pos_max(void) { return v_set[0]; }
void valve_uncalibrate(const char *why) { (void)why; uncal_calls++; v_cal = false; v_mode = VALVE_NOT_CAL; }
bool valve_restore(int32_t count, const char **why)
{
    if (v_cal) { *why = "already calibrated"; return false; }
    m_pos = count; v_cal = true; v_mode = VALVE_MANUAL;
    return true;
}
void valve_event(const char *tag, const char *fmt, ...)
{
    va_list a;
    if (quiet) return;
    printf("EVT %s ", tag);
    va_start(a, fmt);
    vprintf(fmt, a);
    va_end(a);
    printf("\n");
}

/* Records ----------------------------------------------------------------- */
static uint32_t crc32(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t c = 0xFFFFFFFFUL;
    while (len--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320UL & (0UL - (c & 1)));
    }
    return ~c;
}

// The 0.6.0 to 0.8.x layout, as cfg.c reads it.
typedef struct {
    uint32_t magic, version, size, rev;
    int32_t mot[7];
    int32_t valve[VALVE_N_SETTINGS];
    uint32_t has_cal;
    cal_rec_t cal;
    uint32_t crc;
} rec_v1_t;

static cal_rec_t bench_cal(const char *id, int32_t r_vac, int32_t r_air, int32_t p_air)
{
    cal_rec_t c;
    memset(&c, 0, sizeof c);
    c.magic = CAL_MAGIC;
    c.version = CAL_VERSION;
    c.npts = 2;
    strncpy(c.id, id, CAL_ID_LEN - 1);
    c.r_ppb[0] = r_vac; c.p_mmbar[0] = 0;
    c.r_ppb[1] = r_air; c.p_mmbar[1] = p_air;
    c.kind = CAL_KIND_BENCH;
    c.vac_unc_mmbar = 10;
    c.air_unc_mmbar = 1000;
    c.crc = crc32(&c, offsetof(cal_rec_t, crc));
    return c;
}

static void write_v1(uint32_t addr, uint32_t rev)
{
    rec_v1_t r;
    memset(&r, 0, sizeof r);
    r.magic = 0x47464350UL;
    r.version = 1;
    r.size = sizeof r;
    r.rev = rev;
    int32_t mot[7] = { 350, 180, 3000, STEPPER3_FULL, 0, 0, 213 };
    memcpy(r.mot, mot, sizeof mot);
    int32_t valve[VALVE_N_SETTINGS] = { 99072, 400, 25, 400, 800, 0 };
    memcpy(r.valve, valve, sizeof valve);
    r.has_cal = 1;
    r.cal = bench_cal("5880156 bench", 48000, 9300000, 1013200);
    r.crc = crc32(&r, offsetof(rec_v1_t, crc));
    extflash_erase_4k(addr);
    extflash_write(addr, &r, sizeof r);
}

static void reboot(void)
{
    // What main.c does at start-up, with every live value back to factory.
    m_rate = 400; m_start = 200; m_accel = 3200; m_rev = false;
    int32_t valve[VALVE_N_SETTINGS] = { 99072, 400, 25, 400, 800, 0 };
    memcpy(v_set, valve, sizeof valve);
    v_cal = false; v_mode = VALVE_NOT_CAL;
    cal_init();
    cfg_init();
}

static int32_t val(cfg_from_t f, cfg_key_t k)
{
    int32_t v = -999999;
    cfg_value(f, k, &v);
    return v;
}

static void test_cfg(void)
{
    int32_t v[CFG_N] = { 0 };
    bool set[CFG_N] = { 0 };
    const char *why;
    uint32_t rev;
    const char *at;
    cfg_sensor_t s;

    // A 0.8.x board: one version 1 record in A.
    write_v1(0x7FD000, 5);
    reboot();
    CHECK(cfg_rev(CFG_FROM_SAVED, &rev, &at) && rev == 5);
    CHECK(!cfg_rev(CFG_FROM_PREVIOUS, &rev, &at));
    CHECK(m_rate == 350 && m_start == 180);
    CHECK(cfg_sensor_count() == 1 && cfg_sensor(0, &s) && s.serial == 5880156 && s.psi10 == 150 && s.in_use);
    CHECK(s.has_cal && s.saved && !cal_is_default());
    CHECK(val(CFG_FROM_LIVE, CFG_KP) == 400 && val(CFG_FROM_LIVE, CFG_BAND) == 10);
    CHECK(cfg_unsaved() == 0);

    // Save from the screens: gain 2.50, run speed 500.
    v[CFG_KP] = 250; set[CFG_KP] = true;
    v[CFG_RATE] = 500; set[CFG_RATE] = true;
    CHECK(cfg_save_with(v, set, &why));
    CHECK(cfg_rev(CFG_FROM_SAVED, &rev, &at) && rev == 6 && strcmp(at, "2026-10-16 08:15:00") == 0);
    CHECK(cfg_rev(CFG_FROM_PREVIOUS, &rev, &at) && rev == 5);
    uint32_t ver;
    memcpy(&ver, flash + 0x7FE004, 4);
    CHECK(ver == 3);   // the save went to B, the v1 record in A is kept
    memcpy(&ver, flash + 0x7FD004, 4);
    CHECK(ver == 1);
    CHECK(m_rate == 500 && cfg_tune(CFG_KP) == 250 && cfg_unsaved() == 0);

    // Restart.
    reboot();
    CHECK(m_rate == 500 && cfg_tune(CFG_KP) == 250);
    CHECK(cfg_rev(CFG_FROM_SAVED, &rev, &at) && rev == 6);
    CHECK(!cal_is_default());

    // Load settings > Previous save: in use, unsaved.
    CHECK(cfg_load_live(CFG_FROM_PREVIOUS, &why));
    CHECK(m_rate == 350 && cfg_tune(CFG_KP) == 400 && cfg_unsaved() == 2);
    CHECK(cfg_load_live(CFG_FROM_SAVED, &why));
    CHECK(cfg_unsaved() == 0);

    // A combination refused as a whole: start above run.
    memset(set, 0, sizeof set);
    v[CFG_START] = 900; set[CFG_START] = true;
    v[CFG_KP] = 300; set[CFG_KP] = true;
    CHECK(!cfg_save_with(v, set, &why) && strstr(why, "Start"));
    CHECK(m_start == 180 && cfg_tune(CFG_KP) == 250);
    CHECK(cfg_rev(CFG_FROM_SAVED, &rev, &at) && rev == 6);
    // Out of range.
    memset(set, 0, sizeof set);
    v[CFG_OUTMIN] = 501; set[CFG_OUTMIN] = true;
    CHECK(!cfg_save_with(v, set, &why));
    // Refused while the motor runs.
    memset(set, 0, sizeof set);
    v[CFG_KP] = 300; set[CFG_KP] = true;
    m_busy = true;
    CHECK(!cfg_save_with(v, set, &why));
    m_busy = false;
    // A failed flash write: nothing saved and nothing put in use (written
    // before it is applied, review of 0.10.11).
    memset(set, 0, sizeof set);
    v[CFG_RATE] = 450; set[CFG_RATE] = true;
    v[CFG_KP] = 300; set[CFG_KP] = true;
    write_fails = 1;
    CHECK(!cfg_save_with(v, set, &why) && strstr(why, "flash"));
    write_fails = 0;
    CHECK(m_rate == 500 && cfg_tune(CFG_KP) == 250 && cfg_unsaved() == 0);
    CHECK(cfg_rev(CFG_FROM_SAVED, &rev, &at) && rev == 6);

    // A second sensor: added (in use, unsaved), then selected and saved.
    CHECK(cfg_sensor_add(1234567, 300, &why));
    CHECK(!cfg_sensor_add(1234567, 300, &why));
    CHECK(cfg_sensor_count() == 2 && cfg_unsaved() == 1);
    memset(set, 0, sizeof set);
    v[CFG_SENSOR] = 1234567; set[CFG_SENSOR] = true;
    CHECK(cfg_save_with(v, set, &why));
    CHECK(cfg_sensor(cfg_sensor_active(), &s) && s.serial == 1234567 && !s.has_cal && cal_is_default());
    // Nominal over 30 psi: 10 mV/V is 2068.4 mbar.
    CHECK(cal_pressure_mmbar(10000000) == cfg_psi10_mmbar(300));
    CHECK(!cfg_sensor_remove(1234567, &why));   // in use
    // Back to the first: its own cal comes back.
    v[CFG_SENSOR] = 5880156;
    CHECK(cfg_save_with(v, set, &why));
    CHECK(!cal_is_default() && cal_pressure_mmbar(9300000) == 1013200);
    CHECK(cfg_sensor_remove(1234567, &why) && cfg_sensor_count() == 1 && cfg_unsaved() == 1);
    // Removed, then the previous save (which had it) put back.
    CHECK(cfg_load_live(CFG_FROM_SAVED, &why) && cfg_sensor_count() == 2);
    CHECK(cfg_sensor_remove(1234567, &why));
    CHECK(cfg_save());

    // Factory defaults keep the sensors.
    CHECK(cfg_load_live(CFG_FROM_DEFAULTS, &why));
    CHECK(m_rate == 400 && cfg_tune(CFG_KP) == 400 && cfg_sensor_count() == 1 && !cal_is_default());
    CHECK(cfg_load_live(CFG_FROM_SAVED, &why));

    // Motor direction: saving it clears the calibration.
    v_cal = true; v_mode = VALVE_MANUAL;
    memset(set, 0, sizeof set);
    v[CFG_REVERSE] = 1; set[CFG_REVERSE] = true;
    CHECK(cfg_save_with(v, set, &why));
    CHECK(m_rev && uncal_calls == 1 && !v_cal);

    // Spoil the newest: the other one loads at the next start-up.
    CHECK(cfg_rev(CFG_FROM_SAVED, &rev, &at));
    uint32_t newest = rev;
    cfg_command("CFG TEST CORRUPT");
    reboot();
    CHECK(cfg_rev(CFG_FROM_SAVED, &rev, &at) && rev == newest - 1);
    CHECK(!m_rev);
}

// Calibrate sensor on the screen: vacuum, then air, then Save settings.
static void test_capture(void)
{
    const char *why;
    cal_point_t vac, air;
    int32_t v[CFG_N] = { 0 };
    bool set[CFG_N] = { 0 };
    cs.r_ppb = 9300000;
    CHECK(!cal_capture(1, 0, 0, &why));            // not a vacuum
    cs.r_ppb = 51000;
    CHECK(cal_capture(1, 0, 0, &why) && cal_capturing() == 1);
    CHECK(!cal_capture(2, 1013000, 0, &why));      // one at a time
    for (int i = 0; i < 10; i++)
        cal_update(&cs);
    CHECK(cal_capturing() == 0);
    cal_points(&vac, &air);
    CHECK(vac.have && vac.r_ppb == 51000 && air.have);   // air kept from the table
    CHECK(!cal_saved() && cfg_unsaved() == 1);
    cs.r_ppb = 9310000;
    CHECK(cal_capture(2, 1012000, 0, &why));
    for (int i = 0; i < 10; i++)
        cal_update(&cs);
    CHECK(cal_pressure_mmbar(9310000) == 1012000 && cal_pressure_mmbar(51000) == 0);
    CHECK(strncmp(cal_id(), "5880156 bench 26-10-16", 22) == 0);
    const cal_rec_t *sv;
    CHECK(cfg_saved_cal(5880156, &sv) && sv && cal_pressure_with(sv, 9300000) == 1013200);
    CHECK(cfg_save_with(v, set, &why) && cal_saved() && cfg_unsaved() == 0);
}


// Cryostat name (version 3) and a version 2 record loading.
static void test_name(void)
{
    int32_t v[CFG_N] = { 0 };
    bool set[CFG_N] = { 0 };
    const char *why;
    uint32_t rev;
    const char *at;
    reboot();
    uint32_t r0 = 0;
    CHECK(cfg_rev(CFG_FROM_SAVED, &r0, &at));


    // A new name is live
    // and unsaved, too long is refused, and it survives a save and restart.
        CHECK(!cfg_set_name("ABCDEFGHIJKLM", &why) && strcmp(cfg_name(), "CF-11") == 0);
    CHECK(cfg_set_name("HM-2", &why) && cfg_unsaved() == 1);
    CHECK(cfg_save_with(v, set, &why) && cfg_unsaved() == 0);
    reboot();
    CHECK(strcmp(cfg_name(), "HM-2") == 0);
    // The same record rewritten as version 2 (0.9.x): loads, name "CF-11".
    {
        uint32_t size3, a3 = 0;
        int sl = 0;
        for (int i = 0; i < 2; i++) {
            uint32_t r3;
            memcpy(&r3, flash + 0x7FD000 + i * 0x1000 + 12, 4);
            uint32_t v3;
            memcpy(&v3, flash + 0x7FD000 + i * 0x1000 + 4, 4);
            if (v3 == 3 && r3 >= a3) { a3 = r3; sl = i; }
        }
        uint32_t base = 0x7FD000 + sl * 0x1000;
        memcpy(&size3, flash + base + 8, 4);
        uint32_t size2 = size3 - (CFG_NAME_LEN + 4);
        static uint8_t buf[4096];
        memcpy(buf, flash + base, size3);
        uint32_t two = 2;
        memcpy(buf + 4, &two, 4);
        memcpy(buf + 8, &size2, 4);
        uint32_t c = crc32(buf, size2 - 4);
        memcpy(buf + size2 - 4, &c, 4);
        extflash_erase_4k(base);
        extflash_write(base, buf, size2);
        reboot();
        CHECK(cfg_rev(CFG_FROM_SAVED, &rev, &at) && rev == a3);
        CHECK(strcmp(cfg_name(), "CF-11") == 0 );
    }
}

static void tick(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 10) {
        lv_tick_inc(10);
        valvepos_task();
    }
}

static void test_valvepos(void)
{
    int32_t c;
    const char *why;
    v_cal = false; v_mode = VALVE_NOT_CAL; m_busy = false; m_pos = 0;
    valvepos_init();
    CHECK(!valvepos_offer(&c));

    // Calibrated and still: written after 2 s.
    v_cal = true; v_mode = VALVE_MANUAL; m_pos = 33880;
    tick(2500);
    // Restart: offered, not used.
    v_cal = false; v_mode = VALVE_NOT_CAL; m_pos = 0;
    valvepos_init();
    CHECK(valvepos_offer(&c) && c == 33880);
    tick(5000);
    CHECK(valvepos_offer(&c) && !v_cal);   // still waiting for the operator
    CHECK(valvepos_use(&why) && v_cal && m_pos == 33880 && !valvepos_offer(&c));

    // A move: the entry is withdrawn before the motor turns, so a reset
    // straight after the start (no valvepos_task yet) offers nothing.
    start_move(34000);
    v_cal = false; v_mode = VALVE_NOT_CAL; m_busy = false;
    valvepos_init();
    CHECK(!valvepos_offer(&c));   // as if the power went mid-move

    // Still for less than 2 s: nothing saved, whatever the tick's parity.
    v_cal = true; v_mode = VALVE_MANUAL; m_pos = 20000;
    lv_tick_inc(1);               // odd/even tick no longer matters
    tick(1500);
    v_cal = false; v_mode = VALVE_NOT_CAL; m_pos = 0;
    valvepos_init();
    CHECK(!valvepos_offer(&c));
    v_cal = true; v_mode = VALVE_MANUAL; m_pos = 20000;
    tick(2500);
    v_cal = false; v_mode = VALVE_NOT_CAL; m_pos = 0;
    valvepos_init();
    CHECK(valvepos_offer(&c) && c == 20000);
    valvepos_decline();

    // The withdrawal write fails once: the second try takes it, in the
    // next slot, after a blank one. valvepos_init() clears the module's RAM
    // as a reset does, so only the flash tells it what happened.
    v_cal = true; v_mode = VALVE_MANUAL; m_pos = 20000;
    write_fails = 1;
    CHECK(start_move(21000));
    m_busy = false;
    v_cal = false; v_mode = VALVE_NOT_CAL;
    valvepos_init();
    CHECK(!valvepos_offer(&c));

    // The same with a save first, as the review's case: 33880 saved, first
    // withdrawal fails, retry works, move to 34000, cold restart.
    v_cal = true; v_mode = VALVE_MANUAL; m_pos = 33880;
    tick(2500);
    write_fails = 1;
    CHECK(start_move(34000));
    m_busy = false;
    v_cal = false; v_mode = VALVE_NOT_CAL; m_pos = 0;
    valvepos_init();
    CHECK(!valvepos_offer(&c));

    // Both tries fail: the move is refused, so the saved count still
    // matches the valve and is still offered after a restart.
    v_cal = true; v_mode = VALVE_MANUAL; m_pos = 30000;
    tick(2500);
    write_fails = 2;
    CHECK(!start_move(31000) && m_pos == 30000 && !m_busy);
    v_cal = false; v_mode = VALVE_NOT_CAL; m_pos = 0;
    valvepos_init();
    CHECK(valvepos_offer(&c) && c == 30000);
    valvepos_decline();
    v_cal = true; v_mode = VALVE_MANUAL; m_pos = 30000;
    CHECK(start_move(31000));   // flash good again: the move goes
    m_busy = false;

    // Many moves: the log wraps through the sectors; the newest still wins.
    v_cal = true; v_mode = VALVE_MANUAL;
    int e0 = erases;
    for (int i = 0; i < 700; i++) {
        CHECK(start_move(1000 + i));
        tick(20);
        m_busy = false;
        tick(2100);
    }
    CHECK(erases - e0 >= 5);
    v_cal = false; v_mode = VALVE_NOT_CAL; m_pos = 0;
    valvepos_init();
    CHECK(valvepos_offer(&c) && c == 1699);

    // Declined: Calibrate instead. The next move withdraws the entry.
    valvepos_decline();
    CHECK(!valvepos_offer(&c));
    v_mode = VALVE_CALIBRATING; m_busy = true;
    tick(100);
    valvepos_init();
    CHECK(!valvepos_offer(&c));
}

// Trend slots are fixed one-second boundaries: a late poll fills its own
// slot (review: 1000, 2600, 3000 ms gave four samples), a stall leaves
// no-data slots.
static void test_trend(void)
{
    clicks_state_t q = { .pm_found = true, .reading_ok = true, .p_mmbar = 5000 };
    uint32_t n0 = trend_seconds();
    trend_second(&q);                    // t
    lv_tick_inc(1600); trend_second(&q); // t + 1.6 s: slot 1, late
    lv_tick_inc(400);  trend_second(&q); // t + 2.0 s: slot 2
    CHECK(trend_seconds() - n0 == 3);
    lv_tick_inc(500);  trend_second(&q); // t + 2.5 s: slot 2 again, dropped
    CHECK(trend_seconds() - n0 == 3);
    lv_tick_inc(3700); trend_second(&q); // t + 6.2 s: slots 3 to 5 empty, 6
    CHECK(trend_seconds() - n0 == 7);
    trend_pt_t o;
    CHECK(trend_fine(0, &o) && o.p == 5000);
    CHECK(trend_fine(1, &o) && o.p == TREND_NONE);
    CHECK(trend_fine(4, &o) && o.p == 5000);
}

// Stage 8: in AUTO only the tuning changes (screens and PID); the trend
// carries the target.
static void test_auto(void)
{
    int32_t v[CFG_N] = { 0 };
    bool set[CFG_N] = { false };
    const char *why;
    uint32_t rev0, rev;
    const char *at;
    cfg_rev(CFG_FROM_SAVED, &rev0, &at);
    uint32_t rate = m_rate;
    v_auto = true;
    v[CFG_KP] = 300; set[CFG_KP] = true;
    v[CFG_RATE] = (int32_t)rate; set[CFG_RATE] = true;          // unchanged: allowed
    CHECK(cfg_save_with(v, set, &why));
    CHECK(cfg_tune(CFG_KP) == 300 && m_rate == rate && cfg_unsaved() == 0);
    CHECK(cfg_rev(CFG_FROM_SAVED, &rev, &at) && rev == rev0 + 1);
    v[CFG_RATE] = (int32_t)rate + 50;                             // changed: refused
    v[CFG_KP] = 320;
    CHECK(!cfg_save_with(v, set, &why) && strstr(why, "MANUAL"));
    CHECK(cfg_tune(CFG_KP) == 300 && m_rate == rate);
    CHECK(!cfg_load_live(CFG_FROM_DEFAULTS, &why) && strstr(why, "MANUAL"));
    CHECK(cfg_command("PID KP 3.5") && cfg_tune(CFG_KP) == 350 && cfg_unsaved() == 1);
    CHECK(cfg_command("PID TI 0.4") && cfg_tune(CFG_TI) == 40);
    CHECK(cfg_command("PID OUTMIN 0") && cfg_tune(CFG_OUTMIN) == 0);
    CHECK(cfg_command("PID BAND 0.05") && cfg_tune(CFG_BAND) == 10);   // below 0.10: refused, kept
    CHECK(cfg_command("PID KDIR 0") && cfg_tune(CFG_KDIR) == 1);
    CHECK(cfg_command("PID KDIR -1") && cfg_tune(CFG_KDIR) == -1);
    CHECK(cfg_command("PID KDIR +1") && cfg_tune(CFG_KDIR) == 1);
    CHECK(cfg_command("PID TS 2") && cfg_command("PID?"));
    CHECK(cfg_command("PID DB 8x") && cfg_tune(CFG_DB) == 8);
    clicks_state_t q = { .pm_found = true, .reading_ok = true, .p_mmbar = 5100 };
    trend_pt_t o;
    lv_tick_inc(1000); trend_second(&q);
    CHECK(trend_fine(0, &o) && o.sp == 5000);
    v_auto = false;
    lv_tick_inc(1000); trend_second(&q);
    CHECK(trend_fine(0, &o) && o.sp == TREND_NONE);
}

int main(int argc, char **argv)
{
    quiet = !(argc > 1 && strcmp(argv[1], "-v") == 0);
    flash = malloc(FLASH_SIZE);
    memset(flash, 0xFF, FLASH_SIZE);
    lv_init();
    test_cfg();
    test_capture();
    test_name();
    test_valvepos();
    test_trend();
    test_auto();
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
