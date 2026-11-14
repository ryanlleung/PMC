/*
 * Host test of the settings store (cfg.c, cal.c) and the saved valve
 * position (valvepos.c) against a serial flash held in RAM. Run with run.sh.
 *
 * Covers: a blank flash; a version 10 record (0.16.21 to 0.16.29) loading
 * and converting, with what it held that is not kept (a step mode, a
 * reversed pressure response, several sensors, a cal that is not two points)
 * said in the events; versions 1 to 9 not read; a save from the screens
 * (cfg_save_with) going to the other sector as version 11; a restart; Undo
 * and Reset to factory; the cryostat name, the IP address and Remote
 * control saved as they are set; a combination refused as a whole; a motor direction change
 * clearing the calibration; a spoiled record; and the saved valve position
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
// Shut down (0.15.0; at 0 % and every start OFF from 0.15.1): the parked
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

// The version 10 layout (0.16.21 to 0.16.29), byte for byte as 0.16.29 wrote
// it: declared here again, so a slip in cfg.c's copy shows.
typedef struct {
    uint32_t serial;
    int32_t psi10;
    uint32_t has_cal;
    cal_rec_v4_t cal;
} sens_v10_t;
typedef struct {
    uint32_t magic, version, size, rev;
    int32_t mot[7];              // RATE, START, ACCEL, MODE, HOLD, DIR, ORDER
    int32_t valve[7];            // POSMAX, FHOME, FCREEP, BACKOFF, CLEAR, the zero offset, RETURN
    int32_t tune[24];            // KP, TI, OUTMIN, DB, KDIR, BAND, KD, sets 2 and 3, SCHED, B1, B2, HYST, DBC, set 4, B3, PASSLO, PASSHI
    uint32_t nsens, active;
    sens_v10_t sens[4];
    char saved_at[20];
    char name[CFG_NAME_LEN + 4];
    uint32_t net_ip, net_mask;
    uint32_t remote;
    uint32_t crc;
} rec_v10_t;
_Static_assert(sizeof(rec_v10_t) == 1508, "the version 10 record is 1508 bytes");
_Static_assert(offsetof(rec_v10_t, nsens) == 168 && offsetof(rec_v10_t, sens) == 176 &&
               offsetof(rec_v10_t, crc) == 1504, "version 10 offsets");

// A version 4 cal record: n points (2 for the bench cal), the first at vacuum.
static cal_rec_v4_t cal_v4(int n, int32_t r_vac, int32_t r_air, int32_t p_air)
{
    cal_rec_v4_t c;
    memset(&c, 0, sizeof c);
    c.magic = CAL_MAGIC;
    c.version = 4;
    c.npts = (uint32_t)n;
    strcpy(c.id, "5880156 bench 26-10-24");
    c.r_ppb[0] = r_vac; c.p_mmbar[0] = 0;
    c.r_ppb[n - 1] = r_air; c.p_mmbar[n - 1] = p_air;
    for (int i = 1; i < n - 1; i++) {   // a typed table: points between, on the line
        c.r_ppb[i] = r_vac + (r_air - r_vac) / (n - 1) * i;
        c.p_mmbar[i] = p_air / (n - 1) * i;
    }
    c.kind = n == 2;   // 1: CAL VAC + CAL AIR
    c.crc = crc32(&c, offsetof(cal_rec_v4_t, crc));
    return c;
}

// The record a 0.16.29 board holds: the values of
// simulator/tests/fixtures/flash-0.16.29.json, rev 5.
static rec_v10_t v10(void)
{
    rec_v10_t r;
    memset(&r, 0, sizeof r);
    r.magic = 0x47464350UL;
    r.version = 10;
    r.size = sizeof r;
    r.rev = 5;
    const int32_t mot[7] = { 350, 180, 3000, 1, 0, 0, 213 };
    const int32_t valve[7] = { 60000, 380, 30, 300, 800, 0, 350 };
    const int32_t tune[24] = { 7, 61, 5, 9, 1, 12, 3, 13, 83, 4, 81, 95, 5, 2, 55, 95, 12, 9, 41, 26, 6, 135, 4, 45 };
    memcpy(r.mot, mot, sizeof mot);
    memcpy(r.valve, valve, sizeof valve);
    memcpy(r.tune, tune, sizeof tune);
    r.nsens = 1;
    r.sens[0].serial = 5880156;
    r.sens[0].psi10 = 150;
    r.sens[0].has_cal = 1;
    r.sens[0].cal = cal_v4(2, 48000, 9300000, 1013200);
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

static void put_v10(uint32_t addr, rec_v10_t *r)
{
    r->crc = crc32(r, offsetof(rec_v10_t, crc));
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

// The tuning of v10(), by key, as it must load.
static const cfg_key_t TK[] = { CFG_KP, CFG_TI, CFG_OUTMIN, CFG_DB, CFG_BAND, CFG_KD, CFG_KP2, CFG_TI2, CFG_KD2,
                                CFG_KP3, CFG_TI3, CFG_KD3, CFG_B1, CFG_B2, CFG_HYST, CFG_KP4, CFG_TI4, CFG_KD4,
                                CFG_B3, CFG_PASS_LO, CFG_PASS_HI };
static const int32_t TV[] = { 7, 61, 5, 9, 12, 3, 13, 83, 4, 81, 95, 5, 55, 95, 12, 41, 26, 6, 135, 4, 45 };
static bool tune_as_v10(cfg_key_t but)
{
    bool all = true;
    for (size_t i = 0; i < sizeof TK / sizeof TK[0]; i++)
        all &= TK[i] == but || cfg_tune(TK[i]) == TV[i];
    return all;
}
static bool motor_valve_as_v10(void)
{
    static const int32_t valve[VALVE_N_SETTINGS] = { 60000, 380, 30, 300, 800, 350 };
    static const uint8_t order[4] = { 0, 2, 1, 3 };
    return m_rate == 350 && m_start == 180 && m_accel == 3000 && !m_hold && !m_rev && !memcmp(m_order, order, 4) &&
           !memcmp(v_set, valve, sizeof valve);
}

// What a version 10 record held that 0.16.30 does not keep, and the records
// that are not read.
static void test_v10(void)
{
    uint32_t rev, ip, mask;
    const char *at;
    rec_v10_t o;

    // Half step, with an address, Remote control Off: the settings were in
    // full steps in every mode, so they load as they are; the mode is said.
    o = v10();
    o.mot[3] = 2;
    o.net_ip = 0xC0A80132UL; o.net_mask = 0xFFFFFF00UL; o.remote = 0;
    put_v10(0x7FD000, &o);
    reboot();
    cfg_net(&ip, &mask);
    CHECK(cfg_rev(CFG_FROM_SAVED, &rev, &at) && rev == 5 && motor_valve_as_v10() && tune_as_v10(CFG_N));
    CHECK(said("Step mode in the saved settings not kept: full step only") && !said("Pressure response"));
    CHECK(ip == 0xC0A80132UL && mask == 0xFFFFFF00UL && !cfg_remote() && strcmp(cfg_name(), "CF-10") == 0);
    CHECK(cfg_unsaved() == 0);

    // Pressure response Reversed.
    o = v10();
    o.tune[4] = -1;
    put_v10(0x7FD000, &o);
    reboot();
    CHECK(cfg_rev(CFG_FROM_SAVED, &rev, &at) && tune_as_v10(CFG_N) && cfg_remote());
    CHECK(said("Pressure response Reversed in the saved settings not kept") && !said("Step mode"));

    // A closing deadband of its own (0.15.15 to 0.16.31): the opening one is
    // kept, for both ways, and the events say so.
    o = v10();
    o.tune[17] = 7;
    put_v10(0x7FD000, &o);
    reboot();
    CHECK(cfg_rev(CFG_FROM_SAVED, &rev, &at) && tune_as_v10(CFG_N) && cfg_tune(CFG_DB) == 9);
    CHECK(said("Deadband close in the saved settings not kept: one deadband, 9 steps") && !said("Step mode"));

    // Three sensors (0.9.0 to 0.16.1 could save them), the second (30 psi,
    // uncalibrated) in use: it loads as that sensor at the 15 psia range,
    // nothing counts as unsaved, and the next save writes the one.
    o = v10();
    o.nsens = 3; o.active = 1;
    o.sens[1].serial = 1234567; o.sens[1].psi10 = 300;
    o.sens[2].serial = 7654321; o.sens[2].psi10 = 150;
    put_v10(0x7FD000, &o);
    reboot();
    CHECK(cfg_rev(CFG_FROM_SAVED, &rev, &at) && rev == 5 && said("Several sensors in the saved settings"));
    CHECK(cal_is_default() && cfg_sensor_range_mmbar() == 1034214 && cal_pressure_mmbar(10000000) == 1034214);   // 15 psia, not 30
    CHECK(cfg_unsaved() == 0 && cal_saved());
    const cal_rec_t *sv;
    CHECK(cfg_saved_cal(&sv) && sv == NULL);   // saved without a cal
    CHECK(save_all());
    {
        uint32_t ver, size;
        memcpy(&ver, flash + 0x7FE004, 4);
        memcpy(&size, flash + 0x7FE008, 4);
        CHECK(ver == 11 && size < 300 && cfg_rev(CFG_FROM_SAVED, &rev, &at) && rev == 6);
    }
    // The same with the bench Druck (the first, calibrated) in use.
    wipe();
    o.active = 0;
    put_v10(0x7FD000, &o);
    reboot();
    CHECK(said("Several sensors") && !cal_is_default() && cal_saved() && cal_pressure_mmbar(9300000) == 1013200);

    // A typed table of three points (the engineering commands, to 0.15.x):
    // not loaded, the nominal cal, and said.
    o = v10();
    o.sens[0].cal = cal_v4(3, 48000, 9300000, 1013200);
    put_v10(0x7FD000, &o);
    reboot();
    CHECK(cfg_rev(CFG_FROM_SAVED, &rev, &at) && tune_as_v10(CFG_N) && cal_is_default());
    CHECK(said("Saved calibration is not a two-point one: not loaded, calibrate the sensor"));
    // A cal record that does not check: the same.
    o = v10();
    o.sens[0].cal.r_ppb[1]++;
    put_v10(0x7FD000, &o);
    reboot();
    CHECK(cal_is_default() && said("not a two-point one"));

    // Not read: a spoiled record, one with no sensor in use, one of another
    // size, and versions 1 to 9 (0.6.0 to 0.16.20). Factory settings, said.
    o = v10();
    put_v10(0x7FD000, &o);
    flash[0x7FD000 + 100] ^= 1;
    reboot();
    CHECK(!cfg_rev(CFG_FROM_SAVED, &rev, &at) && said("Record in A not used: CRC mismatch") && said("No saved settings"));
    o = v10();
    o.active = 1;
    put_v10(0x7FD000, &o);
    reboot();
    CHECK(!cfg_rev(CFG_FROM_SAVED, &rev, &at) && said("Record in A not used: value out of range"));
    o = v10();
    o.size = sizeof o - 8;
    put_v10(0x7FD000, &o);
    reboot();
    CHECK(!cfg_rev(CFG_FROM_SAVED, &rev, &at) && said("Record in A not used: not a settings record of this version"));
    for (uint32_t ver = 1; ver <= 9; ver++) {
        o = v10();
        o.version = ver;
        put_v10(0x7FD000, &o);
        reboot();
        CHECK(!cfg_rev(CFG_FROM_SAVED, &rev, &at) && said("Record in A not used: saved before 0.16.21, no longer read") &&
              said("No saved settings"));
        CHECK(m_rate == 400 && cfg_tune(CFG_KP) == 6 && cal_is_default());
    }
    // A save from there is the first of a new board.
    CHECK(save_all() && cfg_rev(CFG_FROM_SAVED, &rev, &at) && rev == 1);
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
    CHECK(m_rate == 400 && val(CFG_FROM_LIVE, CFG_KP) == 6 && val(CFG_FROM_LIVE, CFG_BAND) == 10);   // the factory's (KP 0.06 from 0.16.18)
    CHECK(cal_is_default() && cfg_sensor_range_mmbar() == 1034214 && cal_pressure_mmbar(10000000) == 1034214);
    CHECK(!cfg_value(CFG_FROM_LIVE, (cfg_key_t)CFG_N, &v[0]) && !cfg_value(CFG_FROM_LIVE, (cfg_key_t)-1, &v[0]));

    // A 0.16.29 board: one version 10 record in A. Every setting 0.16.30
    // still has loads as it was saved, with the two-point cal.
    rec_v10_t o = v10();
    put_v10(0x7FD000, &o);
    reboot();
    CHECK(cfg_rev(CFG_FROM_SAVED, &rev, &at) && rev == 5 && strcmp(at, "2026-10-24 14:04:08") == 0);
    CHECK(said("Settings saved 2026-10-24 14:04:08 loaded from A (saved by 0.16.29 or earlier)"));
    CHECK(!said("not kept") && !said("Several sensors") && !said("two-point"));
    CHECK(motor_valve_as_v10() && tune_as_v10(CFG_N));
    CHECK(strcmp(cfg_name(), "CF-10") == 0 && cfg_remote());
    CHECK(cal_saved() && !cal_is_default() && cal_pressure_mmbar(9300000) == 1013200 && cal_pressure_mmbar(48000) == 0);
    CHECK(cfg_unsaved() == 0);

    // Save from the screens: gain 2.50, run speed 500. Written as version 11
    // to B; the version 10 record in A is kept, so 0.16.29 flashed back
    // still finds its settings.
    v[CFG_KP] = 250; set[CFG_KP] = true;
    v[CFG_RATE] = 500; set[CFG_RATE] = true;
    CHECK(cfg_save_with(v, set, &why));
    CHECK(cfg_rev(CFG_FROM_SAVED, &rev, &at) && rev == 6 && strcmp(at, "2026-10-16 08:15:00") == 0);
    uint32_t ver;
    memcpy(&ver, flash + 0x7FE004, 4);
    CHECK(ver == 11);
    memcpy(&ver, flash + 0x7FD004, 4);
    CHECK(ver == 10);
    CHECK(m_rate == 500 && cfg_tune(CFG_KP) == 250 && cfg_unsaved() == 0);

    // Restart: the version 11 record, with everything else as it was.
    reboot();
    CHECK(m_rate == 500 && cfg_tune(CFG_KP) == 250 && tune_as_v10(CFG_KP));
    CHECK(cfg_rev(CFG_FROM_SAVED, &rev, &at) && rev == 6 && said("loaded from B") && !said("0.16.29"));
    CHECK(!cal_is_default() && cal_pressure_mmbar(9300000) == 1013200 && strcmp(cfg_name(), "CF-10") == 0);

    // Undo (0.16.31): a change in use but not saved (Motor override sets the
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

    // Reset to factory (0.16.31): the motor, valve and tuning settings to
    // factory, saved at once (rev 7 in A); the calibration, the name, the
    // address and Remote control stay. Refused while the valve moves.
    m_busy = true;
    CHECK(!cfg_reset_factory(&why) && m_rate == 500);
    m_busy = false;
    CHECK(cfg_reset_factory(&why) && said("Reset to factory"));
    CHECK(cfg_rev(CFG_FROM_SAVED, &rev, &at) && rev == 7 && cfg_unsaved() == 0);
    CHECK(m_rate == 400 && m_start == 200 && v_set[0] == 99072 && cfg_tune(CFG_KP) == 6);
    CHECK(!cal_is_default() && cal_saved() && cal_pressure_mmbar(9300000) == 1013200 && strcmp(cfg_name(), "CF-10") == 0);
    // The factory PID table (0.16.18, Ryan 12 Nov): B 6/10/14 mbar, 15 %;
    // sets 0.06/0.60, 0.12/0.82, 0.80/0.94, 0.40/0.25, KD 0.
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
    // The name saved meanwhile (System saves at once, 0.16.31) does not save
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

// Cryostat name: too long refused; set live and saved with cfg_save_system
// (0.16.31), which leaves a setting in use but unsaved as it is.
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

// IP address (0.12.13; the DHCP fallback until 0.14.9): parsing, checks,
// unsaved until a save, used after a restart. Remote control and the homing
// return.
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

    // Remote control (0.13.4): On by default (0.13.7), live at once and
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

    // Homing return (0.13.6): its own setting beside the back-off.
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

// KD (0.14.21): a saved setting.
static void test_kd(void)
{
    int32_t v[CFG_N] = { 0 }, lo, hi;
    bool set[CFG_N] = { false };
    const char *why;
    cfg_range(CFG_KD, &lo, &hi);
    CHECK(lo == 0 && hi == 10000 && cfg_tune(CFG_KD) == 0);
    cfg_range(CFG_BAND, &lo, &hi);   // the setpoint band from 0 (0.16.16, Ryan 12 Nov)
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

// The PID table (0.15.15; four sets from 0.16.14; always on from 0.16.20):
// the sets, the boundaries, the hysteresis and the closing deadband are
// saved settings. Boundary 3 at 0 is three sets.
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
    // The 5 Nov proposal for CF-11: B2 20 mbar is above the factory B3 (14),
    // so refused unless B3 is 0 (three sets) or above.
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
    // The 11 Nov four-set table: 0.06/0.70, 0.14/1.50, 0.50/1.00, 0.30/0.25
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

// Disable switch edges (0.16.21, Ryan 12 Nov "make the boundary editable"):
// saved settings, From 0 to 99 and To 1 to 100 % open (0.16.22), From below To.
static void test_pass(void)
{
    int32_t v[CFG_N] = { 0 }, lo, hi;
    bool set[CFG_N] = { false };
    const char *why;
    reboot();
    CHECK(cfg_tune(CFG_PASS_LO) == 5 && cfg_tune(CFG_PASS_HI) == 50);
    cfg_range(CFG_PASS_LO, &lo, &hi);
    CHECK(lo == 0 && hi == 99);   // 0 to 100 from 0.16.22 (1 to 99 and 2 to 100 in 0.16.21)
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
    // The whole travel, 0 to 100 % (Ryan 13 Nov: "extend the limit switch ignore to 0 to 100% inputtable").
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
    // the fastest rate.
    CHECK(valvepos_offer(&c) && c == 33880 && valvepos_offer_moving(&off) && off == STEPPER3_RATE_MAX * 4);
    valvepos_decline();

    // While it moves: the count every 2 s, no more often (Ryan, 22 Oct).
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
    lv_tick_inc(1);               // odd/even tick no longer matters
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

    // The same as the review's case: 33880 saved, first write fails, retry
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

    // An entry written before 0.13.27 records no direction: after the
    // upgrade it is offered under the direction in use, as before, even a
    // reversed one. Written raw into sector 0, after the others.
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

    // Shut down (0.15.0; at 0 % from 0.15.1): OFF writes a parked entry at
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
    // main.c calls on its own 1 s grid: a call a few ms earlier in its second
    // than the one before is a new second, not a repeat (to 0.13.25 about
    // every other reading was dropped after a late first call).
    uint32_t n1 = trend_seconds();
    lv_tick_inc(810);  trend_second(&q);   // 10 ms after a slot edge
    lv_tick_inc(985);  trend_second(&q);   // 5 ms before the next edge
    lv_tick_inc(1000); trend_second(&q);
    lv_tick_inc(995);  trend_second(&q);
    CHECK(trend_seconds() - n1 == 4);
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
    CHECK(!cfg_save_with(v, set, &why) && strstr(why, "Stop AUTO"));
    CHECK(cfg_tune(CFG_KP) == 300 && m_rate == rate);
    CHECK(!cfg_reset_factory(&why) && strstr(why, "Stop AUTO"));
    CHECK(!cfg_undo(&why) && strstr(why, "Stop AUTO"));
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
    test_v10();
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
