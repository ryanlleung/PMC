/*
 * Host test of the settings store (cfg.c, cal.c) and the saved valve
 * position (valvepos.c) against a serial flash held in RAM. Run with run.sh.
 *
 * Covers: a blank flash; records that are not read (spoiled, out of range,
 * another size or version); a saved record loading; a save from the
 * screens (cfg_save_with) going to the other sector; a restart; Undo and
 * Reset to factory; the cryostat name, the IP address and Network control
 * saved as they are set; a combination refused as a whole; a motor
 * direction change clearing the calibration; a spoiled newest record; the
 * calibration capture; the PID table; and the saved valve position
 * written at standstill, withdrawn by a move, and found again after the log
 * wraps to the next sector.
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
#include "limit.h"
#include "sysinfo.h"
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

/* Clock; -v prints the events */
static int quiet = 1;
const char *rtclock_iso(void) { return "2026-10-16 08:15:00"; }

/* Druck reading */
static clicks_state_t cs = { .pm_found = true, .reading_ok = true, .bus_mv = 10000 };
const clicks_state_t *clicks_state(void) { return &cs; }

/* Stepper 3 */
static uint32_t m_rate = 400, m_start = 200, m_accel = 3200;
static bool m_hold, m_rev, m_busy;
static int32_t m_pos;
static uint8_t m_order[4] = { 0, 2, 1, 3 };
bool stepper3_set_rate(uint32_t v) { m_rate = v; return true; }
bool stepper3_set_start(uint32_t v) { m_start = v; return true; }
bool stepper3_set_accel(uint32_t v) { m_accel = v; return true; }
bool stepper3_set_hold(bool on) { m_hold = on; return true; }
// As stepper3.c: a change of direction clears the homing through the
// valve layer's stepper3_on_reference_change callback.
bool stepper3_set_reverse(bool on)
{
    if (on != m_rev)
        valve_uncalibrate("motor direction changed");
    m_rev = on;
    return true;
}
bool stepper3_set_order(const uint8_t o[4]) { memcpy(m_order, o, 4); return true; }
uint32_t stepper3_rate(void) { return m_rate; }
uint32_t stepper3_start_rate(void) { return m_start; }
uint32_t stepper3_accel(void) { return m_accel; }
bool stepper3_hold(void) { return m_hold; }
bool stepper3_reverse(void) { return m_rev; }
void stepper3_order(uint8_t o[4]) { memcpy(o, m_order, 4); }
bool stepper3_busy(void) { return m_busy; }
static int stops;
void stepper3_stop(void) { m_busy = false; stops++; }
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

/* Valve */
static int32_t v_set[VALVE_N_SETTINGS] = { 99072, 400, 25, 400, 800, 400 };
static const char *const v_name[VALVE_N_SETTINGS] = { "POSMAX", "FHOME", "FCREEP", "BACKOFF", "CLEAR", "RETURN" };
static const int32_t v_lo[VALVE_N_SETTINGS] = { 1000, 16, 16, 10, 10, 10 }, v_hi[VALVE_N_SETTINGS] = { 300000, 1000, 1000, 5000, 10000, 5000 };
static valve_mode_t v_mode = VALVE_NOT_CAL;
static bool v_cal;
static int uncal_calls;
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
bool valve_calibrated(void) { return v_cal; }
int32_t valve_pos_max(void) { return v_set[0]; }
void valve_uncalibrate(const char *why) { (void)why; uncal_calls++; v_cal = false; v_mode = VALVE_NOT_CAL; }
bool valve_restore(int32_t count, const char **why)
{
    if (v_cal) { *why = "already calibrated"; return false; }
    m_pos = count; v_cal = true; v_mode = VALVE_MANUAL;
    return true;
}
// Shut down (at 0 %, and every start OFF): the parked
// entry at start-up.
static const char *reset_why = "power-on";
const char *sysinfo_reset_reason(void) { return reset_why; }
static limit_state_t lim = LIMIT_CLOSED;
limit_state_t limit_state(void) { return lim; }
static valve_ref_t v_ref = VALVE_REF_HOMED;
valve_ref_t valve_reference(void) { return v_cal ? v_ref : VALVE_REF_NONE; }
static int parked_calls;
static bool parked_homed;
bool valve_restore_parked(int32_t count, bool homed, const char **why)
{
    if (v_cal) { *why = "already calibrated"; return false; }
    m_pos = count; v_cal = true;
    if (v_mode == VALVE_NOT_CAL) v_mode = VALVE_MANUAL;
    parked_homed = homed;
    parked_calls++;
    return true;
}
// The events since the last reboot(), one a line, for the checks.
static char evlog[4096];
void valve_event(const char *tag, const char *fmt, ...)
{
    va_list a;
    const size_t n = strlen(evlog);
    if (n > sizeof evlog - 300) return;
    va_start(a, fmt);
    vsnprintf(evlog + n, 256, fmt, a);
    va_end(a);
    if (!quiet) printf("EVT %s %s\n", tag, evlog + n);
    strcat(evlog, "\n");
}
static bool said(const char *what) { return strstr(evlog, what) != NULL; }

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

// The settings record (version 13), byte for byte as cfg.c writes it:
// declared here again, so a slip in cfg.c's copy shows.
typedef struct {
    uint32_t magic, version, size, rev;
    int32_t mot[8];              // RATE, START, ACCEL, HOLD, DIR, ORDER, steps per turn, Jog speed
    int32_t valve[VALVE_N_SETTINGS];   // POSMAX, FHOME, FCREEP, BACKOFF, CLEAR, RETURN
    int32_t tune[21];            // KP, TI, OUTMIN, DB, BAND, KD, zones 2 and 3, B1, B2, HYST, zone 4, B3, PASSLO, PASSHI
    uint32_t has_cal;
    cal_rec_t cal;
    char saved_at[20];
    char name[CFG_NAME_LEN + 4];
    uint32_t net_ip, net_mask;
    uint32_t remote;
    uint32_t crc;
} rec_t;

// A saved record, rev 5, with a two-point cal: vacuum at 0.048 mV/V, room
// air 1013.2 mbar at 9.3 mV/V.
static rec_t rec(void)
{
    rec_t r;
    memset(&r, 0, sizeof r);
    r.magic = 0x47464350UL;
    r.version = 13;
    r.size = sizeof r;
    r.rev = 5;
    const int32_t mot[8] = { 350, 180, 3000, 0, 0, 213, STEPPER3_STEPS_PER_TURN, 100 };
    const int32_t valve[VALVE_N_SETTINGS] = { 60000, 380, 30, 300, 800, 350 };
    const int32_t tune[21] = { 7, 61, 5, 9, 12, 3, 13, 83, 4, 81, 95, 5, 55, 95, 12, 41, 26, 6, 135, 4, 45 };
    memcpy(r.mot, mot, sizeof mot);
    memcpy(r.valve, valve, sizeof valve);
    memcpy(r.tune, tune, sizeof tune);
    r.has_cal = 1;
    r.cal.magic = CAL_MAGIC;
    r.cal.version = CAL_VERSION;
    r.cal.r_ppb[0] = 48000;   r.cal.p_mmbar[0] = 0;
    r.cal.r_ppb[1] = 9300000; r.cal.p_mmbar[1] = 1013200;
    r.cal.crc = crc32(&r.cal, offsetof(cal_rec_t, crc));
    strcpy(r.saved_at, "2026-10-24 14:04:08");
    strcpy(r.name, "CF-10");
    r.remote = 1;
    return r;
}

// Save with no drafts: the settings in use, as the screens' Save.
static bool save_all(void)
{
    int32_t v[CFG_N] = { 0 };
    bool set[CFG_N] = { false };
    const char *why;
    return cfg_save_with(v, set, &why);
}

static void put_rec(uint32_t addr, rec_t *r)
{
    r->crc = crc32(r, offsetof(rec_t, crc));
    extflash_erase_4k(addr);
    extflash_write(addr, r, sizeof *r);
}

static void wipe(void)
{
    extflash_erase_4k(0x7FD000);
    extflash_erase_4k(0x7FE000);
}

static void reboot(void)
{
    // What main.c does at start-up, with every live value back to factory.
    m_rate = 400; m_start = 200; m_accel = 3200; m_rev = false;
    int32_t valve[VALVE_N_SETTINGS] = { 99072, 400, 25, 400, 800, 400 };
    memcpy(v_set, valve, sizeof valve);
    v_cal = false; v_mode = VALVE_NOT_CAL;
    evlog[0] = '\0';
    cal_init();
    cfg_init();
}

// Spoils the newest settings record's CRC, so the other one loads.
static void corrupt_newest(void)
{
    uint32_t best = 0;
    int sl = -1;
    for (int i = 0; i < 2; i++) {
        uint32_t r, ver;
        memcpy(&r, flash + 0x7FD000 + i * 0x1000 + 12, 4);
        memcpy(&ver, flash + 0x7FD000 + i * 0x1000 + 4, 4);
        if (ver != 0xFFFFFFFFUL && (sl < 0 || r > best)) { best = r; sl = i; }
    }
    if (sl >= 0)
        flash[0x7FD000 + sl * 0x1000 + 12] ^= 0x5A;   // the rev: the CRC no longer matches
}

static int32_t val(cfg_from_t f, cfg_key_t k)
{
    int32_t v = -999999;
    cfg_value(f, k, &v);
    return v;
}

// The tuning of rec(), by key, as it must load.
static const cfg_key_t TK[] = { CFG_KP, CFG_TI, CFG_OUTMIN, CFG_DB, CFG_BAND, CFG_KD, CFG_KP2, CFG_TI2, CFG_KD2,
                                CFG_KP3, CFG_TI3, CFG_KD3, CFG_B1, CFG_B2, CFG_HYST, CFG_KP4, CFG_TI4, CFG_KD4,
                                CFG_B3, CFG_PASS_LO, CFG_PASS_HI };
static const int32_t TV[] = { 7, 61, 5, 9, 12, 3, 13, 83, 4, 81, 95, 5, 55, 95, 12, 41, 26, 6, 135, 4, 45 };
static bool tune_as_saved(cfg_key_t but)
{
    bool all = true;
    for (size_t i = 0; i < sizeof TK / sizeof TK[0]; i++)
        all &= TK[i] == but || cfg_tune(TK[i]) == TV[i];
    return all;
}
static bool motor_valve_as_saved(void)
{
    static const int32_t valve[VALVE_N_SETTINGS] = { 60000, 380, 30, 300, 800, 350 };
    static const uint8_t order[4] = { 0, 2, 1, 3 };
    return m_rate == 350 && m_start == 180 && m_accel == 3000 && !m_hold && !m_rev && !memcmp(m_order, order, 4) &&
           !memcmp(v_set, valve, sizeof valve);
}

// Records that are not read: a spoiled one, one with a value out of range,
// one of another size and one of any other version. Factory settings, said.
static void test_not_read(void)
{
    uint32_t rev;
    const char *at;
    rec_t o;

    o = rec();
    put_rec(0x7FD000, &o);
    flash[0x7FD000 + 100] ^= 1;
    reboot();
    CHECK(!cfg_rev(CFG_FROM_SAVED, &rev, &at) && said("Saved record not used (slot A): CRC mismatch") && said("No saved settings"));
    o = rec();
    o.remote = 2;
    put_rec(0x7FD000, &o);
    reboot();
    CHECK(!cfg_rev(CFG_FROM_SAVED, &rev, &at) && said("Saved record not used (slot A): value out of range"));
    o = rec();
    o.size = sizeof o - 8;
    put_rec(0x7FD000, &o);
    reboot();
    CHECK(!cfg_rev(CFG_FROM_SAVED, &rev, &at) && said("Saved record not used (slot A): not a settings record of this version"));
    for (uint32_t ver = 1; ver <= 14; ver++) {
        if (ver == 13)
            continue;
        o = rec();
        o.version = ver;
        put_rec(0x7FD000, &o);
        reboot();
        CHECK(!cfg_rev(CFG_FROM_SAVED, &rev, &at) && said("Saved record not used (slot A): not a settings record of this version") &&
              said("No saved settings"));
        CHECK(m_rate == 400 && cfg_tune(CFG_KP) == 6 && cal_is_default());
    }
    // A save from there is the first of a new board, in A.
    CHECK(save_all() && cfg_rev(CFG_FROM_SAVED, &rev, &at) && rev == 1);
    {
        uint32_t ver, size;
        memcpy(&ver, flash + 0x7FD004, 4);
        memcpy(&size, flash + 0x7FD008, 4);
        CHECK(ver == 13 && size == sizeof(rec_t));
    }
    wipe();
}

static void test_cfg(void)
{
    int32_t v[CFG_N] = { 0 };
    bool set[CFG_N] = { 0 };
    const char *why;
    uint32_t rev;
    const char *at;

    // A new board: nothing saved, the factory settings and the nominal cal.
    reboot();
    CHECK(!cfg_rev(CFG_FROM_SAVED, &rev, &at) && said("No saved settings"));
    CHECK(m_rate == 400 && val(CFG_FROM_LIVE, CFG_KP) == 6 && val(CFG_FROM_LIVE, CFG_BAND) == 10);   // the factory's (KP 0.06)
    CHECK(cal_is_default() && cfg_sensor_range_mmbar() == 1034214 && cal_pressure_mmbar(10000000) == 1034214);
    CHECK(!cfg_value(CFG_FROM_LIVE, (cfg_key_t)CFG_N, &v[0]) && !cfg_value(CFG_FROM_LIVE, (cfg_key_t)-1, &v[0]));

    // A board with one saved record in A: every setting loads as it was
    // saved, with the two-point cal.
    rec_t o = rec();
    put_rec(0x7FD000, &o);
    reboot();
    CHECK(cfg_rev(CFG_FROM_SAVED, &rev, &at) && rev == 5 && strcmp(at, "2026-10-24 14:04:08") == 0);
    CHECK(said("Settings saved 2026-10-24 14:04:08 loaded (slot A)") && !said("not used"));
    CHECK(motor_valve_as_saved() && tune_as_saved(CFG_N));
    CHECK(strcmp(cfg_name(), "CF-10") == 0 && cfg_remote());
    CHECK(cal_saved() && !cal_is_default() && cal_pressure_mmbar(9300000) == 1013200 && cal_pressure_mmbar(48000) == 0);
    CHECK(cfg_unsaved() == 0);

    // Save from the screens: gain 2.50, run speed 500. Written to B; the
    // record in A stays as the older of the two.
    v[CFG_KP] = 250; set[CFG_KP] = true;
    v[CFG_RATE] = 500; set[CFG_RATE] = true;
    CHECK(cfg_save_with(v, set, &why));
    CHECK(cfg_rev(CFG_FROM_SAVED, &rev, &at) && rev == 6 && strcmp(at, "2026-10-16 08:15:00") == 0);
    uint32_t ver;
    memcpy(&ver, flash + 0x7FE004, 4);
    CHECK(ver == 13);
    memcpy(&ver, flash + 0x7FD004, 4);
    CHECK(ver == 13);
    CHECK(m_rate == 500 && cfg_tune(CFG_KP) == 250 && cfg_unsaved() == 0);

    // Restart: the record in B, with everything else as it was.
    reboot();
    CHECK(m_rate == 500 && cfg_tune(CFG_KP) == 250 && tune_as_saved(CFG_KP));
    CHECK(cfg_rev(CFG_FROM_SAVED, &rev, &at) && rev == 6 && said("loaded (slot B)"));
    CHECK(!cal_is_default() && cal_pressure_mmbar(9300000) == 1013200 && strcmp(cfg_name(), "CF-10") == 0);

    // Undo: a change in use but not saved (Motor override sets the
    // run speed live) goes back to the last save. Refused while the valve
    // moves; with nothing unsaved it changes nothing.
    stepper3_set_rate(450);
    stepper3_set_hold(true);
    CHECK(cfg_unsaved() == 2);
    m_busy = true;
    CHECK(!cfg_undo(&why) && strstr(why, "wait for the valve") && m_rate == 450);
    m_busy = false;
    CHECK(cfg_undo(&why) && m_rate == 500 && !m_hold && cfg_tune(CFG_KP) == 250 && cfg_unsaved() == 0);
    CHECK(said("Undo: settings saved 2026-10-16 08:15:00 back in use"));
    CHECK(cfg_undo(&why) && m_rate == 500 && cfg_unsaved() == 0);
    CHECK(cfg_rev(CFG_FROM_SAVED, &rev, &at) && rev == 6);   // Undo saves nothing

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
    // (A motor setting: the tuning can be saved while it runs.)
    v[CFG_RATE] = 450; set[CFG_RATE] = true;
    m_busy = true;
    CHECK(!cfg_save_with(v, set, &why));
    m_busy = false;
    // A failed flash write: nothing saved and nothing put in use (written
    // before it is applied).
    memset(set, 0, sizeof set);
    v[CFG_RATE] = 450; set[CFG_RATE] = true;
    v[CFG_KP] = 300; set[CFG_KP] = true;
    write_fails = 1;
    CHECK(!cfg_save_with(v, set, &why) && strstr(why, "flash"));
    write_fails = 0;
    CHECK(m_rate == 500 && cfg_tune(CFG_KP) == 250 && cfg_unsaved() == 0);
    CHECK(cfg_rev(CFG_FROM_SAVED, &rev, &at) && rev == 6);

    // Reset to factory: the motor, valve and tuning settings to
    // factory, saved at once (rev 7 in A); the calibration, the name, the
    // address and Network control stay. Refused while the valve moves.
    m_busy = true;
    CHECK(!cfg_reset_factory(&why) && m_rate == 500);
    m_busy = false;
    CHECK(cfg_reset_factory(&why) && said("Reset to factory"));
    CHECK(cfg_rev(CFG_FROM_SAVED, &rev, &at) && rev == 7 && cfg_unsaved() == 0);
    CHECK(m_rate == 400 && m_start == 200 && v_set[0] == 99072 && cfg_tune(CFG_KP) == 6);
    CHECK(!cal_is_default() && cal_saved() && cal_pressure_mmbar(9300000) == 1013200 && strcmp(cfg_name(), "CF-10") == 0);
    // The factory PID table: B 6/10/14 mbar, 15 %; zones 0.06/0.60,
    // 0.12/0.82, 0.80/0.94, 0.40/0.25, KD 0.
    CHECK(cfg_tune(CFG_B1) == 60 && cfg_tune(CFG_B2) == 100 && cfg_tune(CFG_B3) == 140 && cfg_tune(CFG_HYST) == 15);
    CHECK(cfg_tune(CFG_TI) == 60 && cfg_tune(CFG_KD) == 0 && cfg_tune(CFG_KP2) == 12 && cfg_tune(CFG_TI2) == 82 &&
          cfg_tune(CFG_KP3) == 80 && cfg_tune(CFG_TI3) == 94 && cfg_tune(CFG_KP4) == 40 && cfg_tune(CFG_TI4) == 25 &&
          cfg_tune(CFG_KD2) == 0 && cfg_tune(CFG_KD3) == 0 && cfg_tune(CFG_KD4) == 0);
    reboot();            // the factory table is what the tests below start from
    CHECK(m_rate == 400 && cfg_tune(CFG_KP) == 6 && !cal_is_default() && cfg_unsaved() == 0);

    // Motor direction: saving it clears the calibration.
    v_cal = true; v_mode = VALVE_MANUAL;
    memset(set, 0, sizeof set);
    v[CFG_REVERSE] = 1; set[CFG_REVERSE] = true;
    CHECK(cfg_save_with(v, set, &why));
    CHECK(m_rev && uncal_calls == 1 && !v_cal);

    // Spoil the newest: the other one loads at the next start-up.
    CHECK(cfg_rev(CFG_FROM_SAVED, &rev, &at));
    uint32_t newest = rev;
    corrupt_newest();
    reboot();
    CHECK(cfg_rev(CFG_FROM_SAVED, &rev, &at) && rev == newest - 1);
    CHECK(!m_rev && said("CRC mismatch"));
}

// Calibrate sensor on the screen: vacuum, then air, then Save.
static void test_capture(void)
{
    const char *why;
    cal_point_t vac, air;
    int32_t v[CFG_N] = { 0 };
    bool set[CFG_N] = { 0 };
    cs.r_ppb = 9300000;
    CHECK(!cal_capture(1, 0, &why));            // not a vacuum
    cs.r_ppb = 51000;
    CHECK(cal_capture(1, 0, &why) && cal_capturing() == 1);
    CHECK(!cal_capture(2, 1013000, &why));      // one at a time
    for (int i = 0; i < 10; i++)
        cal_update(&cs);
    CHECK(cal_capturing() == 0);
    cal_points(&vac, &air);
    CHECK(vac.have && vac.r_ppb == 51000 && air.have);   // air kept from the saved cal
    CHECK(!cal_saved() && cfg_unsaved() == 1);
    // The name saved meanwhile (System saves at once) does not save
    // the new calibration by the way.
    CHECK(cfg_set_name("TEST", &why) && cfg_unsaved() == 2 && cfg_save_system(&why));
    CHECK(!cal_saved() && cfg_unsaved() == 1);
    CHECK(cfg_set_name("CF-10", &why) && cfg_save_system(&why) && !cal_saved() && cfg_unsaved() == 1);
    // Undo puts the saved calibration back; captured again for the rest.
    CHECK(cfg_undo(&why) && cal_saved() && cfg_unsaved() == 0 && cal_pressure_mmbar(9300000) == 1013200);
    CHECK(cal_capture(1, 0, &why));
    for (int i = 0; i < 10; i++)
        cal_update(&cs);
    CHECK(!cal_saved() && cfg_unsaved() == 1);
    cs.r_ppb = 9310000;
    CHECK(cal_capture(2, 1012000, &why));
    for (int i = 0; i < 10; i++)
        cal_update(&cs);
    CHECK(cal_pressure_mmbar(9310000) == 1012000 && cal_pressure_mmbar(51000) == 0);
    // Past the two points the line carries on: full scale and below vacuum.
    CHECK(cal_pressure_mmbar(2 * 9310000 - 51000) == 2024000 && cal_pressure_mmbar(0) < 0);
    const cal_rec_t *sv;
    CHECK(cfg_saved_cal(&sv) && sv && cal_pressure_with(sv, 9300000) == 1013200);
    CHECK(cfg_save_with(v, set, &why) && cal_saved() && cfg_unsaved() == 0);
    reboot();
    CHECK(cal_saved() && !cal_is_default() && cal_pressure_mmbar(9310000) == 1012000 && cal_pressure_mmbar(51000) == 0);
    cal_points(&vac, &air);
    CHECK(vac.have && vac.r_ppb == 51000 && vac.p_mmbar == 0 && air.have && air.r_ppb == 9310000 && air.p_mmbar == 1012000);
}

// Cryostat name: too long refused; set live and saved with cfg_save_system,
// which leaves a setting in use but unsaved as it is.
static void test_name(void)
{
    const char *why;
    uint32_t rev, rev0;
    const char *at;
    reboot();
    cfg_rev(CFG_FROM_SAVED, &rev0, &at);
    CHECK(!cfg_set_name("ABCDEFGHIJKLM", &why) && strcmp(cfg_name(), "CF-10") == 0);
    stepper3_set_rate(444);   // Motor override
    CHECK(cfg_set_name("HM-2", &why) && cfg_unsaved() == 2);
    CHECK(cfg_save_system(&why) && cfg_unsaved() == 1 && m_rate == 444);
    CHECK(cfg_rev(CFG_FROM_SAVED, &rev, &at) && rev == rev0 + 1);
    reboot();
    CHECK(strcmp(cfg_name(), "HM-2") == 0 && m_rate != 444 && cfg_unsaved() == 0);
    // A failed write: the name stays in use, unsaved.
    write_fails = 1;
    CHECK(cfg_set_name("AVM-1", &why) && !cfg_save_system(&why) && strstr(why, "flash") && cfg_unsaved() == 1);
    write_fails = 0;
    CHECK(cfg_set_name("HM-2", &why) && cfg_save_system(&why) && cfg_unsaved() == 0);
}

// IP address (static): parsing, checks, unsaved until a save, used after a
// restart. Network control and the homing return.
static void test_net(void)
{
    const char *why;
    uint32_t ip, mask;
    char a[16], m[16], t[48];
    reboot();
    CHECK(cfg_net_parse("192.168.1.50", &ip) && ip == 0xC0A80132UL);
    CHECK(!cfg_net_parse("192.168.1", &ip) && !cfg_net_parse("192.168.1.256", &ip) &&
          !cfg_net_parse("192.168.1.50x", &ip) && !cfg_net_parse("1.2.3.0004", &ip) && !cfg_net_parse("", &ip));
    CHECK(cfg_net_why(0, 0) == NULL && cfg_net_why(0xC0A80132UL, 0xFFFFFF00UL) == NULL);
    CHECK(cfg_net_why(0xC0A80132UL, 0xFFFF00FFUL) && cfg_net_why(0xC0A80100UL, 0xFFFFFF00UL) &&
          cfg_net_why(0xC0A801FFUL, 0xFFFFFF00UL) && cfg_net_why(0x7F000001UL, 0xFF000000UL) &&
          cfg_net_why(0xE0000001UL, 0xFFFFFF00UL) && cfg_net_why(0xC0A80132UL, 0xFFFFFFFFUL));
    cfg_net_boot(a, m);
    CHECK(strcmp(a, "192.168.1.100") == 0 && strcmp(m, "255.255.255.0") == 0 && !cfg_net_restart_due());
    int u0 = cfg_unsaved();
    CHECK(!cfg_set_net(0xC0A80100UL, 0xFFFFFF00UL, &why) && cfg_unsaved() == u0);
    CHECK(cfg_set_net(0xC0A80132UL, 0xFFFFFF00UL, &why) && cfg_unsaved() == u0 + 1 && cfg_net_restart_due());
    cfg_net_boot(a, m);
    CHECK(strcmp(a, "192.168.1.100") == 0);   // not until a restart
    CHECK(cfg_save_system(&why) && cfg_unsaved() == 0);
    reboot();
    cfg_net(&ip, &mask);
    cfg_net_boot(a, m);
    CHECK(ip == 0xC0A80132UL && mask == 0xFFFFFF00UL && strcmp(a, "192.168.1.50") == 0 &&
          strcmp(m, "255.255.255.0") == 0 && !cfg_net_restart_due());
    CHECK(strcmp(cfg_net_text(t, sizeof t, ip, mask), "192.168.1.50 255.255.255.0") == 0);
    CHECK(cfg_set_net(0, 0, &why) && cfg_save_system(&why));   // back to the build default

    // Network control: On by default, live at once and
    // saved with cfg_save_system.
    CHECK(cfg_remote());
    int u1 = cfg_unsaved();
    cfg_set_remote(false);
    CHECK(!cfg_remote() && cfg_unsaved() == u1 + 1);
    CHECK(cfg_save_system(&why) && cfg_unsaved() == 0);
    reboot();
    CHECK(!cfg_remote());
    cfg_set_remote(true);
    CHECK(cfg_save_system(&why) && cfg_remote() && cfg_unsaved() == 0);

    // Homing return: its own setting beside the back-off.
    {
        int32_t bo = 0, rt = 0;
        CHECK(cfg_value(CFG_FROM_LIVE, CFG_BACKOFF, &bo) && cfg_value(CFG_FROM_LIVE, CFG_RETURN, &rt) && rt == 400);
        int32_t v2[CFG_N] = { 0 };
        bool set2[CFG_N] = { false };
        v2[CFG_BACKOFF] = 300; set2[CFG_BACKOFF] = true;
        v2[CFG_RETURN] = 250; set2[CFG_RETURN] = true;
        CHECK(cfg_save_with(v2, set2, &why));
        reboot();
        CHECK(cfg_value(CFG_FROM_LIVE, CFG_BACKOFF, &bo) && bo == 300 && cfg_value(CFG_FROM_LIVE, CFG_RETURN, &rt) && rt == 250);
    }
}

// KD: a saved setting.
static void test_kd(void)
{
    int32_t v[CFG_N] = { 0 }, lo, hi;
    bool set[CFG_N] = { false };
    const char *why;
    cfg_range(CFG_KD, &lo, &hi);
    CHECK(lo == 0 && hi == 10000 && cfg_tune(CFG_KD) == 0);
    cfg_range(CFG_BAND, &lo, &hi);   // the setpoint band from 0
    CHECK(lo == 0 && hi == 1000);
    v[CFG_KD] = 150; set[CFG_KD] = true;
    v[CFG_KP] = 40; set[CFG_KP] = true;
    CHECK(cfg_save_with(v, set, &why) && cfg_tune(CFG_KD) == 150);
    reboot();
    CHECK(cfg_tune(CFG_KD) == 150 && cfg_tune(CFG_KP) == 40);
    v[CFG_KD] = 10001;
    set[CFG_KP] = false;
    CHECK(!cfg_save_with(v, set, &why));
    CHECK(cfg_tune(CFG_KD) == 150);
}

// The PID table (four zones, always on): the zones, the boundaries, the
// hysteresis and the closing deadband are saved settings. Boundary 3 at 0
// is three zones.
static void test_table(void)
{
    int32_t v[CFG_N] = { 0 }, lo, hi;
    bool set[CFG_N] = { false };
    const char *why;
    CHECK(cfg_pid_key(1, 0) == CFG_KP && cfg_pid_key(2, 1) == CFG_TI2 && cfg_pid_key(3, 2) == CFG_KD3);
    CHECK(cfg_pid_key(4, 0) == CFG_KP4 && cfg_pid_key(4, 1) == CFG_TI4 && cfg_pid_key(4, 2) == CFG_KD4);
    CHECK(cfg_pid_key(5, 0) == CFG_KP);
    cfg_range(CFG_HYST, &lo, &hi);
    CHECK(lo == 0 && hi == 40);
    cfg_range(CFG_B3, &lo, &hi);
    CHECK(lo == 0 && hi == 10000);
    CHECK(cfg_tune(CFG_B1) == 60 && cfg_tune(CFG_B2) == 100 && cfg_tune(CFG_B3) == 140);
    // Boundaries too close for the hysteresis: refused, nothing changes.
    v[CFG_B1] = 200; set[CFG_B1] = true;
    v[CFG_B2] = 250; set[CFG_B2] = true;
    v[CFG_HYST] = 15; set[CFG_HYST] = true;
    CHECK(!cfg_save_with(v, set, &why) && strstr(why, "B2 must be above B1"));
    CHECK(cfg_tune(CFG_B1) == 60 && cfg_tune(CFG_B2) == 100 && cfg_tune(CFG_B3) == 140);
    // B2 20 mbar is above the factory B3 (14), so refused unless B3 is 0
    // (three zones) or above.
    memset(set, 0, sizeof set);
    const cfg_key_t k[] = { CFG_KP, CFG_TI, CFG_KP2, CFG_TI2, CFG_KP3, CFG_TI3, CFG_B1, CFG_B2, CFG_HYST, CFG_DB, CFG_B3 };
    const int32_t x[] = { 10, 40, 80, 30, 120, 30, 60, 200, 15, 200, 0 };
    for (int i = 0; i < 10; i++) { v[k[i]] = x[i]; set[k[i]] = true; }
    CHECK(!cfg_save_with(v, set, &why) && strstr(why, "B3 must be above B2"));
    v[CFG_B3] = 0; set[CFG_B3] = true;
    CHECK(cfg_save_with(v, set, &why));
    reboot();
    bool all = true;
    for (int i = 0; i < 11; i++) all &= cfg_tune(k[i]) == x[i];
    CHECK(all && cfg_unsaved() == 0);
    // In AUTO the PID table is tuning too: taken without leaving AUTO.
    v_auto = true;
    memset(set, 0, sizeof set);
    v[CFG_KP3] = 150; set[CFG_KP3] = true;
    CHECK(cfg_save_with(v, set, &why) && cfg_tune(CFG_KP3) == 150);
    v_auto = false;
    // A four-zone table: 0.06/0.70, 0.14/1.50, 0.50/1.00, 0.30/0.25
    // at 6, 10 and 14 mbar, hysteresis 10 %.
    const cfg_key_t k4[] = { CFG_KP, CFG_TI, CFG_KP2, CFG_TI2, CFG_KP3, CFG_TI3, CFG_KP4, CFG_TI4,
                             CFG_B1, CFG_B2, CFG_B3, CFG_HYST };
    const int32_t x4[] = { 6, 70, 14, 150, 50, 100, 30, 25, 60, 100, 140, 10 };
    memset(set, 0, sizeof set);
    for (int i = 0; i < 12; i++) { v[k4[i]] = x4[i]; set[k4[i]] = true; }
    CHECK(cfg_save_with(v, set, &why));
    reboot();
    all = true;
    for (int i = 0; i < 12; i++) all &= cfg_tune(k4[i]) == x4[i];
    CHECK(all && cfg_unsaved() == 0);
    // B3 within the hysteresis of B2: refused, nothing changes.
    memset(set, 0, sizeof set);
    v[CFG_B3] = 110; set[CFG_B3] = true;
    CHECK(!cfg_save_with(v, set, &why) && strstr(why, "B3 must be above B2"));
    CHECK(cfg_tune(CFG_B3) == 140);
}

// Disable switch edges: saved settings, From 0 to 99 and To 1 to 100 % open,
// From below To.
static void test_pass(void)
{
    int32_t v[CFG_N] = { 0 }, lo, hi;
    bool set[CFG_N] = { false };
    const char *why;
    reboot();
    CHECK(cfg_tune(CFG_PASS_LO) == 5 && cfg_tune(CFG_PASS_HI) == 50);
    cfg_range(CFG_PASS_LO, &lo, &hi);
    CHECK(lo == 0 && hi == 99);   // the band can cover 0 to 100 %
    cfg_range(CFG_PASS_HI, &lo, &hi);
    CHECK(lo == 1 && hi == 100);
    v[CFG_PASS_LO] = 10; set[CFG_PASS_LO] = true;
    v[CFG_PASS_HI] = 30; set[CFG_PASS_HI] = true;
    CHECK(cfg_save_with(v, set, &why));
    reboot();
    CHECK(cfg_tune(CFG_PASS_LO) == 10 && cfg_tune(CFG_PASS_HI) == 30 && cfg_unsaved() == 0);
    // From at or above To: refused, nothing changes.
    memset(set, 0, sizeof set);
    v[CFG_PASS_LO] = 30; set[CFG_PASS_LO] = true;
    CHECK(!cfg_save_with(v, set, &why) && strstr(why, "From must be below To"));
    CHECK(cfg_tune(CFG_PASS_LO) == 10);
    v[CFG_PASS_LO] = -1;
    CHECK(!cfg_save_with(v, set, &why) && cfg_tune(CFG_PASS_LO) == 10);   // out of range
    // The whole travel, 0 to 100 %.
    v[CFG_PASS_LO] = 0; v[CFG_PASS_HI] = 100; set[CFG_PASS_HI] = true;
    CHECK(cfg_save_with(v, set, &why) && cfg_tune(CFG_PASS_LO) == 0 && cfg_tune(CFG_PASS_HI) == 100);
    v[CFG_PASS_LO] = 10; v[CFG_PASS_HI] = 30;
    CHECK(cfg_save_with(v, set, &why) && cfg_tune(CFG_PASS_LO) == 10 && cfg_tune(CFG_PASS_HI) == 30);
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

    // A move: a moving entry at the start goes before the motor turns, so
    // a reset straight after the start (no valvepos_task yet) offers the
    // start count, marked as saved while moving.
    int32_t off;
    start_move(34000);
    v_cal = false; v_mode = VALVE_NOT_CAL; m_busy = false;
    valvepos_init();
    // Up to 4 s (2 s between entries, the 1 s stall stop, a loop pass) at
    // the run rate the context entry kept.
    CHECK(valvepos_offer(&c) && c == 33880 && valvepos_offer_moving(&off) && off == 400 * 4);
    valvepos_decline();
    // Jog faster than Speed: the context keeps the faster of the two, so
    // the offer covers a jog cut short.
    int32_t jv[CFG_N] = { 0 };
    bool jon[CFG_N] = { false };
    jv[CFG_JOG] = 900;
    jon[CFG_JOG] = true;
    v_cal = true; v_mode = VALVE_MANUAL; m_busy = false; m_pos = 33880;
    CHECK(cfg_apply_with(jv, jon, &why) && cfg_jog_rate() == 900);
    tick(2500);
    CHECK(start_move(34000));
    v_cal = false; v_mode = VALVE_NOT_CAL; m_busy = false;
    valvepos_init();
    CHECK(valvepos_offer(&c) && c == 33880 && valvepos_offer_moving(&off) && off == 900 * 4);
    valvepos_decline();
    jv[CFG_JOG] = 100;
    CHECK(cfg_apply_with(jv, jon, &why) && cfg_jog_rate() == 100);

    // While it moves: the count every 2 s, no more often.
    v_cal = true; v_mode = VALVE_MANUAL; m_pos = 10000;
    tick(2500);                                   // safe at 10000
    CHECK(start_move(10100));                     // moving entry at 10000
    tick(2100);                                   // 2 s on: 10100
    m_pos = 10300;
    tick(1500);                                   // under 2 s since: not yet
    v_cal = false; v_mode = VALVE_NOT_CAL; m_busy = false; m_pos = 0;
    valvepos_init();
    CHECK(valvepos_offer(&c) && c == 10100 && valvepos_offer_moving(&off));
    valvepos_decline();
    v_cal = true; v_mode = VALVE_MANUAL; m_busy = true; m_pos = 10300;
    tick(2100);
    v_cal = false; v_mode = VALVE_NOT_CAL; m_busy = false; m_pos = 0;
    valvepos_init();
    CHECK(valvepos_offer(&c) && c == 10300 && valvepos_offer_moving(&off));
    valvepos_decline();

    // The flash stops keeping up during a move: the move is stopped, so the
    // last entry stays within 2 s of travel.
    v_cal = true; v_mode = VALVE_MANUAL; m_busy = false; m_pos = 10300;
    tick(2500);
    CHECK(start_move(12000));
    m_pos = 10600;
    int s0 = stops;
    write_fails = 1000;
    tick(2100);
    write_fails = 0;
    CHECK(stops > s0 && !m_busy);
    m_busy = false;
    tick(2500);   // the flash good again: past the retry wait, safe at the stop
    v_cal = false; v_mode = VALVE_NOT_CAL; m_pos = 0;
    valvepos_init();
    CHECK(valvepos_offer(&c) && c == 10600);
    valvepos_decline();

    // Calibrated at a new count without a move (VALVE CLOSED): the old
    // entry is replaced at once, as moving; safe after 2 s still.
    v_cal = true; v_mode = VALVE_MANUAL; m_pos = 20000;
    lv_tick_inc(1);               // odd/even tick does not matter
    tick(1500);
    v_cal = false; v_mode = VALVE_NOT_CAL; m_pos = 0;
    valvepos_init();
    CHECK(valvepos_offer(&c) && c == 20000 && valvepos_offer_moving(&off));
    valvepos_decline();
    v_cal = true; v_mode = VALVE_MANUAL; m_pos = 20000;
    tick(2500);
    v_cal = false; v_mode = VALVE_NOT_CAL; m_pos = 0;
    valvepos_init();
    CHECK(valvepos_offer(&c) && c == 20000 && !valvepos_offer_moving(&off));
    valvepos_decline();

    // Calibration lost: withdrawn, nothing on offer.
    v_cal = false; v_mode = VALVE_NOT_CAL;
    tick(100);
    valvepos_init();
    CHECK(!valvepos_offer(&c));

    // The move-start write fails once: the second try takes it, in the
    // next slot, after a blank one. valvepos_init() clears the module's RAM
    // as a reset does, so only the flash tells it what happened.
    v_cal = true; v_mode = VALVE_MANUAL; m_pos = 20000;
    tick(2500);
    write_fails = 1;
    CHECK(start_move(21000));
    m_busy = false;
    v_cal = false; v_mode = VALVE_NOT_CAL;
    valvepos_init();
    CHECK(valvepos_offer(&c) && c == 20000 && valvepos_offer_moving(&off));
    valvepos_decline();

    // 33880 saved, first write fails, retry
    // works, move to 34000, cold restart: the start, as moving, not safe.
    v_cal = true; v_mode = VALVE_MANUAL; m_pos = 33880;
    tick(2500);
    write_fails = 1;
    CHECK(start_move(34000));
    m_busy = false;
    v_cal = false; v_mode = VALVE_NOT_CAL; m_pos = 0;
    valvepos_init();
    CHECK(valvepos_offer(&c) && c == 33880 && valvepos_offer_moving(&off));
    valvepos_decline();

    // Both tries fail: the move is refused, so the saved count still
    // matches the valve and is still offered after a restart.
    v_cal = true; v_mode = VALVE_MANUAL; m_pos = 30000;
    tick(2500);
    write_fails = 2;
    CHECK(!start_move(31000) && m_pos == 30000 && !m_busy);
    v_cal = false; v_mode = VALVE_NOT_CAL; m_pos = 0;
    valvepos_init();
    CHECK(valvepos_offer(&c) && c == 30000 && !valvepos_offer_moving(&off));
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
    v_cal = false; v_mode = VALVE_CALIBRATING; m_busy = true;   // valve.c uncalibrates while homing
    tick(100);
    valvepos_init();
    CHECK(!valvepos_offer(&c));

    // Saved under another motor direction (Direction changed on Motor
    // override and not saved, then a restart): not offered; under the
    // direction it was saved, it is.
    m_busy = false;
    v_cal = true; v_mode = VALVE_MANUAL; m_pos = 25000; m_rev = true;
    tick(2500);
    v_cal = false; v_mode = VALVE_NOT_CAL; m_pos = 0; m_rev = false;
    valvepos_init();
    CHECK(!valvepos_offer(&c));
    m_rev = true;
    valvepos_init();
    CHECK(valvepos_offer(&c) && c == 25000);
    m_rev = false;   // the direction changes while the offer waits: withdrawn
    CHECK(!valvepos_offer(&c) && !valvepos_use(&why));

    // An entry with no direction recorded is offered under the direction in
    // use, even a reversed one. Written raw into sector 0, after the others.
    struct { uint32_t seq; int32_t count; uint32_t state, crc; } old_e = { 0x7FFFFFF0u, 12345, 0x45464153u, 0 };
    old_e.crc = crc32(&old_e, 12);
    extflash_erase_4k(0x7EC000UL);
    extflash_write(0x7EC000UL, &old_e, sizeof old_e);
    m_rev = true;
    v_cal = false; v_mode = VALVE_NOT_CAL; m_pos = 0;
    valvepos_init();
    CHECK(valvepos_offer(&c) && c == 12345);
    valvepos_decline();
    m_rev = false;

    // Shut down (at 0 %): OFF writes a parked entry at
    // once, saying whether the valve was homed. Every start is OFF; with the
    // switch released the count is taken (still OFF), whatever the reset;
    // pressed: nothing taken or offered, the entry withdrawn.
    v_cal = true; v_mode = VALVE_MANUAL; m_pos = 30000; v_ref = VALVE_REF_HOMED;
    tick(2500);
    m_pos = 0; v_mode = VALVE_OFF;
    tick(10);
    v_cal = false; v_mode = VALVE_OFF; m_pos = 0;
    reset_why = "power-on"; lim = LIMIT_UNKNOWN; parked_calls = 0;
    valvepos_init();
    CHECK(!valvepos_offer(&c));
    tick(50);
    CHECK(parked_calls == 0 && !v_cal);         // waits for a steady switch reading
    lim = LIMIT_CLOSED;
    tick(10);
    CHECK(parked_calls == 1 && v_cal && v_mode == VALVE_OFF && m_pos == 0 && parked_homed);
    v_mode = VALVE_MANUAL;                      // turned on: safe at once
    tick(10);
    v_cal = false; v_mode = VALVE_OFF; m_pos = 0;
    valvepos_init();
    tick(10);
    CHECK(parked_calls == 1 && !v_cal && valvepos_offer(&c) && c == 0 && valvepos_offer_unparked());
    valvepos_decline();
    // Shut down again, then a watchdog reset: taken as well.
    v_cal = true; v_mode = VALVE_OFF; m_pos = 0;
    tick(10);
    v_cal = false; m_pos = 0; reset_why = "watchdog";
    valvepos_init();
    tick(10);
    CHECK(parked_calls == 2 && v_cal && v_mode == VALVE_OFF && m_pos == 0 && parked_homed);
    // Set by hand, not homed: parked as not homed.
    v_ref = VALVE_REF_HAND; m_pos = 5000;
    tick(10);
    v_cal = false; m_pos = 0; reset_why = "power-on";
    valvepos_init();
    tick(10);
    CHECK(parked_calls == 3 && v_cal && m_pos == 5000 && !parked_homed);
    v_ref = VALVE_REF_HOMED;
    // The switch pressed at start (closed further while off).
    v_cal = false; v_mode = VALVE_OFF; m_pos = 0; lim = LIMIT_OPEN;
    valvepos_init();
    tick(10);
    CHECK(parked_calls == 3 && !v_cal && !valvepos_offer(&c));
    lim = LIMIT_CLOSED;
    valvepos_init();                            // withdrawn: nothing next time either
    tick(10);
    CHECK(parked_calls == 3 && !v_cal && !valvepos_offer(&c));
    v_mode = VALVE_NOT_CAL;
    // Switched off without Shut down: offered, and said so.
    v_cal = true; v_mode = VALVE_MANUAL; m_pos = 22000;
    tick(2500);
    v_cal = false; v_mode = VALVE_NOT_CAL; m_pos = 0;
    valvepos_init();
    CHECK(valvepos_offer(&c) && c == 22000 && valvepos_offer_unparked());
    valvepos_decline();
    reset_why = "watchdog";
    valvepos_init();
    CHECK(valvepos_offer(&c) && !valvepos_offer_unparked());
    valvepos_decline();
    reset_why = "power-on";
    // Power lost in AUTO: the context entry has the mode and the
    // setpoint; back in MANUAL before the cut, it says nothing.
    int32_t sp;
    v_cal = true; v_mode = VALVE_AUTO; v_auto = true; v_sp = 8000; m_pos = 23000;
    tick(2500);
    v_cal = false; v_mode = VALVE_NOT_CAL; v_auto = false; m_pos = 0;
    valvepos_init();
    CHECK(valvepos_offer(&c) && c == 23000 && valvepos_offer_auto(&sp) && sp == 8000);
    valvepos_decline();
    v_cal = true; v_mode = VALVE_MANUAL; m_pos = 23000;
    tick(2500);
    v_cal = false; v_mode = VALVE_NOT_CAL; m_pos = 0;
    valvepos_init();
    CHECK(valvepos_offer(&c) && c == 23000 && !valvepos_offer_auto(&sp));
    valvepos_decline();
    v_sp = 5000;
}

// Trend slots are fixed one-second boundaries: a late poll fills its own
// slot (polls at 0, 1.6 and 2.0 s fill slots 0, 1 and 2), a stall leaves
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
    // main.c calls on its own 1 s grid: a call a few ms earlier in its second
    // than the one before is a new second, not a repeat, so no reading is
    // dropped after a late first call.
    uint32_t n1 = trend_seconds();
    lv_tick_inc(810);  trend_second(&q);   // 10 ms after a slot edge
    lv_tick_inc(985);  trend_second(&q);   // 5 ms before the next edge
    lv_tick_inc(1000); trend_second(&q);
    lv_tick_inc(995);  trend_second(&q);
    CHECK(trend_seconds() - n1 == 4);
}

// In AUTO only the tuning changes (screens and PID); the trend
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
    CHECK(!cfg_save_with(v, set, &why) && strstr(why, "Stop AUTO"));
    CHECK(cfg_tune(CFG_KP) == 300 && m_rate == rate);
    CHECK(!cfg_reset_factory(&why) && strstr(why, "Stop AUTO"));
    CHECK(cfg_undo(&why));   // nothing but the tuning can differ in AUTO: allowed
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
    test_not_read();
    test_cfg();
    test_capture();
    test_name();
    test_net();
    test_kd();
    test_table();
    test_pass();
    test_valvepos();
    test_trend();
    test_auto();
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
