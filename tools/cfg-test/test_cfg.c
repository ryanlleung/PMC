/*
 * Host test of the settings store (cfg.c, cal.c) and the saved valve
 * position (valvepos.c) against a serial flash held in RAM. Run with run.sh.
 *
 * Covers: a version 1 record (0.6.0 to 0.8.x) loading as one sensor; a save
 * from the screens (cfg_save_with) going to the other sector as version 2;
 * a restart; Load settings > Previous save; the IP address (version 4); a combination refused as a
 * whole; a record with several sensors (0.9.0 to 0.16.1) loading as the one
 * in use at the 15 psia range and saving as one (0.16.2); a motor
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
static stepper3_mode_t m_mode = STEPPER3_FULL;
static bool m_hold, m_rev, m_busy;
static int32_t m_pos;
static uint8_t m_order[4] = { 0, 2, 1, 3 };
bool stepper3_set_rate(uint32_t v) { m_rate = v; return true; }
bool stepper3_set_start(uint32_t v) { m_start = v; return true; }
bool stepper3_set_accel(uint32_t v) { m_accel = v; return true; }
static const char *m_lock;   // stepper3_lock(): set by the valve layer, e.g. a latched fault
bool stepper3_set_mode(stepper3_mode_t m) { if (m_lock) return false; m_mode = m; return true; }
const char *stepper3_locked(void) { return m_lock; }
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
stepper3_mode_t stepper3_mode(void) { return m_mode; }
int32_t stepper3_scale(void) { return m_mode == STEPPER3_HALF ? 2 : 1; }
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
const char *stepper3_mode_name(stepper3_mode_t m) { return m == STEPPER3_WAVE ? "WAVE" : m == STEPPER3_FULL ? "FULL" : "HALF"; }

/* Valve */
static int32_t v_set[VALVE_N_SETTINGS] = { 99072, 400, 25, 400, 800, 0, 400 };
static const char *const v_name[VALVE_N_SETTINGS] = { "POSMAX", "FHOME", "FCREEP", "BACKOFF", "CLEAR", "OFFSET", "RETURN" };
static const int32_t v_lo[VALVE_N_SETTINGS] = { 1000, 16, 16, 10, 10, 0, 10 }, v_hi[VALVE_N_SETTINGS] = { 300000, 1000, 1000, 5000, 10000, 5000, 5000 };
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

// A version 10 record (0.16.21) in buf made a version 9 one: tune[22..23],
// the Ignore switch edges, taken out (after the 16-byte header, mot[7],
// valve[7] and tune[0..21]). Returns the version 9 size; the CRC is left for
// the caller.
static uint32_t as_v9(uint8_t *buf, uint32_t size10)
{
    const uint32_t at = 16 + 7 * 4 + 7 * 4 + 22 * 4, nine = 9, size9 = size10 - 2 * 4;
    memmove(buf + at, buf + at + 2 * 4, size10 - at - 2 * 4);
    memcpy(buf + 4, &nine, 4);
    memcpy(buf + 8, &size9, 4);
    return size9;
}

// A version 10 record in buf made a version 8 one: as_v9, then tune[18..21],
// PID set 4 and boundary 3, taken out (after the 16-byte header, mot[7],
// valve[7] and tune[0..17]). Returns the version 8 size; the CRC is left
// for the caller.
static uint32_t as_v8(uint8_t *buf, uint32_t size10)
{
    const uint32_t size9 = as_v9(buf, size10);
    const uint32_t at = 16 + 7 * 4 + 7 * 4 + 18 * 4, eight = 8, size8 = size9 - 4 * 4;
    memmove(buf + at, buf + at + 4 * 4, size9 - at - 4 * 4);
    memcpy(buf + 4, &eight, 4);
    memcpy(buf + 8, &size8, 4);
    return size8;
}

// A version 9 record in buf made a version 7 one: as_v8, then tune[7..17],
// the PID sets 2 and 3, the schedule and the closing deadband, taken out
// (after the 16-byte header, mot[7], valve[7] and tune[0..6]). Returns the
// version 7 size; the CRC is left for the caller.
static uint32_t as_v7(uint8_t *buf, uint32_t size9)
{
    const uint32_t size8 = as_v8(buf, size9);
    const uint32_t at = 16 + 7 * 4 + 7 * 4 + 7 * 4, seven = 7, size7 = size8 - 11 * 4;
    memmove(buf + at, buf + at + 11 * 4, size8 - at - 11 * 4);
    memcpy(buf + 4, &seven, 4);
    memcpy(buf + 8, &size7, 4);
    return size7;
}

// A version 9 record in buf made a version 6 one: as_v7, then tune[6], KD,
// taken out (after the 16-byte header, mot[7], valve[7] and tune[0..5]).
// Returns the version 6 size; the CRC is left for the caller.
static uint32_t as_v6(uint8_t *buf, uint32_t size9)
{
    const uint32_t size7 = as_v7(buf, size9);
    const uint32_t at = 16 + 7 * 4 + 7 * 4 + 6 * 4, six = 6, size6 = size7 - 4;
    memmove(buf + at, buf + at + 4, size7 - at - 4);
    memcpy(buf + 4, &six, 4);
    memcpy(buf + 8, &size6, 4);
    return size6;
}

// A version 6 record (0.13.6) in buf made a version 5 one: valve[6], the
// return, taken out (after the 16-byte header, mot[7] and valve[0..5]).
// Returns the version 5 size; the CRC is left for the caller.
static uint32_t as_v5(uint8_t *buf, uint32_t size6)
{
    const uint32_t at = 16 + 7 * 4 + 6 * 4, five = 5, size5 = size6 - 4;
    memmove(buf + at, buf + at + 4, size6 - at - 4);
    memcpy(buf + 4, &five, 4);
    memcpy(buf + 8, &size5, 4);
    return size5;
}

// The 0.6.0 to 0.8.x layout, as cfg.c reads it.
typedef struct {
    uint32_t magic, version, size, rev;
    int32_t mot[7];
    int32_t valve[6];
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
    int32_t valve[6] = { 99072, 400, 25, 400, 800, 0 };
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
    int32_t valve[VALVE_N_SETTINGS] = { 99072, 400, 25, 400, 800, 0, 400 };
    memcpy(v_set, valve, sizeof valve);
    v_cal = false; v_mode = VALVE_NOT_CAL;
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

// nsens in a version 10 record: after the header, mot[7], valve[7], tune[24].
#define REC_NSENS_AT (16 + 7 * 4 + 7 * 4 + 24 * 4)

// Makes the version 10 record at addr one with three sensors, as 0.9.0 to
// 0.16.1 could save: the one it holds (slot 0), then s/n 1234567 at 30 psi
// and s/n 7654321 at 15 psi, both uncalibrated, with slot active in use.
static void three_sensors(uint32_t addr, uint32_t active)
{
    static uint8_t rec[4096];
    const uint8_t *r = flash + addr;
    uint32_t size, nsens = 3, crc, zero = 0;
    const uint32_t sens_sz = 12 + sizeof(cal_rec_t);
    memcpy(&size, r + 8, 4);
    memcpy(rec, r, size);
    memcpy(rec + REC_NSENS_AT, &nsens, 4);
    memcpy(rec + REC_NSENS_AT + 4, &active, 4);
    const uint32_t sn[2] = { 1234567, 7654321 }, psi[2] = { 300, 150 };
    for (int i = 0; i < 2; i++) {
        uint8_t *q = rec + REC_NSENS_AT + 8 + (i + 1) * sens_sz;
        memset(q, 0, sens_sz);
        memcpy(q, &sn[i], 4);
        memcpy(q + 4, &psi[i], 4);
        memcpy(q + 8, &zero, 4);
    }
    crc = crc32(rec, size - 4);
    memcpy(rec + size - 4, &crc, 4);
    extflash_erase_4k(addr);
    extflash_write(addr, rec, size);
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

    // A 0.8.x board: one version 1 record in A.
    write_v1(0x7FD000, 5);
    reboot();
    CHECK(cfg_rev(CFG_FROM_SAVED, &rev, &at) && rev == 5);
    CHECK(!cfg_rev(CFG_FROM_PREVIOUS, &rev, &at));
    CHECK(m_rate == 350 && m_start == 180);
    CHECK(cfg_sensor_range_mmbar() == 1034214 && strncmp(cal_id(), "5880156 bench", 13) == 0);
    CHECK(cal_saved() && !cal_is_default());
    CHECK(!cfg_value(CFG_FROM_LIVE, (cfg_key_t)CFG_N, &v[0]) && !cfg_value(CFG_FROM_LIVE, (cfg_key_t)-1, &v[0]));   // CFG_SENSOR, gone in 0.16.2
    CHECK(val(CFG_FROM_LIVE, CFG_KP) == 6 && val(CFG_FROM_LIVE, CFG_BAND) == 10);   // the factory's (KP 0.06 from 0.16.18)
    CHECK(cfg_unsaved() == 0);

    // Save from the screens: gain 2.50, run speed 500.
    v[CFG_KP] = 250; set[CFG_KP] = true;
    v[CFG_RATE] = 500; set[CFG_RATE] = true;
    CHECK(cfg_save_with(v, set, &why));
    CHECK(cfg_rev(CFG_FROM_SAVED, &rev, &at) && rev == 6 && strcmp(at, "2026-10-16 08:15:00") == 0);
    CHECK(cfg_rev(CFG_FROM_PREVIOUS, &rev, &at) && rev == 5);
    uint32_t ver;
    memcpy(&ver, flash + 0x7FE004, 4);
    CHECK(ver == 10);   // the save went to B, the v1 record in A is kept
    memcpy(&ver, flash + 0x7FD004, 4);
    CHECK(ver == 1);
    CHECK(m_rate == 500 && cfg_tune(CFG_KP) == 250 && cfg_unsaved() == 0);

    // Restart.
    reboot();
    CHECK(m_rate == 500 && cfg_tune(CFG_KP) == 250);
    CHECK(cfg_rev(CFG_FROM_SAVED, &rev, &at) && rev == 6);
    CHECK(!cal_is_default());

    // Load settings > Previous save: the version 1 record in A is from
    // before 0.12.13 (its Direction bit is from either side of 0.11.19), so
    // it is not loaded and nothing changes (to 0.13.28 it loaded).
    CHECK(!cfg_load_live(CFG_FROM_PREVIOUS, &why) && strstr(why, "0.12.13"));
    CHECK(m_rate == 500 && cfg_tune(CFG_KP) == 250 && cfg_unsaved() == 0);
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

    // A record from 0.9.0 to 0.16.1 with three sensors, the second (30 psi,
    // uncalibrated) in use: one sensor per board from 0.16.2, so it loads
    // as that sensor at the 15 psia range, nothing counts as unsaved, and
    // the next save writes it alone.
    CHECK(cfg_rev(CFG_FROM_SAVED, &rev, &at) && rev == 6);
    three_sensors(0x7FE000, 1);   // the newest, rev 6, version 9, in B
    reboot();
    CHECK(cfg_rev(CFG_FROM_SAVED, &rev, &at) && rev == 6);
    CHECK(cal_is_default());   // the sensor in use is uncalibrated (the nominal id, not its serial, until a capture)
    CHECK(cfg_sensor_range_mmbar() == 1034214 && cal_pressure_mmbar(10000000) == 1034214);   // 15 psia, not 30
    CHECK(cfg_unsaved() == 0 && cal_saved());
    const cal_rec_t *sv;
    CHECK(cfg_saved_cal(&sv) && sv == NULL);   // saved without a cal
    CHECK(cfg_save());
    {
        uint32_t nsens, active, ver;
        memcpy(&nsens, flash + 0x7FD000 + REC_NSENS_AT, 4);
        memcpy(&active, flash + 0x7FD000 + REC_NSENS_AT + 4, 4);
        memcpy(&ver, flash + 0x7FD004, 4);
        CHECK(ver == 10 && nsens == 1 && active == 0);   // written as one sensor, rev 7 in A
    }
    // The same record with the bench Druck (s/n 5880156, calibrated) in use,
    // as the previous save: loading it brings that cal.
    three_sensors(0x7FE000, 0);
    reboot();
    CHECK(cfg_rev(CFG_FROM_SAVED, &rev, &at) && rev == 7 && cal_is_default());
    CHECK(cfg_load_live(CFG_FROM_PREVIOUS, &why));
    CHECK(strncmp(cal_id(), "5880156 bench", 13) == 0 && !cal_is_default() && cal_pressure_mmbar(9300000) == 1013200);
    CHECK(cfg_unsaved() == 1 && !cal_saved());   // the calibration differs from the newest save
    CHECK(cfg_save());
    CHECK(cfg_unsaved() == 0 && cal_saved());

    // Factory defaults keep the sensor's calibration.
    CHECK(cfg_load_live(CFG_FROM_DEFAULTS, &why));
    CHECK(m_rate == 400 && cfg_tune(CFG_KP) == 6 && !cal_is_default());
    // The factory PID table (0.16.18, Ryan 12 Nov): On by the pressure, B 6/10/14 mbar, 15 %; sets 0.06/0.60,
    // 0.12/0.82, 0.80/0.94, 0.40/0.25, KD 0.
    CHECK(cfg_tune(CFG_SCHED) == 2 && cfg_tune(CFG_B1) == 60 && cfg_tune(CFG_B2) == 100 && cfg_tune(CFG_B3) == 140 &&
          cfg_tune(CFG_HYST) == 15);
    CHECK(cfg_tune(CFG_TI) == 60 && cfg_tune(CFG_KD) == 0 && cfg_tune(CFG_KP2) == 12 && cfg_tune(CFG_TI2) == 82 &&
          cfg_tune(CFG_KP3) == 80 && cfg_tune(CFG_TI3) == 94 && cfg_tune(CFG_KP4) == 40 && cfg_tune(CFG_TI4) == 25 &&
          cfg_tune(CFG_KD2) == 0 && cfg_tune(CFG_KD3) == 0 && cfg_tune(CFG_KD4) == 0);
    CHECK(cfg_load_live(CFG_FROM_SAVED, &why));

    // A latched fault locks the step mode: a load that would change it is
    // refused whole, nothing applied; without the lock it loads.
    m_mode = STEPPER3_HALF;
    m_rate = 450;
    m_lock = "Reset the fault first";
    CHECK(!cfg_load_live(CFG_FROM_DEFAULTS, &why) && strstr(why, "fault") && m_rate == 450 &&
          m_mode == STEPPER3_HALF);
    m_lock = NULL;
    CHECK(cfg_load_live(CFG_FROM_DEFAULTS, &why) && m_rate == 400 && m_mode == STEPPER3_FULL);
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
    corrupt_newest();
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
    CHECK(!cal_capture(1, 0, &why));            // not a vacuum
    cs.r_ppb = 51000;
    CHECK(cal_capture(1, 0, &why) && cal_capturing() == 1);
    CHECK(!cal_capture(2, 1013000, &why));      // one at a time
    for (int i = 0; i < 10; i++)
        cal_update(&cs);
    CHECK(cal_capturing() == 0);
    cal_points(&vac, &air);
    CHECK(vac.have && vac.r_ppb == 51000 && air.have);   // air kept from the table
    CHECK(!cal_saved() && cfg_unsaved() == 1);
    cs.r_ppb = 9310000;
    CHECK(cal_capture(2, 1012000, &why));
    for (int i = 0; i < 10; i++)
        cal_update(&cs);
    CHECK(cal_pressure_mmbar(9310000) == 1012000 && cal_pressure_mmbar(51000) == 0);
    CHECK(strncmp(cal_id(), "5880156 bench 26-10-16", 22) == 0);
    const cal_rec_t *sv;
    CHECK(cfg_saved_cal(&sv) && sv && cal_pressure_with(sv, 9300000) == 1013200);
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
        uint32_t size4, a3 = 0;
        int sl = 0;
        for (int i = 0; i < 2; i++) {
            uint32_t r3;
            memcpy(&r3, flash + 0x7FD000 + i * 0x1000 + 12, 4);
            uint32_t v3;
            memcpy(&v3, flash + 0x7FD000 + i * 0x1000 + 4, 4);
            if (v3 == 10 && r3 >= a3) { a3 = r3; sl = i; }
        }
        uint32_t base = 0x7FD000 + sl * 0x1000;
        memcpy(&size4, flash + base + 8, 4);
        static uint8_t buf[4096];
        memcpy(buf, flash + base, size4);
        size4 = as_v5(buf, as_v6(buf, size4));
        uint32_t size2 = size4 - 4 - 8 - (CFG_NAME_LEN + 4);   // size4: the version 5 size
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

// IP address (version 4, 0.12.13; the DHCP fallback until 0.14.9): parsing, checks, unsaved until a
// save, used after a restart, and a version 3 record loading with the
// build default.
static void test_net(void)
{
    int32_t v[CFG_N] = { 0 };
    bool set[CFG_N] = { 0 };
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
    CHECK(cfg_save_with(v, set, &why) && cfg_unsaved() == 0);
    reboot();
    cfg_net(&ip, &mask);
    cfg_net_boot(a, m);
    CHECK(ip == 0xC0A80132UL && mask == 0xFFFFFF00UL && strcmp(a, "192.168.1.50") == 0 &&
          strcmp(m, "255.255.255.0") == 0 && !cfg_net_restart_due());
    CHECK(strcmp(cfg_net_text(t, sizeof t, ip, mask), "192.168.1.50 255.255.255.0") == 0);
    // The record rewritten as version 3 (0.10.9 to 0.12.12): the build default.
    {
        uint32_t size4, a4 = 0;
        int sl = 0;
        for (int i = 0; i < 2; i++) {
            uint32_t r, ver;
            memcpy(&r, flash + 0x7FD000 + i * 0x1000 + 12, 4);
            memcpy(&ver, flash + 0x7FD000 + i * 0x1000 + 4, 4);
            if (ver == 10 && r >= a4) { a4 = r; sl = i; }
        }
        uint32_t base = 0x7FD000 + sl * 0x1000, three = 3;
        memcpy(&size4, flash + base + 8, 4);
        static uint8_t buf[4096];
        memcpy(buf, flash + base, size4);
        size4 = as_v5(buf, as_v6(buf, size4));
        uint32_t size3 = size4 - 4 - 8;   // size4: the version 5 size
        memcpy(buf + 4, &three, 4);
        memcpy(buf + 8, &size3, 4);
        uint32_t c = crc32(buf, size3 - 4);
        memcpy(buf + size3 - 4, &c, 4);
        extflash_erase_4k(base);
        extflash_write(base, buf, size3);
        reboot();
        uint32_t rev;
        const char *at;
        cfg_net(&ip, &mask);
        cfg_net_boot(a, m);
        CHECK(cfg_rev(CFG_FROM_SAVED, &rev, &at) && rev == a4 && ip == 0 && mask == 0 &&
              strcmp(a, "192.168.1.100") == 0 && strcmp(cfg_name(), "CF-11") == 0);
    }
    CHECK(cfg_set_net(0, 0, &why));   // back to the build default

    // Remote control (0.13.4): On by default (0.13.7), live at once and
    // unsaved, Off kept by a save, On again from a version 4 record.
    CHECK(cfg_remote());
    int u1 = cfg_unsaved();
    cfg_set_remote(false);
    CHECK(!cfg_remote() && cfg_unsaved() == u1 + 1);
    CHECK(cfg_save_with(v, set, &why) && cfg_unsaved() == 0);
    reboot();
    CHECK(!cfg_remote());
    {
        uint32_t size5, a5 = 0;
        int sl = 0;
        for (int i = 0; i < 2; i++) {
            uint32_t r, ver;
            memcpy(&r, flash + 0x7FD000 + i * 0x1000 + 12, 4);
            memcpy(&ver, flash + 0x7FD000 + i * 0x1000 + 4, 4);
            if (ver == 10 && r >= a5) { a5 = r; sl = i; }
        }
        uint32_t base = 0x7FD000 + sl * 0x1000, four = 4;
        memcpy(&size5, flash + base + 8, 4);
        static uint8_t buf[4096];
        memcpy(buf, flash + base, size5);
        size5 = as_v5(buf, as_v6(buf, size5));
        uint32_t size4 = size5 - 4;
        memcpy(buf + 4, &four, 4);
        memcpy(buf + 8, &size4, 4);
        uint32_t c = crc32(buf, size4 - 4);
        memcpy(buf + size4 - 4, &c, 4);
        extflash_erase_4k(base);
        extflash_write(base, buf, size4);
        reboot();
        uint32_t rev;
        const char *at;
        CHECK(cfg_rev(CFG_FROM_SAVED, &rev, &at) && rev == a5 && cfg_remote());
    }

    // Homing return (0.13.6): a version 5 record loads it as its back-off.
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
        uint32_t size6, a6 = 0;
        int sl = 0;
        for (int i = 0; i < 2; i++) {
            uint32_t r, ver;
            memcpy(&r, flash + 0x7FD000 + i * 0x1000 + 12, 4);
            memcpy(&ver, flash + 0x7FD000 + i * 0x1000 + 4, 4);
            if (ver == 10 && r >= a6) { a6 = r; sl = i; }
        }
        uint32_t base = 0x7FD000 + sl * 0x1000;
        memcpy(&size6, flash + base + 8, 4);
        static uint8_t buf[4096];
        memcpy(buf, flash + base, size6);
        uint32_t size5 = as_v5(buf, as_v6(buf, size6));
        uint32_t c = crc32(buf, size5 - 4);
        memcpy(buf + size5 - 4, &c, 4);
        extflash_erase_4k(base);
        extflash_write(base, buf, size5);
        reboot();
        uint32_t rev;
        const char *at;
        CHECK(cfg_rev(CFG_FROM_SAVED, &rev, &at) && rev == a6);
        CHECK(cfg_value(CFG_FROM_LIVE, CFG_BACKOFF, &bo) && bo == 300 && cfg_value(CFG_FROM_LIVE, CFG_RETURN, &rt) && rt == 300);
    }
}

// KD (0.14.21): a saved setting from version 7; a version 6 record loads
// KD 0 (the loop as it ran) and keeps the rest of its tuning.
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
    {
        uint32_t size7, a7 = 0;
        int sl = 0;
        for (int i = 0; i < 2; i++) {
            uint32_t r, ver;
            memcpy(&r, flash + 0x7FD000 + i * 0x1000 + 12, 4);
            memcpy(&ver, flash + 0x7FD000 + i * 0x1000 + 4, 4);
            if (ver == 10 && r >= a7) { a7 = r; sl = i; }
        }
        uint32_t base = 0x7FD000 + sl * 0x1000;
        memcpy(&size7, flash + base + 8, 4);
        static uint8_t buf[4096];
        memcpy(buf, flash + base, size7);
        uint32_t size6 = as_v6(buf, size7);   // size7: the version 9 record's
        uint32_t c = crc32(buf, size6 - 4);
        memcpy(buf + size6 - 4, &c, 4);
        extflash_erase_4k(base);
        extflash_write(base, buf, size6);
        reboot();
        uint32_t rev;
        const char *at;
        CHECK(cfg_rev(CFG_FROM_SAVED, &rev, &at) && rev == a7);
        CHECK(cfg_tune(CFG_KD) == 0 && cfg_tune(CFG_KP) == 40);
    }
    v[CFG_KD] = 10001;
    set[CFG_KP] = false;
    CHECK(!cfg_save_with(v, set, &why));
    CHECK(cfg_tune(CFG_KD) == 0);
}

// Gain schedule (0.15.15, version 8): PID sets 2 and 3, the schedule and
// the closing deadband are saved settings; a version 7 record loads with
// sets 2 and 3 as set 1, the schedule on (Off to 0.16.19; with every set
// the same AUTO runs as it did) and the closing deadband as the deadband.
static void test_sched(void)
{
    int32_t v[CFG_N] = { 0 }, lo, hi;
    bool set[CFG_N] = { false };
    const char *why;
    CHECK(cfg_pid_key(1, 0) == CFG_KP && cfg_pid_key(2, 1) == CFG_TI2 && cfg_pid_key(3, 2) == CFG_KD3);
    cfg_range(CFG_SCHED, &lo, &hi);
    CHECK(lo == 2 && hi == 2);   // always on the pressure from 0.16.20
    cfg_range(CFG_HYST, &lo, &hi);
    CHECK(lo == 0 && hi == 40);
    // Boundaries too close for the hysteresis: refused, nothing changes.
    v[CFG_B1] = 200; set[CFG_B1] = true;
    v[CFG_B2] = 250; set[CFG_B2] = true;
    v[CFG_HYST] = 15; set[CFG_HYST] = true;
    CHECK(!cfg_save_with(v, set, &why) && strstr(why, "B2 must be above B1"));
    CHECK(cfg_tune(CFG_B1) == 60 && cfg_tune(CFG_B2) == 100 && cfg_tune(CFG_B3) == 140);
    // The 5 Nov proposal for CF-11, on the pressure: B2 20 mbar is above
    // the factory B3 (14), so refused unless B3 is 0 (three sets) or above.
    memset(set, 0, sizeof set);
    const cfg_key_t k[] = { CFG_KP, CFG_TI, CFG_KP2, CFG_TI2, CFG_KP3, CFG_TI3, CFG_SCHED, CFG_B1, CFG_B2, CFG_HYST, CFG_DB, CFG_DBC,
                            CFG_B3 };
    const int32_t x[] = { 10, 40, 80, 30, 120, 30, 2, 60, 200, 15, 200, 8, 0 };
    for (int i = 0; i < 12; i++) { v[k[i]] = x[i]; set[k[i]] = true; }
    CHECK(!cfg_save_with(v, set, &why) && strstr(why, "B3 must be above B2"));
    v[CFG_B3] = 0; set[CFG_B3] = true;
    CHECK(cfg_save_with(v, set, &why));
    reboot();
    bool all = true;
    for (int i = 0; i < 13; i++) all &= cfg_tune(k[i]) == x[i];
    CHECK(all && cfg_unsaved() == 0);
    // In AUTO the PID table is tuning too: taken without leaving AUTO. Off
    // (0) is refused from 0.16.20 (Ryan 12 Nov: "always has pid table").
    v_auto = true;
    memset(set, 0, sizeof set);
    v[CFG_SCHED] = 0; set[CFG_SCHED] = true;
    v[CFG_KP3] = 150; set[CFG_KP3] = true;
    CHECK(!cfg_save_with(v, set, &why) && cfg_tune(CFG_SCHED) == 2 && cfg_tune(CFG_KP3) == 120);
    v[CFG_SCHED] = 2;
    CHECK(cfg_save_with(v, set, &why) && cfg_tune(CFG_SCHED) == 2 && cfg_tune(CFG_KP3) == 150);
    v_auto = false;
    // 1, the setpoint schedule (0.15.15 to 0.15.19), removed in 0.15.20:
    // refused on save; a saved record holding it, or 0 (Off, to 0.16.19),
    // loads with the PID table on the pressure and the sets as saved.
    memset(set, 0, sizeof set);
    v[CFG_SCHED] = 1; set[CFG_SCHED] = true;
    CHECK(!cfg_save_with(v, set, &why) && cfg_tune(CFG_SCHED) == 2);
    {
        uint32_t size8, a8 = 0, one = 1;
        int sl = 0;
        for (int i = 0; i < 2; i++) {
            uint32_t r, ver;
            memcpy(&r, flash + 0x7FD000 + i * 0x1000 + 12, 4);
            memcpy(&ver, flash + 0x7FD000 + i * 0x1000 + 4, 4);
            if (ver == 10 && r >= a8) { a8 = r; sl = i; }
        }
        uint32_t base = 0x7FD000 + sl * 0x1000;
        memcpy(&size8, flash + base + 8, 4);
        static uint8_t buf[4096];
        memcpy(buf, flash + base, size8);
        memcpy(buf + 16 + 7 * 4 + 7 * 4 + 13 * 4, &one, 4);   // tune[T_SCHED]
        uint32_t c = crc32(buf, size8 - 4);
        memcpy(buf + size8 - 4, &c, 4);
        extflash_erase_4k(base);
        extflash_write(base, buf, size8);
        reboot();
        uint32_t rev;
        const char *at;
        CHECK(cfg_rev(CFG_FROM_SAVED, &rev, &at) && rev == a8);
        CHECK(cfg_tune(CFG_SCHED) == 2 && cfg_tune(CFG_KP3) == 150 && cfg_tune(CFG_KP) == 10);
        uint32_t off = 0;                                       // the same with 0, Off
        memcpy(buf + 16 + 7 * 4 + 7 * 4 + 13 * 4, &off, 4);
        c = crc32(buf, size8 - 4);
        memcpy(buf + size8 - 4, &c, 4);
        extflash_erase_4k(base);
        extflash_write(base, buf, size8);
        reboot();
        CHECK(cfg_rev(CFG_FROM_SAVED, &rev, &at) && rev == a8);
        CHECK(cfg_tune(CFG_SCHED) == 2 && cfg_tune(CFG_KP3) == 150 && cfg_tune(CFG_KP) == 10);
    }
    // The same record as version 7.
    {
        uint32_t size8, a8 = 0;
        int sl = 0;
        for (int i = 0; i < 2; i++) {
            uint32_t r, ver;
            memcpy(&r, flash + 0x7FD000 + i * 0x1000 + 12, 4);
            memcpy(&ver, flash + 0x7FD000 + i * 0x1000 + 4, 4);
            if (ver == 10 && r >= a8) { a8 = r; sl = i; }
        }
        uint32_t base = 0x7FD000 + sl * 0x1000;
        memcpy(&size8, flash + base + 8, 4);
        static uint8_t buf[4096];
        memcpy(buf, flash + base, size8);
        uint32_t size7 = as_v7(buf, size8);
        uint32_t c = crc32(buf, size7 - 4);
        memcpy(buf + size7 - 4, &c, 4);
        extflash_erase_4k(base);
        extflash_write(base, buf, size7);
        reboot();
        uint32_t rev;
        const char *at;
        CHECK(cfg_rev(CFG_FROM_SAVED, &rev, &at) && rev == a8);
        CHECK(cfg_tune(CFG_KP) == 10 && cfg_tune(CFG_TI) == 40 && cfg_tune(CFG_DB) == 200);
        CHECK(cfg_tune(CFG_KP2) == 10 && cfg_tune(CFG_TI2) == 40 && cfg_tune(CFG_KP3) == 10 && cfg_tune(CFG_TI3) == 40 &&
              cfg_tune(CFG_KD2) == cfg_tune(CFG_KD) && cfg_tune(CFG_KD3) == cfg_tune(CFG_KD));
        CHECK(cfg_tune(CFG_SCHED) == 2 && cfg_tune(CFG_B1) == 60 && cfg_tune(CFG_B2) == 100 && cfg_tune(CFG_HYST) == 15 &&
              cfg_tune(CFG_DBC) == 200);   // on, every set set 1's: as it ran
        CHECK(cfg_tune(CFG_KP4) == 10 && cfg_tune(CFG_TI4) == 40 && cfg_tune(CFG_KD4) == cfg_tune(CFG_KD) &&
              cfg_tune(CFG_B3) == 140);
        CHECK(cfg_unsaved() == 0);
    }
}

// Fourth PID set (0.16.14, version 9): set 4 and boundary 3 are saved
// settings, B3 0 is three sets; a version 8 record loads with set 4 as set
// 3 and B3 0, its three sets as they ran.
static void test_set4(void)
{
    int32_t v[CFG_N] = { 0 }, lo, hi;
    bool set[CFG_N] = { false };
    const char *why;
    CHECK(cfg_pid_key(4, 0) == CFG_KP4 && cfg_pid_key(4, 1) == CFG_TI4 && cfg_pid_key(4, 2) == CFG_KD4);
    CHECK(cfg_pid_key(5, 0) == CFG_KP);
    cfg_range(CFG_B3, &lo, &hi);
    CHECK(lo == 0 && hi == 10000);
    // The 11 Nov four-set table: 0.06/0.70, 0.14/1.50, 0.50/1.00, 0.30/0.25
    // at 6, 10 and 14 mbar, hysteresis 10 %.
    const cfg_key_t k[] = { CFG_KP, CFG_TI, CFG_KP2, CFG_TI2, CFG_KP3, CFG_TI3, CFG_KP4, CFG_TI4,
                            CFG_SCHED, CFG_B1, CFG_B2, CFG_B3, CFG_HYST };
    const int32_t x[] = { 6, 70, 14, 150, 50, 100, 30, 25, 2, 60, 100, 140, 10 };
    for (int i = 0; i < 13; i++) { v[k[i]] = x[i]; set[k[i]] = true; }
    CHECK(cfg_save_with(v, set, &why));
    reboot();
    bool all = true;
    for (int i = 0; i < 13; i++) all &= cfg_tune(k[i]) == x[i];
    CHECK(all && cfg_unsaved() == 0);
    // B3 within the hysteresis of B2: refused, nothing changes.
    memset(set, 0, sizeof set);
    v[CFG_B3] = 110; set[CFG_B3] = true;
    CHECK(!cfg_save_with(v, set, &why) && strstr(why, "B3 must be above B2"));
    CHECK(cfg_tune(CFG_B3) == 140);
    // As version 8: the three sets kept, set 4 = set 3, B3 0.
    {
        uint32_t size9, a9 = 0;
        int sl = 0;
        for (int i = 0; i < 2; i++) {
            uint32_t r, ver;
            memcpy(&r, flash + 0x7FD000 + i * 0x1000 + 12, 4);
            memcpy(&ver, flash + 0x7FD000 + i * 0x1000 + 4, 4);
            if (ver == 10 && r >= a9) { a9 = r; sl = i; }
        }
        uint32_t base = 0x7FD000 + sl * 0x1000;
        memcpy(&size9, flash + base + 8, 4);
        static uint8_t buf[4096];
        memcpy(buf, flash + base, size9);
        uint32_t size8 = as_v8(buf, size9);
        uint32_t c = crc32(buf, size8 - 4);
        memcpy(buf + size8 - 4, &c, 4);
        extflash_erase_4k(base);
        extflash_write(base, buf, size8);
        reboot();
        uint32_t rev;
        const char *at;
        CHECK(cfg_rev(CFG_FROM_SAVED, &rev, &at) && rev == a9);
        CHECK(cfg_tune(CFG_KP) == 6 && cfg_tune(CFG_KP2) == 14 && cfg_tune(CFG_KP3) == 50 && cfg_tune(CFG_TI3) == 100);
        CHECK(cfg_tune(CFG_KP4) == 50 && cfg_tune(CFG_TI4) == 100 && cfg_tune(CFG_KD4) == cfg_tune(CFG_KD3));
        CHECK(cfg_tune(CFG_B3) == 0 && cfg_tune(CFG_SCHED) == 2 && cfg_tune(CFG_B2) == 100 && cfg_tune(CFG_HYST) == 10);
        CHECK(cfg_unsaved() == 0);
        // The next save writes version 10.
        CHECK(cfg_save());
        uint32_t ver = 0;
        for (int i = 0; i < 2; i++) {
            uint32_t r, vv;
            memcpy(&r, flash + 0x7FD000 + i * 0x1000 + 12, 4);
            memcpy(&vv, flash + 0x7FD000 + i * 0x1000 + 4, 4);
            if (r == a9 + 1) ver = vv;
        }
        CHECK(ver == 10);
    }
}

// Disable switch edges (0.16.21, version 10, Ryan 12 Nov "make the boundary
// editable"): saved settings, From 0 to 99 and To 1 to 100 % open (0.16.22), From
// below To; a version 9 record loads them at 5 and 50, the band before.
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
    // As version 9: the rest kept, the edges 5 and 50.
    {
        uint32_t size10, a10 = 0;
        int sl = 0;
        for (int i = 0; i < 2; i++) {
            uint32_t r, ver;
            memcpy(&r, flash + 0x7FD000 + i * 0x1000 + 12, 4);
            memcpy(&ver, flash + 0x7FD000 + i * 0x1000 + 4, 4);
            if (ver == 10 && r >= a10) { a10 = r; sl = i; }
        }
        uint32_t base = 0x7FD000 + sl * 0x1000;
        memcpy(&size10, flash + base + 8, 4);
        static uint8_t buf[4096];
        memcpy(buf, flash + base, size10);
        int32_t b3;
        memcpy(&b3, buf + 16 + 7 * 4 + 7 * 4 + 21 * 4, 4);
        uint32_t size9 = as_v9(buf, size10);
        uint32_t c = crc32(buf, size9 - 4);
        memcpy(buf + size9 - 4, &c, 4);
        extflash_erase_4k(base);
        extflash_write(base, buf, size9);
        reboot();
        uint32_t rev;
        const char *at;
        CHECK(cfg_rev(CFG_FROM_SAVED, &rev, &at) && rev == a10);
        CHECK(cfg_tune(CFG_PASS_LO) == 5 && cfg_tune(CFG_PASS_HI) == 50 && cfg_tune(CFG_B3) == b3);
        CHECK(cfg_unsaved() == 0);
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
    CHECK(!cfg_load_live(CFG_FROM_DEFAULTS, &why) && strstr(why, "Stop AUTO"));
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
    test_net();
    test_kd();
    test_sched();
    test_set4();
    test_pass();
    test_valvepos();
    test_trend();
    test_auto();
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
