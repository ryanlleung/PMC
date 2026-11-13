#include <stddef.h>
#include <string.h>

#include "lvgl.h"        // lv_tick_get
#include "link.h"
#include "extflash.h"
#include "cal.h"
#include "stepper3.h"
#include "valve.h"
#include "rtclock.h"
#include "fixed.h"
#include "ctrl.h"
#include "cfg.h"
#include "pmc_config.h"

/* --------------------------------------------------------------------------
 * Record. Little-endian, fixed layout, CRC32 (IEEE) over everything before
 * the crc field. One per 4 KB sector, two sectors used in turn, the higher
 * rev is the newest. Below the old cal record at 0x7FF000.
 *
 * valve[5] was the zero offset (OFFSET, "count at 0 %"); removed in
 * 0.13.31, as % was always worked out from count 0 (a non-zero offset put
 * 0 % below the seat). The slot stays so records keep their layout; it is
 * written 0, and a saved value is dropped when read (an event says so).
 *
 * Version 10 (0.16.21) adds the Disable switch edges (From and To, % open)
 * at the end of tune[]; older records load them at 5 and 50 %, the fixed
 * band before.
 *
 * Version 9 (0.16.14) adds PID set 4 and boundary 3 at the end of tune[].
 * Boundary 3 at 0 means no set 4 (three sets, as before). Version 8 records
 * load with set 4 as set 3 and boundary 3 at 0, so AUTO runs on their three
 * sets until boundary 3 is set.
 *
 * Version 8 (0.15.15) adds PID sets 2 and 3, the gain schedule (type, B1,
 * B2, hysteresis) and the closing deadband at the end of tune[]. Older
 * records load with sets 2 and 3 as set 1, the schedule Off (on from
 * 0.16.20, with every set as set 1, so AUTO still runs as it did) and the
 * closing deadband as the deadband.
 *
 * Version 7 (0.14.21) adds KD at the end of tune[]; older records load KD 0.
 *
 * Version 6 (0.13.6) adds the homing return (valve[6], RETURN: steps off
 * the switch after the slow close); older records load it as their
 * back-off, which homing used for that step until then.
 *
 * Version 5 (0.13.4) adds the Remote control switch (0 Off, 1 On); older
 * records load with it Off.
 *
 * Version 4 (0.12.13) adds the Ethernet address and mask (then the DHCP
 * fallback, the only address from 0.14.9; 0 for
 * the build's PMC_IP); version 3 records load with 0.
 *
 * Version 3 (0.10.9) adds the cryostat name; version 2 records load with
 * the default name.
 *
 * Version 2 (0.9.0) adds the control tuning, the sensors on the board (each
 * with its own cal) and the save time. Version 1 records (0.6.0 to 0.8.x)
 * still load, as one sensor (the bench Druck) with default tuning; the
 * first save writes version 2 to the other sector, so an older firmware
 * flashed back still finds the version 1 record as its newest.
 *
 * One sensor per board from 0.16.2 (Ryan 9 Nov: "only one pressure sensor
 * per mm4 and a new one will be recalibrated anyway"): Choose sensor, Add
 * sensor, Remove sensor and the sensor-in-use setting are gone. The record
 * keeps its layout (nsens, active, sens[4]) so no version is added: a
 * record with several sensors loads the one that was in use (an event names
 * the ones dropped) and the next save writes that one alone, nsens 1. The
 * range is the 15 psia Druck's whatever a record says (a 30 psi range
 * could be picked on Add sensor to 0.16.1; an event says if one is dropped).
 * ------------------------------------------------------------------------ */
#define CFG_ADDR_A   0x7FD000UL
#define CFG_ADDR_B   0x7FE000UL
#define CFG_MAGIC    0x47464350UL   // "PCFG"
#define CFG_VERSION  10
#define OLD_VALVE_N  6       // valve[] in records before version 6
#define REC_SENS_MAX 4       // sensor slots in the record (one in use from 0.16.2)

enum { M_RATE, M_START, M_ACCEL, M_MODE, M_HOLD, M_DIR, M_ORDER, M_N };
static const char *const mot_name[M_N] = { "RATE", "START", "ACCEL", "MODE", "HOLD", "DIR", "ORDER" };

// Control tuning, used from stage 8 (AUTO). Fixed point as noted. New
// values go last so older records keep their order: KD (0.14.21, Ryan 4 Nov;
// records before version 7 have the first six, OLD_TUNE_N, and load KD 0),
// then PID sets 2 and 3, the gain schedule and the closing deadband
// (0.15.15, Ryan 5 Nov; version 7 records have the first seven, V7_TUNE_N),
// then PID set 4 and boundary 3 (0.16.14, Ryan 11 Nov "Yes fourth set";
// version 8 records have the first eighteen, V8_TUNE_N), then the Ignore
// switch edges (0.16.21, Ryan 12 Nov "make the boundary editable"; version 9
// records have the first 22, V9_TUNE_N).
// KP, TI and KD are set 1; DB is the opening deadband.
enum { T_KP, T_TI, T_OUTMIN, T_DB, T_KDIR, T_BAND, T_KD,
       T_KP2, T_TI2, T_KD2, T_KP3, T_TI3, T_KD3, T_SCHED, T_B1, T_B2, T_HYST, T_DBC,
       T_KP4, T_TI4, T_KD4, T_B3, T_PLO, T_PHI, T_N };
#define OLD_TUNE_N  6        // tune[] in records before version 7
#define V7_TUNE_N   7        // tune[] in version 7 records
#define V8_TUNE_N   18       // tune[] in version 8 records
#define V9_TUNE_N   22       // tune[] in version 9 records
static const char *const tune_name[T_N] = { "KP", "TI", "OUTMIN", "DB", "KDIR", "BAND", "KD",
                                            "KP2", "TI2", "KD2", "KP3", "TI3", "KD3", "SCHED", "B1", "B2", "HYST", "DBC",
                                            "KP4", "TI4", "KD4", "B3", "PASSLO", "PASSHI" };
// Factory (0.16.18, Ryan 12 Nov: "set factory pid settings to boundary at 6
// 10 14. kp ti kd values, 0.06, 0.6, 0; 0.12 0.82 0; 0.8 0.94 0, 0.4 0.25
// 0"): the PID table On by the pressure, B1 6, B2 10, B3 14 mbar, 15 %
// hysteresis; set 1 KP 0.06 TI 0.60, set 2 0.12/0.82, set 3 0.80/0.94, set
// 4 0.40/0.25, KD 0 (the bench step tests of 12 Nov). To 0.16.17: one PID,
// KP 4.00 TI 0.25 in every set, the table Off (B2 was 20 to 0.16.13).
static const int32_t tune_def[T_N] = { 6, 60, 0, 8, 1, 10, 0,
                                       12, 82, 0, 80, 94, 0, 2, 60, 100, 15, 8,
                                       40, 25, 0, 140, 5, 50 };
static const int32_t tune_lo[T_N]  = { 1,     0,     0,   0,    -1, 0,    0,
                                       1,     0,     0,     1,     0,     0,     2, 1,     1,     0,  0,
                                       1,     0,     0,     0,     0,  1 };
static const int32_t tune_hi[T_N]  = { 10000, 10000, 500, 2000,  1, 1000, 10000,
                                       10000, 10000, 10000, 10000, 10000, 10000, 2, 10000, 10000, 40, 2000,
                                       10000, 10000, 10000, 10000, 99, 100 };
static int32_t tune[T_N] = { 6, 60, 0, 8, 1, 10, 0,
                             12, 82, 0, 80, 94, 0, 2, 60, 100, 15, 8,
                             40, 25, 0, 140, 5, 50 };
// Fixed point: 0.01 %/mbar, 0.01 min, 0.1 %, steps, +-1, 0.01 mbar (the band
// from 0, 0.10 before 0.16.16, Ryan 12 Nov), 0.01 % per mbar/s;
// sets 2 to 4 as set 1; PID table 2, on the pressure, always (0.16.20, Ryan
// 12 Nov: "remove the set 1 pid value function. always has pid table"; 0 Off
// to 0.16.19 and 1, the setpoint, to 0.15.19 load as 2); B1 to B3
// 0.1 mbar (B3 0: no set 4, three sets); hysteresis % of the boundary;
// closing deadband steps; Disable switch From 0 to 99 and To 1 to 100 %
// open (0.16.22, Ryan 13 Nov: "extend the limit switch ignore to 0 to 100%
// inputtable"; 1 to 99 and 2 to 100 in 0.16.21), From below To (5 and 50,
// the band fixed to 0.16.20).

typedef struct {
    uint32_t serial;
    int32_t psi10;               // range, 0.1 psi (150 = 15 psi)
    uint32_t has_cal;            // 0: nominal over the range
    cal_rec_t cal;
} cfg_sens_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t size;               // sizeof(cfg_rec_t)
    uint32_t rev;                // 1 for the first save, +1 each save
    int32_t mot[M_N];            // ORDER as four digits, e.g. 213 for 0213
    int32_t valve[VALVE_N_SETTINGS];
    int32_t tune[T_N];
    uint32_t nsens;              // 1 to REC_SENS_MAX (1 when written from 0.16.2)
    uint32_t active;             // index of the sensor in use (0 when written from 0.16.2)
    cfg_sens_t sens[REC_SENS_MAX];
    char saved_at[20];           // "YYYY-MM-DD HH:MM:SS", "" if the clock was not set
    char name[CFG_NAME_LEN + 4]; // cryostat name, NUL-terminated (version 3)
    uint32_t net_ip, net_mask;   // Ethernet address, a.b.c.d as a << 24 | ...; 0 0 = build default (version 4)
    uint32_t remote;             // Remote control: 0 Off, 1 On (version 5)
    uint32_t crc;
} cfg_rec_t;

// Version 9 (0.16.14 to 0.16.20), read only: version 10 without the Ignore
// switch edges.
typedef struct {
    uint32_t magic, version, size, rev;
    int32_t mot[M_N];
    int32_t valve[VALVE_N_SETTINGS];
    int32_t tune[V9_TUNE_N];
    uint32_t nsens, active;
    cfg_sens_t sens[REC_SENS_MAX];
    char saved_at[20];
    char name[CFG_NAME_LEN + 4];
    uint32_t net_ip, net_mask;
    uint32_t remote;
    uint32_t crc;
} cfg_rec_v9_t;

// Version 8 (0.15.15 to 0.16.13), read only: version 9 with the first
// eighteen tuning values (three PID sets).
typedef struct {
    uint32_t magic, version, size, rev;
    int32_t mot[M_N];
    int32_t valve[VALVE_N_SETTINGS];
    int32_t tune[V8_TUNE_N];
    uint32_t nsens, active;
    cfg_sens_t sens[REC_SENS_MAX];
    char saved_at[20];
    char name[CFG_NAME_LEN + 4];
    uint32_t net_ip, net_mask;
    uint32_t remote;
    uint32_t crc;
} cfg_rec_v8_t;

// Version 7 (0.14.21 to 0.15.14), read only: version 8 with the first
// seven tuning values.
typedef struct {
    uint32_t magic, version, size, rev;
    int32_t mot[M_N];
    int32_t valve[VALVE_N_SETTINGS];
    int32_t tune[V7_TUNE_N];
    uint32_t nsens, active;
    cfg_sens_t sens[REC_SENS_MAX];
    char saved_at[20];
    char name[CFG_NAME_LEN + 4];
    uint32_t net_ip, net_mask;
    uint32_t remote;
    uint32_t crc;
} cfg_rec_v7_t;

// Version 6 (0.13.6 to 0.14.20), read only: version 7 without KD.
typedef struct {
    uint32_t magic, version, size, rev;
    int32_t mot[M_N];
    int32_t valve[VALVE_N_SETTINGS];
    int32_t tune[OLD_TUNE_N];
    uint32_t nsens, active;
    cfg_sens_t sens[REC_SENS_MAX];
    char saved_at[20];
    char name[CFG_NAME_LEN + 4];
    uint32_t net_ip, net_mask;
    uint32_t remote;
    uint32_t crc;
} cfg_rec_v6_t;

// Version 5 (0.13.4 and 0.13.5), read only: version 6 with one valve
// setting fewer.
typedef struct {
    uint32_t magic, version, size, rev;
    int32_t mot[M_N];
    int32_t valve[OLD_VALVE_N];
    int32_t tune[OLD_TUNE_N];
    uint32_t nsens, active;
    cfg_sens_t sens[REC_SENS_MAX];
    char saved_at[20];
    char name[CFG_NAME_LEN + 4];
    uint32_t net_ip, net_mask;
    uint32_t remote;
    uint32_t crc;
} cfg_rec_v5_t;

// Version 4 (0.12.13 to 0.13.3), read only: version 5 without the switch.
typedef struct {
    uint32_t magic, version, size, rev;
    int32_t mot[M_N];
    int32_t valve[OLD_VALVE_N];
    int32_t tune[OLD_TUNE_N];
    uint32_t nsens, active;
    cfg_sens_t sens[REC_SENS_MAX];
    char saved_at[20];
    char name[CFG_NAME_LEN + 4];
    uint32_t net_ip, net_mask;
    uint32_t crc;
} cfg_rec_v4_t;
_Static_assert(offsetof(cfg_rec_v4_t, crc) == offsetof(cfg_rec_v5_t, remote), "v4 is v5 up to the switch");

// Version 3 (0.10.9 to 0.12.12), read only: version 4 without the address.
typedef struct {
    uint32_t magic, version, size, rev;
    int32_t mot[M_N];
    int32_t valve[OLD_VALVE_N];
    int32_t tune[OLD_TUNE_N];
    uint32_t nsens, active;
    cfg_sens_t sens[REC_SENS_MAX];
    char saved_at[20];
    char name[CFG_NAME_LEN + 4];
    uint32_t crc;
} cfg_rec_v3_t;
_Static_assert(offsetof(cfg_rec_v3_t, crc) == offsetof(cfg_rec_v5_t, net_ip), "v3 is v4 up to the address");

// Version 2 (0.9.0 to 0.10.8), read only: version 3 without the name.
typedef struct {
    uint32_t magic, version, size, rev;
    int32_t mot[M_N];
    int32_t valve[OLD_VALVE_N];
    int32_t tune[OLD_TUNE_N];
    uint32_t nsens, active;
    cfg_sens_t sens[REC_SENS_MAX];
    char saved_at[20];
    uint32_t crc;
} cfg_rec_v2_t;
_Static_assert(offsetof(cfg_rec_v2_t, crc) == offsetof(cfg_rec_v5_t, name), "v2 is v3 up to the name");

// Version 1 (0.6.0 to 0.8.x), read only.
typedef struct {
    uint32_t magic, version, size, rev;
    int32_t mot[M_N];
    int32_t valve[OLD_VALVE_N];
    uint32_t has_cal;
    cal_rec_t cal;
    uint32_t crc;
} cfg_rec_v1_t;

// The sensor the board was built with, for version 1 records and defaults.
#define FACTORY_SERIAL  5880156UL
#define FACTORY_PSI10   150     // the 15 psia Druck: the only range from 0.16.2

// The live sensor: its serial (the bench cal id) and the range. Its cal
// lives in cal.c; has_cal and cal here are filled on capture().
static cfg_sens_t sensor = { FACTORY_SERIAL, FACTORY_PSI10, 0, { 0 } };
// Live cryostat name, in the header ("CF-11 pressure controller").
#define FACTORY_NAME "CF-11"
static char cname[CFG_NAME_LEN + 1] = FACTORY_NAME;
// Live Ethernet address (unsaved until a save, like the name), and the one
// loaded at start-up, which the Ethernet link uses until the next restart.
static uint32_t net_ip, net_mask, net_boot_ip, net_boot_mask;
// Live Remote control switch (0.13.4): On by default (0.13.7, Ryan 28 Oct),
// as a saved record says once one is loaded.
static bool remote_on = true;

static const uint32_t slot_addr[2] = { CFG_ADDR_A, CFG_ADDR_B };
static cfg_rec_t slot[2];        // as read from each sector (version 1 converted)
static const char *slot_why[2];  // NULL if valid, else why not
static uint32_t slot_ver[2];     // version found in each sector
static int newest = -1;          // slot with the newest valid record, -1 none
static int32_t slot_offset[2];   // the removed zero offset each record held (dropped on read)
static int loaded = -1;          // slot loaded at start-up, -1 none
static uint32_t loaded_rev;
static cfg_rec_t factory;        // start-up values before any record is loaded
static cfg_rec_t scratch;        // the live settings, captured
static bool flash_ok;

static const char *const EMPTY = "empty";

static uint32_t rec_crc(const cfg_rec_t *r)
{
    return crc32_ieee(r, offsetof(cfg_rec_t, crc));
}

static char slot_letter(int i) { return i == 0 ? 'A' : 'B'; }

// When a record was saved, for the events: "2026-10-16 14:32:05", or
// "with no date" if the clock was not set (or the record is version 1).
static const char *when(const cfg_rec_t *r)
{
    static char b[24];
    if (!r->saved_at[0])
        return "with no date";
    lv_snprintf(b, sizeof b, "%.19s", r->saved_at);
    return b;
}

static void order_digits(int32_t packed, uint8_t o[4])
{
    o[0] = packed / 1000 % 10; o[1] = packed / 100 % 10; o[2] = packed / 10 % 10; o[3] = packed % 10;
}

static int32_t psi10_mmbar(int32_t psi10)
{
    return (int32_t)(((int64_t)psi10 * 6894757 + 500) / 1000);   // 68.94757 mbar/psi
}

int32_t cfg_sensor_range_mmbar(void) { return psi10_mmbar(sensor.psi10); }

static void serial_text(char *buf, size_t n, uint32_t serial)
{
    lv_snprintf(buf, n, "%lu", (unsigned long)serial);
}

// Tells cal.c the sensor's serial (the bench cal id) and nominal range.
static void tell_cal(void)
{
    char sn[12];
    serial_text(sn, sizeof sn, sensor.serial);
    cal_set_sensor(sn, psi10_mmbar(sensor.psi10));
}

// The live settings.
static void capture(cfg_rec_t *r)
{
    uint8_t o[4];
    memset(r, 0, sizeof *r);
    r->magic = CFG_MAGIC;
    r->version = CFG_VERSION;
    r->size = sizeof *r;
    r->mot[M_RATE] = (int32_t)stepper3_rate();
    r->mot[M_START] = (int32_t)stepper3_start_rate();
    r->mot[M_ACCEL] = (int32_t)stepper3_accel();
    r->mot[M_MODE] = stepper3_mode();
    r->mot[M_HOLD] = stepper3_hold();
    r->mot[M_DIR] = stepper3_reverse();
    stepper3_order(o);
    r->mot[M_ORDER] = o[0] * 1000 + o[1] * 100 + o[2] * 10 + o[3];
    valve_settings(r->valve);
    memcpy(r->tune, tune, sizeof r->tune);
    r->nsens = 1;   // the one sensor, in slot 0 (0.16.2)
    r->active = 0;
    r->sens[0] = sensor;
    lv_strlcpy(r->name, cname, sizeof r->name);
    r->net_ip = net_ip;
    r->net_mask = net_mask;
    r->remote = remote_on;
    cfg_sens_t *a = &r->sens[0];
    a->has_cal = !cal_is_default();
    if (a->has_cal)
        cal_get_record(&a->cal);
    else
        memset(&a->cal, 0, sizeof a->cal);
}

// The schedule's boundaries: B2 above B1, with the hysteresis bands apart
// (B1 + H below B2 - H), so a set change one way cannot undo itself.
static const char *bounds_why(const int32_t *t)
{
    if ((int64_t)t[T_B1] * (100 + t[T_HYST]) >= (int64_t)t[T_B2] * (100 - t[T_HYST]))
        return "B2 must be above B1, by more than the hysteresis either side";
    if (t[T_B3] != 0 && (int64_t)t[T_B2] * (100 + t[T_HYST]) >= (int64_t)t[T_B3] * (100 - t[T_HYST]))
        return "B3 must be above B2, by more than the hysteresis either side, or 0 for three sets";
    return NULL;
}

// Disable switch: From below To.
static const char *pass_why(const int32_t *t)
{
    return t[T_PLO] < t[T_PHI] ? NULL : "Disable switch: From must be below To";
}

// NULL if every value can be applied, else the first that cannot.
static const char *values_why(const cfg_rec_t *r)
{
    const int32_t *m = r->mot;
    uint8_t o[4], seen = 0;
    if (m[M_RATE] < STEPPER3_RATE_MIN || m[M_RATE] > STEPPER3_RATE_MAX) return "MOT RATE";
    if (m[M_START] < STEPPER3_RATE_MIN || m[M_START] > STEPPER3_RATE_MAX) return "MOT START";
    if (m[M_ACCEL] < STEPPER3_ACCEL_MIN || m[M_ACCEL] > STEPPER3_ACCEL_MAX) return "MOT ACCEL";
    if (m[M_MODE] < STEPPER3_WAVE || m[M_MODE] > STEPPER3_HALF) return "MOT MODE";
    if (m[M_HOLD] < 0 || m[M_HOLD] > 1) return "MOT HOLD";
    if (m[M_DIR] < 0 || m[M_DIR] > 1) return "MOT DIR";
    if (m[M_ORDER] < 0 || m[M_ORDER] > 3333) return "MOT ORDER";
    order_digits(m[M_ORDER], o);
    for (int i = 0; i < 4; i++) {
        if (o[i] > 3 || (seen & (1u << o[i])))
            return "MOT ORDER";
        seen |= 1u << o[i];
    }
    if (valve_settings_check(r->valve))
        return valve_settings_check(r->valve);
    for (int i = 0; i < T_N; i++)
        if (r->tune[i] < tune_lo[i] || r->tune[i] > tune_hi[i] || (i == T_KDIR && r->tune[i] == 0))
            return tune_name[i];
    if (bounds_why(r->tune))
        return bounds_why(r->tune)[1] == '3' ? "B3" : "B2";
    if (pass_why(r->tune))
        return "PASSHI";
    if (r->nsens < 1 || r->nsens > REC_SENS_MAX || r->active >= r->nsens)
        return "SENSOR";
    if (!cfg_name_ok(r->name))
        return "NAME";
    if (cfg_net_why(r->net_ip, r->net_mask))
        return "NET IP";
    if (r->remote > 1)
        return "REMOTE";
    for (uint32_t i = 0; i < r->nsens; i++) {
        const cfg_sens_t *q = &r->sens[i];
        if (q->psi10 < 10 || q->psi10 > 10000 || q->has_cal > 1 || (q->has_cal && !cal_record_ok(&q->cal)))
            return "SENSOR";
        for (uint32_t j = 0; j < i; j++)
            if (r->sens[j].serial == q->serial)
                return "SENSOR";
    }
    return NULL;
}

// A version 1 record as version 8: one sensor, default tuning, the return
// as the back-off.
static void from_v1(const cfg_rec_v1_t *v, cfg_rec_t *r)
{
    memset(r, 0, sizeof *r);
    r->magic = CFG_MAGIC;
    r->version = CFG_VERSION;
    r->size = sizeof *r;
    r->rev = v->rev;
    memcpy(r->mot, v->mot, sizeof r->mot);
    memcpy(r->valve, v->valve, sizeof v->valve);
    r->valve[6] = v->valve[3];
    memcpy(r->tune, tune_def, sizeof r->tune);
    r->nsens = 1;
    r->sens[0].serial = FACTORY_SERIAL;
    r->sens[0].psi10 = FACTORY_PSI10;
    r->sens[0].has_cal = v->has_cal;
    r->sens[0].cal = v->cal;
    lv_strlcpy(r->name, FACTORY_NAME, sizeof r->name);
    r->remote = 1;
}

// A version 2 to 7 record as version 9. They have the first ntune tuning
// values (six to version 6: KD reads 0, the loop as it ran; seven in
// version 7) and none of the fields after crc_at. The values added since
// keep the loop as it ran: sets 2 to 4 as set 1 (the PID table on, always
// from 0.16.20; B1 to B3 and H at their factory values), the closing
// deadband as the deadband.
// Versions 2 to 5 also lack the return (valve[6]): it is their back-off, so
// homing ends where it did. Fields they lack are 0 (the build's address),
// except Remote control, On as the default; versions 5 to 7 have their own.
static void from_old(const void *v, size_t tune_at, int ntune, size_t crc_at, bool has_return, cfg_rec_t *r)
{
    const uint8_t *b = v;
    const size_t old_tune = (size_t)ntune * sizeof(int32_t);
    memset(r, 0, sizeof *r);
    r->remote = 1;                                                 // versions 5 to 7 overwrite it
    memcpy(r, b, tune_at);                                         // header, mot, valve
    memcpy(r->tune, b + tune_at, old_tune);                        // KP to BAND (and KD)
    memcpy(&r->nsens, b + tune_at + old_tune, crc_at - tune_at - old_tune);   // nsens on
    if (!has_return)
        r->valve[6] = r->valve[3];
    for (int s = 0; s < 2; s++) {
        r->tune[T_KP2 + 3 * s] = r->tune[T_KP];
        r->tune[T_TI2 + 3 * s] = r->tune[T_TI];
        r->tune[T_KD2 + 3 * s] = r->tune[T_KD];
    }
    r->tune[T_KP4] = r->tune[T_KP];
    r->tune[T_TI4] = r->tune[T_TI];
    r->tune[T_KD4] = r->tune[T_KD];
    r->tune[T_SCHED] = 2;                                          // on, the sets all set 1: as it ran
    r->tune[T_B1] = tune_def[T_B1];
    r->tune[T_B2] = tune_def[T_B2];
    r->tune[T_B3] = tune_def[T_B3];
    r->tune[T_HYST] = tune_def[T_HYST];
    r->tune[T_DBC] = r->tune[T_DB];
    r->version = CFG_VERSION;
    r->size = sizeof *r;
}

// Version 9 (four PID sets): all kept; the Disable switch edges are set
// after (read_slot).
static void from_v9(const cfg_rec_v9_t *o, cfg_rec_t *r)
{
    memset(r, 0, sizeof *r);
    memcpy(r, o, offsetof(cfg_rec_v9_t, tune));                    // header, mot, valve
    memcpy(r->tune, o->tune, sizeof o->tune);
    memcpy(&r->nsens, &o->nsens, offsetof(cfg_rec_v9_t, crc) - offsetof(cfg_rec_v9_t, nsens));
    r->version = CFG_VERSION;
    r->size = sizeof *r;
}

// Version 8 (three PID sets): its tuning kept, set 4 = set 3 and B3 0 (no
// set 4), so it runs as before until B3 is set (0.16.14).
static void from_v8(const cfg_rec_v8_t *o, cfg_rec_t *r)
{
    memset(r, 0, sizeof *r);
    memcpy(r, o, offsetof(cfg_rec_v8_t, tune));                    // header, mot, valve
    memcpy(r->tune, o->tune, sizeof o->tune);
    memcpy(&r->nsens, &o->nsens, offsetof(cfg_rec_v8_t, crc) - offsetof(cfg_rec_v8_t, nsens));
    r->tune[T_KP4] = r->tune[T_KP3];
    r->tune[T_TI4] = r->tune[T_TI3];
    r->tune[T_KD4] = r->tune[T_KD3];
    r->tune[T_B3] = 0;
    r->version = CFG_VERSION;
    r->size = sizeof *r;
}
_Static_assert(offsetof(cfg_rec_v5_t, tune) == offsetof(cfg_rec_t, valve) + OLD_VALVE_N * 4, "old valve[] then tune");
_Static_assert(offsetof(cfg_rec_v6_t, tune) == offsetof(cfg_rec_t, tune), "v6 has the whole valve[]");
_Static_assert(offsetof(cfg_rec_v7_t, tune) == offsetof(cfg_rec_t, tune), "v7 has the whole valve[]");
_Static_assert(offsetof(cfg_rec_t, nsens) == offsetof(cfg_rec_t, tune) + T_N * 4, "tune then nsens");
_Static_assert(offsetof(cfg_rec_v6_t, crc) - offsetof(cfg_rec_v6_t, tune) - OLD_TUNE_N * 4 ==
               offsetof(cfg_rec_t, crc) - offsetof(cfg_rec_t, nsens), "v6 is v9 from nsens on");
_Static_assert(offsetof(cfg_rec_v7_t, crc) - offsetof(cfg_rec_v7_t, tune) - V7_TUNE_N * 4 ==
               offsetof(cfg_rec_t, crc) - offsetof(cfg_rec_t, nsens), "v7 is v9 from nsens on");
_Static_assert(offsetof(cfg_rec_v8_t, tune) == offsetof(cfg_rec_t, tune), "v8 has the whole valve[]");
_Static_assert(offsetof(cfg_rec_v9_t, tune) == offsetof(cfg_rec_t, tune), "v9 has the whole valve[]");
_Static_assert(offsetof(cfg_rec_v9_t, crc) - offsetof(cfg_rec_v9_t, nsens) ==
               offsetof(cfg_rec_t, crc) - offsetof(cfg_rec_t, nsens), "v9 is v10 from nsens on");
_Static_assert(offsetof(cfg_rec_v8_t, crc) - offsetof(cfg_rec_v8_t, nsens) ==
               offsetof(cfg_rec_t, crc) - offsetof(cfg_rec_t, nsens), "v8 is v9 from nsens on");
_Static_assert(offsetof(cfg_rec_v5_t, crc) - offsetof(cfg_rec_v5_t, tune) ==
               offsetof(cfg_rec_v6_t, crc) - offsetof(cfg_rec_v6_t, tune), "v5 is v6 from tune on");
#define FROM_OLD(T, v, ret, r) from_old((v), offsetof(T, tune), \
                                       (int)(sizeof ((T *)0)->tune / sizeof(int32_t)), offsetof(T, crc), (ret), (r))

// Reads one sector into slot[i]; NULL if usable, else why not.
static const char *read_slot(int i)
{
    static union {                 // one old version per read
        cfg_rec_v1_t v1;
        cfg_rec_v2_t v2;
        cfg_rec_v3_t v3;
        cfg_rec_v4_t v4;
        cfg_rec_v5_t v5;
        cfg_rec_v6_t v6;
        cfg_rec_v7_t v7;
        cfg_rec_v8_t v8;
        cfg_rec_v9_t v9;
    } old;
    cfg_rec_t *r = &slot[i];
    extflash_read(slot_addr[i], r, sizeof *r);
    slot_ver[i] = r->version;
    if (r->magic == 0xFFFFFFFFUL)
        return EMPTY;
    if (r->magic == CFG_MAGIC && r->version == 1 && r->size == sizeof old.v1) {
        memcpy(&old.v1, r, sizeof old.v1);
        if (old.v1.crc != crc32_ieee(&old.v1, offsetof(cfg_rec_v1_t, crc)))
            return "CRC mismatch";
        from_v1(&old.v1, r);
    } else if (r->magic == CFG_MAGIC && r->version == 2 && r->size == sizeof old.v2) {
        memcpy(&old.v2, r, sizeof old.v2);
        if (old.v2.crc != crc32_ieee(&old.v2, offsetof(cfg_rec_v2_t, crc)))
            return "CRC mismatch";
        FROM_OLD(cfg_rec_v2_t, &old.v2, false, r);
        lv_strlcpy(r->name, FACTORY_NAME, sizeof r->name);   // no name before version 3
    } else if (r->magic == CFG_MAGIC && r->version == 3 && r->size == sizeof old.v3) {
        memcpy(&old.v3, r, sizeof old.v3);
        if (old.v3.crc != crc32_ieee(&old.v3, offsetof(cfg_rec_v3_t, crc)))
            return "CRC mismatch";
        FROM_OLD(cfg_rec_v3_t, &old.v3, false, r);
    } else if (r->magic == CFG_MAGIC && r->version == 4 && r->size == sizeof old.v4) {
        memcpy(&old.v4, r, sizeof old.v4);
        if (old.v4.crc != crc32_ieee(&old.v4, offsetof(cfg_rec_v4_t, crc)))
            return "CRC mismatch";
        FROM_OLD(cfg_rec_v4_t, &old.v4, false, r);
    } else if (r->magic == CFG_MAGIC && r->version == 5 && r->size == sizeof old.v5) {
        memcpy(&old.v5, r, sizeof old.v5);
        if (old.v5.crc != crc32_ieee(&old.v5, offsetof(cfg_rec_v5_t, crc)))
            return "CRC mismatch";
        FROM_OLD(cfg_rec_v5_t, &old.v5, false, r);
    } else if (r->magic == CFG_MAGIC && r->version == 6 && r->size == sizeof old.v6) {
        memcpy(&old.v6, r, sizeof old.v6);
        if (old.v6.crc != crc32_ieee(&old.v6, offsetof(cfg_rec_v6_t, crc)))
            return "CRC mismatch";
        FROM_OLD(cfg_rec_v6_t, &old.v6, true, r);
    } else if (r->magic == CFG_MAGIC && r->version == 7 && r->size == sizeof old.v7) {
        memcpy(&old.v7, r, sizeof old.v7);
        if (old.v7.crc != crc32_ieee(&old.v7, offsetof(cfg_rec_v7_t, crc)))
            return "CRC mismatch";
        FROM_OLD(cfg_rec_v7_t, &old.v7, true, r);
    } else if (r->magic == CFG_MAGIC && r->version == 8 && r->size == sizeof old.v8) {
        memcpy(&old.v8, r, sizeof old.v8);
        if (old.v8.crc != crc32_ieee(&old.v8, offsetof(cfg_rec_v8_t, crc)))
            return "CRC mismatch";
        from_v8(&old.v8, r);
    } else if (r->magic == CFG_MAGIC && r->version == 9 && r->size == sizeof old.v9) {
        memcpy(&old.v9, r, sizeof old.v9);
        if (old.v9.crc != crc32_ieee(&old.v9, offsetof(cfg_rec_v9_t, crc)))
            return "CRC mismatch";
        from_v9(&old.v9, r);
    } else if (r->magic != CFG_MAGIC || r->version != CFG_VERSION || r->size != sizeof *r) {
        return "not a settings record of this version";
    } else if (r->crc != rec_crc(r)) {
        return "CRC mismatch";
    }
    slot_offset[i] = r->valve[5];   // the zero offset, removed in 0.13.31
    r->valve[5] = 0;
    // The PID table is always on from 0.16.20: Off (0, set 1 at every
    // pressure) and the setpoint schedule (1, removed in 0.15.20) load as
    // on the pressure, with the sets as saved. Records from before 0.15.15
    // have every set equal to set 1 (from_old), so they run as before.
    if (r->tune[T_SCHED] != 2)
        r->tune[T_SCHED] = 2;
    // Before version 10 the Disable switch band was fixed at 5 to 50 %.
    if (slot_ver[i] < 10) {
        r->tune[T_PLO] = tune_def[T_PLO];
        r->tune[T_PHI] = tune_def[T_PHI];
    }
    if (values_why(r))
        return "value out of range";
    return NULL;
}

static void scan(void)
{
    newest = -1;
    for (int i = 0; i < 2; i++) {
        slot_why[i] = read_slot(i);
        if (!slot_why[i] && (newest < 0 || slot[i].rev > slot[newest].rev))
            newest = i;
    }
}

// What the live settings count as unsaved against.
static const cfg_rec_t *base(void)
{
    return newest >= 0 ? &slot[newest] : &factory;
}

// The sensor in use in a record.
static const cfg_sens_t *rec_sensor(const cfg_rec_t *r) { return &r->sens[r->active]; }

// The same sensor with the same cal. The range is not compared: it is the
// 15 psia Druck's from 0.16.2, whatever an old record holds.
static bool sens_same(const cfg_sens_t *a, const cfg_sens_t *b)
{
    return a->serial == b->serial && a->has_cal == b->has_cal &&
           (!a->has_cal || memcmp(&a->cal, &b->cal, sizeof a->cal) == 0);
}

// Counts the settings in live differing from b. With buf, writes one line
// each for those Review and save has no field for (the calibration, MOT
// MODE / HOLD / ORDER, VALVE CLEAR). Only the sensor in use counts: the
// other slots of an old record are dropped at the next save (0.16.2), which
// the start-up event says, not an unsaved change the screens could discard.
static int diff(const cfg_rec_t *live, const cfg_rec_t *b, char *buf, size_t n)
{
    int count = 0;
    size_t used = 0;
    if (buf && n)
        buf[0] = '\0';
#define LINE(...) do { if (buf && used < n) { lv_snprintf(buf + used, n - used, __VA_ARGS__); used += strlen(buf + used); } } while (0)
    for (int i = 0; i < M_N; i++) {
        if (live->mot[i] == b->mot[i])
            continue;
        count++;
        if (buf && (i == M_MODE || i == M_HOLD || i == M_ORDER))
            LINE("Motor %s\n", mot_name[i]);
    }
    for (int i = 0; i < VALVE_N_SETTINGS; i++) {
        if (live->valve[i] == b->valve[i])
            continue;
        count++;
        if (buf && strcmp(valve_setting_name(i), "CLEAR") == 0)
            LINE("Valve CLEAR\n");
    }
    for (int i = 0; i < T_N; i++) {
        if (live->tune[i] == b->tune[i])
            continue;
        count++;
    }
    if (strcmp(live->name, b->name) != 0) {
        count++;
        LINE("Cryostat name: %s\n", live->name[0] ? live->name : "none");
    }
    if (live->net_ip != b->net_ip || live->net_mask != b->net_mask) {
        char x[2][48];
        count++;
        cfg_net_text(x[0], sizeof x[0], live->net_ip, live->net_mask);
        LINE("IP address: %s\n", x[0]);
    }
    if (live->remote != b->remote) {
        count++;
        LINE("Remote control: %s\n", live->remote ? "On" : "Off");
    }
    if (!sens_same(rec_sensor(live), rec_sensor(b))) {
        count++;
        LINE("Sensor calibration: %s\n", rec_sensor(live)->has_cal ? "new" : "none (nominal)");
    }
#undef LINE
    if (buf && used && buf[used - 1] == '\n')
        buf[used - 1] = '\0';
    return count;
}

int cfg_unsaved(void)
{
    capture(&scratch);
    return diff(&scratch, base(), NULL, 0);
}

int cfg_unsaved_other(char *buf, size_t n)
{
    capture(&scratch);
    diff(&scratch, base(), buf, n);
    return buf[0] != '\0';
}

// The cal counts as saved when it matches the newest saved record.
static void sync_cal_saved(void)
{
    capture(&scratch);
    cal_set_saved(newest >= 0 ? sens_same(rec_sensor(&scratch), rec_sensor(&slot[newest])) : cal_saved());
}

// Why the settings cannot change now, or NULL. In AUTO only the tuning can
// (cfg_save_with, PID).
static const char *busy_why(void)
{
    if (valve_auto()) return "Stop AUTO first";
    if (valve_mode() == VALVE_CALIBRATING || stepper3_busy()) return "wait for the valve to stop";
    return NULL;
}

static bool busy(void) { return busy_why() != NULL; }

// Why r's step mode cannot be put in use now, or NULL: stepper3_set_mode()
// refuses while the valve layer locks the motor (a latched fault).
static const char *mode_why(const cfg_rec_t *r)
{
    return r->mot[M_MODE] != (int32_t)stepper3_mode() ? stepper3_locked() : NULL;
}

// All or nothing: checked first, applied only when idle. False if not applied.
// with_sensor: the record's sensor in use (its serial and cal) too; the
// range stays the 15 psia Druck's (0.16.2).
static bool apply(const cfg_rec_t *r, bool with_sensor)
{
    uint8_t o[4];
    if (values_why(r) || busy() || mode_why(r))
        return false;
    stepper3_set_rate((uint32_t)r->mot[M_RATE]);
    stepper3_set_start((uint32_t)r->mot[M_START]);
    stepper3_set_accel((uint32_t)r->mot[M_ACCEL]);
    stepper3_set_mode((stepper3_mode_t)r->mot[M_MODE]);
    stepper3_set_hold(r->mot[M_HOLD] != 0);
    stepper3_set_reverse(r->mot[M_DIR] != 0);
    order_digits(r->mot[M_ORDER], o);
    stepper3_set_order(o);
    valve_set_settings(r->valve);
    memcpy(tune, r->tune, sizeof tune);
    lv_strlcpy(cname, r->name, sizeof cname);
    net_ip = r->net_ip;
    net_mask = r->net_mask;
    if ((r->remote != 0) != remote_on) {
        remote_on = r->remote != 0;
        valve_event("NET", remote_on ? "Remote control on (settings loaded)" : "Remote control off (settings loaded)");
    }
    if (with_sensor) {
        const cfg_sens_t *a = rec_sensor(r);
        sensor = *a;
        sensor.psi10 = FACTORY_PSI10;
        tell_cal();
        if (a->has_cal)
            cal_set_record(&a->cal, false);
        else
            cal_set_default(false);
    }
    sync_cal_saved();
    return true;
}

static bool verify(uint32_t addr, const void *buf, size_t len)
{
    uint8_t b[64];
    const uint8_t *p = buf;
    for (size_t o = 0; o < len; o += sizeof b) {
        size_t n = len - o < sizeof b ? len - o : sizeof b;
        extflash_read(addr + o, b, n);
        if (memcmp(b, p + o, n) != 0)
            return false;
    }
    return true;
}

/* --------------------------------------------------------------------------
 * API
 * ------------------------------------------------------------------------ */
void cfg_init(void)
{
    loaded = -1;
    memcpy(tune, tune_def, sizeof tune);
    memset(&sensor, 0, sizeof sensor);
    sensor.serial = FACTORY_SERIAL;
    sensor.psi10 = FACTORY_PSI10;
    net_ip = net_mask = 0;
    remote_on = true;
    // The cal found by cal_init() (old record, or nominal) is the factory sensor's.
    tell_cal();
    capture(&factory);
    flash_ok = extflash_init();
    if (!flash_ok) {
        valve_event("CFG", "Serial flash not found, factory defaults");
        return;
    }
    scan();
    for (int i = 0; i < 2; i++)
        if (slot_why[i] && slot_why[i] != EMPTY)
            valve_event("CFG", "Record in %c not used: %s", slot_letter(i), slot_why[i]);
    if (newest < 0) {
        net_boot_ip = net_boot_mask = 0;
        valve_event("CFG", "No saved settings, factory defaults%s",
                    cal_from_old_record() ? ", cal from the old record" : "");
        return;
    }
    apply(&slot[newest], true);   // idle at start-up: cannot be refused
    net_boot_ip = net_ip;
    net_boot_mask = net_mask;
    loaded = newest;
    loaded_rev = slot[newest].rev;
    valve_event("CFG", "Settings saved %s loaded from %c%s", when(&slot[newest]),
                slot_letter(newest), slot_ver[newest] == 1 ? " (version 1 record)" : "");
    if (slot_offset[newest])
        valve_event("CFG", "Zero offset %ld dropped (removed in 0.13.31): 0 %% is the return above the switch",
                    (long)slot_offset[newest]);
    // One sensor per board (0.16.2): what an older record held besides.
    const cfg_rec_t *r = &slot[newest];
    if (r->nsens > 1)
        valve_event("CFG", "%lu sensors in the saved settings: only s/n %lu, the one in use, is kept (one sensor from 0.16.2)",
                    (unsigned long)r->nsens, (unsigned long)rec_sensor(r)->serial);
    if (rec_sensor(r)->psi10 != FACTORY_PSI10)
        valve_event("CFG", "Sensor range %ld psi in the saved settings not used: the 15 psia sensor's (0.16.2)",
                    (long)(rec_sensor(r)->psi10 / 10));
}

// Saves the live settings as the next rev. why is for the screens.
// Writes the record in scratch as the next rev (cfg_save_with fills it with
// the candidate before putting it in use; save() with the live settings).
static bool write_scratch(const char **why)
{
    if (!flash_ok) {
        *why = "serial flash not found, nothing saved";
        return false;
    }
    if (values_why(&scratch) != NULL) {
        *why = "a value is out of range, nothing saved";
        return false;
    }
    scratch.rev = newest >= 0 ? slot[newest].rev + 1 : 1;
    lv_strlcpy(scratch.saved_at, rtclock_iso(), sizeof scratch.saved_at);
    scratch.crc = rec_crc(&scratch);
    int t = newest == 0 ? 1 : 0;
    bool ok = extflash_erase_4k(slot_addr[t]) &&
              extflash_write(slot_addr[t], &scratch, sizeof scratch) &&
              verify(slot_addr[t], &scratch, sizeof scratch);
    if (!ok) {
        scan();
        valve_event("CFG", "Save failed in %c, nothing saved", slot_letter(t));
        *why = "flash write failed, nothing saved";
        return false;
    }
    slot[t] = scratch;
    slot_why[t] = NULL;
    slot_ver[t] = CFG_VERSION;
    newest = t;
    cal_set_saved(true);
    valve_event("CFG", "Settings saved %s in %c", when(&scratch), slot_letter(t));
    return true;
}

static bool save(const char **why)
{
    capture(&scratch);
    return write_scratch(why);
}

bool cfg_save(void)
{
    const char *why;
    return save(&why);
}

/* --------------------------------------------------------------------------
 * Settings by key, for the screens
 * ------------------------------------------------------------------------ */
static int32_t *key_ref(cfg_rec_t *r, cfg_key_t k)
{
    switch (k) {
    case CFG_POSMAX:  return &r->valve[0];
    case CFG_FHOME:   return &r->valve[1];
    case CFG_FCREEP:  return &r->valve[2];
    case CFG_BACKOFF: return &r->valve[3];
    case CFG_RETURN:  return &r->valve[6];
    case CFG_REVERSE: return &r->mot[M_DIR];
    case CFG_RATE:    return &r->mot[M_RATE];
    case CFG_START:   return &r->mot[M_START];
    case CFG_ACCEL:   return &r->mot[M_ACCEL];
    default:
        // The tuning keys run in tune[] order (cfg.h).
        return k >= CFG_KP && k <= CFG_TUNE_LAST ? &r->tune[k - CFG_KP] : NULL;
    }
}
_Static_assert(VALVE_N_SETTINGS == 7, "key_ref() valve indices");

static const cfg_rec_t *source(cfg_from_t from)
{
    switch (from) {
    case CFG_FROM_LIVE:     capture(&scratch); return &scratch;
    case CFG_FROM_SAVED:    return newest >= 0 ? &slot[newest] : NULL;
    case CFG_FROM_PREVIOUS: return newest >= 0 && !slot_why[1 - newest] ? &slot[1 - newest] : NULL;
    case CFG_FROM_DEFAULTS: return &factory;
    }
    return NULL;
}

bool cfg_value(cfg_from_t from, cfg_key_t k, int32_t *v)
{
    // k from the simulator's test hook can be anything (CFG_SENSOR, 27,
    // went in 0.16.2): no key, no value.
    if ((int)k < 0 || k >= CFG_N)
        return false;
    const cfg_rec_t *r = source(from);
    if (!r)
        return false;
    *v = *key_ref((cfg_rec_t *)r, k);
    return true;
}

bool cfg_rev(cfg_from_t from, uint32_t *rev, const char **saved_at)
{
    const cfg_rec_t *r = from == CFG_FROM_SAVED || from == CFG_FROM_PREVIOUS ? source(from) : NULL;
    if (!r)
        return false;
    *rev = r->rev;
    *saved_at = r->saved_at;
    return true;
}

void cfg_range(cfg_key_t k, int32_t *lo, int32_t *hi)
{
    // [5] was the zero offset (removed): 0 only.
    static const int32_t vlo[] = { 1000, 16, 16, 10, 10, 0, 10 }, vhi[] = { 300000, 1000, 1000, 5000, 10000, 0, 5000 };
    _Static_assert(sizeof vlo / sizeof vlo[0] == VALVE_N_SETTINGS, "valve ranges");
    *lo = 0; *hi = 0;
    if (k >= CFG_KP && k <= CFG_TUNE_LAST) {
        *lo = tune_lo[k - CFG_KP]; *hi = tune_hi[k - CFG_KP];
    } else if (k == CFG_RATE || k == CFG_START) {
        *lo = STEPPER3_RATE_MIN; *hi = STEPPER3_RATE_MAX;
    } else if (k == CFG_ACCEL) {
        *lo = STEPPER3_ACCEL_MIN; *hi = STEPPER3_ACCEL_MAX;
    } else if (k == CFG_REVERSE) {
        *hi = 1;
    } else {
        int i = k == CFG_POSMAX ? 0 : k == CFG_FHOME ? 1 : k == CFG_FCREEP ? 2 : k == CFG_BACKOFF ? 3 : 6;
        *lo = vlo[i]; *hi = vhi[i];   // the indices key_ref() uses
    }
}
_Static_assert(CFG_KP + T_N - 1 == CFG_TUNE_LAST, "tuning keys in tune[] order");
_Static_assert(CFG_KD - CFG_KP == T_KD && CFG_KP2 - CFG_KP == T_KP2 && CFG_SCHED - CFG_KP == T_SCHED &&
               CFG_DBC - CFG_KP == T_DBC && CFG_KP4 - CFG_KP == T_KP4 && CFG_B3 - CFG_KP == T_B3 &&
               CFG_PASS_LO - CFG_KP == T_PLO && CFG_PASS_HI - CFG_KP == T_PHI,
               "tuning keys in tune[] order");

int32_t cfg_tune(cfg_key_t k)
{
    return k >= CFG_KP && k <= CFG_TUNE_LAST ? tune[k - CFG_KP] : 0;
}

cfg_key_t cfg_pid_key(int set, int which)
{
    static const cfg_key_t key[4][3] = { { CFG_KP, CFG_TI, CFG_KD }, { CFG_KP2, CFG_TI2, CFG_KD2 },
                                         { CFG_KP3, CFG_TI3, CFG_KD3 }, { CFG_KP4, CFG_TI4, CFG_KD4 } };
    return key[set >= 1 && set <= 4 ? set - 1 : 0][which >= 0 && which <= 2 ? which : 0];
}

static cfg_rec_t cand;   // cfg_save_with and cfg_load_live (never at once)

bool cfg_save_with(const int32_t v[CFG_N], const bool set[CFG_N], const char **why)
{
    // In AUTO only the tuning may change (the loop picks it up at its next
    // step); anything else waits for MANUAL.
    bool tune_only = valve_auto();
    if (!tune_only && (*why = busy_why()) != NULL) return false;
    capture(&cand);
    for (int k = 0; k < CFG_N; k++) {
        if (!set[k])
            continue;
        if (tune_only && (k < CFG_KP || k > CFG_TUNE_LAST)) {
            // cand holds the live values here (only tuning keys are written).
            if (v[k] != *key_ref(&cand, (cfg_key_t)k)) { *why = "Stop AUTO first"; return false; }
            continue;
        }
        int32_t lo, hi;
        cfg_range((cfg_key_t)k, &lo, &hi);
        if (v[k] < lo || v[k] > hi) { *why = "a value is out of range, nothing saved"; return false; }
        *key_ref(&cand, (cfg_key_t)k) = v[k];
    }
    if (cand.mot[M_START] > cand.mot[M_RATE]) { *why = "Start speed must not exceed Run speed"; return false; }
    if ((*why = bounds_why(cand.tune)) != NULL) return false;
    if ((*why = pass_why(cand.tune)) != NULL) return false;
    if (values_why(&cand)) { *why = "a value is out of range, nothing saved"; return false; }
    if (!tune_only && (*why = mode_why(&cand)) != NULL) return false;
    // Written first, then put in use: a failed write changes nothing, so
    // "Not saved" also means "not in use" (review of 0.10.11). The cal was
    // captured into cand.
    scratch = cand;
    if (!write_scratch(why))
        return false;
    if (tune_only) {
        memcpy(tune, cand.tune, sizeof tune);
        sync_cal_saved();
        return true;
    }
    if (!apply(&cand, true)) {
        // Not reached in practice: busy() was checked above and nothing ran since.
        valve_event("CFG", "Saved but not put in use (busy)");
        *why = "saved, but not in use until a restart";
        return false;
    }
    return true;
}

bool cfg_load_live(cfg_from_t from, const char **why)
{
    const cfg_rec_t *r = source(from);
    if (!r || from == CFG_FROM_LIVE) { *why = "no such save"; return false; }
    if ((*why = busy_why()) != NULL) return false;
    // Records before version 4 (saved by 0.12.12 or earlier) span two unit
    // changes, 0.11.19 (the Direction bit's meaning flipped) and 0.11.33
    // (HALF settings in full steps), and cannot tell which side they are
    // from. The one the board started on is how it has been running; any
    // other is not loaded, so the motor cannot turn the other way.
    int i = r == &slot[0] ? 0 : r == &slot[1] ? 1 : -1;
    if (i >= 0 && slot_ver[i] < 4 && !(i == loaded && slot[i].rev == loaded_rev)) {
        *why = "saved before 0.12.13, its Direction cannot be read";
        return false;
    }
    if (from == CFG_FROM_DEFAULTS) {
        cfg_rec_t *d = &cand;   // shared, saves a record's worth of RAM
        capture(d);   // keep the sensor and its cal
        memcpy(d->mot, factory.mot, sizeof d->mot);
        memcpy(d->valve, factory.valve, sizeof d->valve);
        memcpy(d->tune, factory.tune, sizeof d->tune);
        r = d;
    }
    if ((*why = mode_why(r)) != NULL) return false;
    if (!apply(r, true)) { *why = "record not applied"; return false; }
    return true;
}

/* --------------------------------------------------------------------------
 * The sensor's saved cal (Calibrate sensor: the Saved cal reading)
 * ------------------------------------------------------------------------ */
bool cfg_saved_cal(const cal_rec_t **cal)
{
    if (newest < 0)
        return false;
    const cfg_sens_t *sv = rec_sensor(&slot[newest]);
    *cal = sv->has_cal ? &sv->cal : NULL;
    return true;
}

/* --------------------------------------------------------------------------
 * Commands
 * ------------------------------------------------------------------------ */

bool cfg_name_ok(const char *n)
{
    for (size_t i = 0; i <= CFG_NAME_LEN; i++) {
        if (n[i] == '\0')
            return true;
        if (n[i] < 0x20 || n[i] > 0x7E)
            return false;
    }
    return false;   // too long
}

const char *cfg_name(void) { return cname; }

bool cfg_set_name(const char *n, const char **why)
{
    if (!cfg_name_ok(n)) {
        *why = "name: up to 12 letters, digits, spaces or signs";
        return false;
    }
    lv_strlcpy(cname, n, sizeof cname);
    return true;
}

/* --------------------------------------------------------------------------
 * Ethernet address (0.12.13, Ryan 26 Oct, as the fallback for DHCP; the only
 * address from 0.14.9, no DHCP). Set live and unsaved until a save, like the name; the link takes
 * the one loaded at start-up, so a change applies after the next restart.
 * 0 0 is the build's PMC_IP and PMC_MASK.
 * ------------------------------------------------------------------------ */
bool cfg_net_parse(const char *s, uint32_t *v)
{
    uint32_t a = 0;
    for (int i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9')
            return false;
        uint32_t o = 0;
        for (int d = 0; *s >= '0' && *s <= '9'; d++, s++) {
            if (d == 3)
                return false;
            o = o * 10 + (uint32_t)(*s - '0');
        }
        if (o > 255 || (i < 3 && *s++ != '.'))
            return false;
        a = a << 8 | o;
    }
    *v = a;
    return *s == '\0';
}

const char *cfg_net_why(uint32_t ip, uint32_t mask)
{
    if (ip == 0 && mask == 0)
        return NULL;   // the build default
    uint32_t inv = ~mask;
    if ((inv & (inv + 1)) != 0 || mask < 0xFF000000UL || mask > 0xFFFFFFFCUL)
        return "mask must be 255.0.0.0 to 255.255.255.252, ones then zeros";
    uint32_t top = ip >> 24;
    if (top == 0 || top == 127 || top >= 224)
        return "not a device address";
    if ((ip & inv) == 0 || (ip & inv) == inv)
        return "that is the network or broadcast address of the mask";
    return NULL;
}

static void net_addr(char *buf, size_t n, uint32_t a)
{
    lv_snprintf(buf, n, "%lu.%lu.%lu.%lu", (unsigned long)(a >> 24), (unsigned long)(a >> 16 & 255),
                (unsigned long)(a >> 8 & 255), (unsigned long)(a & 255));
}

const char *cfg_net_text(char *buf, size_t n, uint32_t ip, uint32_t mask)
{
    char a[16], m[16];
    if (ip == 0 && mask == 0) {
        lv_snprintf(buf, n, "%s %s (build default)", PMC_IP, PMC_MASK);
        return buf;
    }
    net_addr(a, sizeof a, ip);
    net_addr(m, sizeof m, mask);
    lv_snprintf(buf, n, "%s %s", a, m);
    return buf;
}

void cfg_net(uint32_t *ip, uint32_t *mask) { *ip = net_ip; *mask = net_mask; }

bool cfg_set_net(uint32_t ip, uint32_t mask, const char **why)
{
    if ((*why = cfg_net_why(ip, mask)) != NULL)
        return false;
    net_ip = ip;
    net_mask = mask;
    return true;
}

void cfg_net_boot(char ip[16], char mask[16])
{
    if (net_boot_ip == 0 && net_boot_mask == 0) {
        lv_strlcpy(ip, PMC_IP, 16);
        lv_strlcpy(mask, PMC_MASK, 16);
    } else {
        net_addr(ip, 16, net_boot_ip);
        net_addr(mask, 16, net_boot_mask);
    }
}

bool cfg_net_restart_due(void) { return net_ip != net_boot_ip || net_mask != net_boot_mask; }

/* --------------------------------------------------------------------------
 * Remote control (0.13.4, Ryan 28 Oct, network plan A): while Off, set
 * commands over the network (port 8888, STOP included) and Modbus writes are
 * refused; queries and reads are not. Live at once, kept by a save.
 * ------------------------------------------------------------------------ */
bool cfg_remote(void) { return remote_on; }

void cfg_set_remote(bool on)
{
    if (on == remote_on)
        return;
    remote_on = on;
    valve_event("NET", on ? "Remote control on (screen): the network can set" :
                            "Remote control off (screen): the network can only read");
}

