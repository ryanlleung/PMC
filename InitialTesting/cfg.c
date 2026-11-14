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
 * rev is the newest.
 *
 * Version 11 (0.16.30, Ryan 13 Nov, after the review of what the firmware
 * does not need) is the layout below: what the firmware uses, and nothing
 * kept for an older layout. Against version 10 it has no step mode (full
 * step only), no zero offset slot, no Pressure response (KDIR) and no
 * schedule type (the PID table is always on), one sensor with the two
 * points of its bench cal, and no sensor serial or range (nothing showed
 * the serial; the range is the 15 psia Druck's).
 *
 * Version 10 records (0.16.21 to 0.16.29) are read and converted
 * (from_v10); the first save writes version 11 to the other sector, so an
 * older firmware flashed back still finds its version 10 record. Versions 1
 * to 9 (0.6.0 to 0.16.20) are no longer read: a board that still holds one
 * starts on the factory defaults and says so in the events. Flash 0.16.29
 * and save once first to carry such a record over.
 * ------------------------------------------------------------------------ */
#define CFG_ADDR_A   0x7FD000UL
#define CFG_ADDR_B   0x7FE000UL
#define CFG_MAGIC    0x47464350UL   // "PCFG"
#define CFG_VERSION  11

enum { M_RATE, M_START, M_ACCEL, M_HOLD, M_DIR, M_ORDER, M_N };
enum { V_POSMAX, V_FHOME, V_FCREEP, V_BACKOFF, V_CLEAR, V_RETURN };   // valve.c settings[]
_Static_assert(VALVE_N_SETTINGS == V_RETURN + 1, "valve[] indices");

// Control tuning. Fixed point as noted below. KP, TI and KD are PID set 1;
// DB is the deadband, both ways (a separate closing one, DBC, to 0.16.31).
enum { T_KP, T_TI, T_OUTMIN, T_DB, T_BAND, T_KD,
       T_KP2, T_TI2, T_KD2, T_KP3, T_TI3, T_KD3, T_B1, T_B2, T_HYST,
       T_KP4, T_TI4, T_KD4, T_B3, T_PLO, T_PHI, T_N };
static const char *const tune_name[T_N] = { "KP", "TI", "OUTMIN", "DB", "BAND", "KD",
                                            "KP2", "TI2", "KD2", "KP3", "TI3", "KD3", "B1", "B2", "HYST",
                                            "KP4", "TI4", "KD4", "B3", "PASSLO", "PASSHI" };
// Factory (0.16.18, Ryan 12 Nov: "set factory pid settings to boundary at 6
// 10 14. kp ti kd values, 0.06, 0.6, 0; 0.12 0.82 0; 0.8 0.94 0, 0.4 0.25
// 0"): B1 6, B2 10, B3 14 mbar, 15 % hysteresis; set 1 KP 0.06 TI 0.60,
// set 2 0.12/0.82, set 3 0.80/0.94, set 4 0.40/0.25, KD 0 (the bench step
// tests of 12 Nov).
static const int32_t tune_def[T_N] = { 6, 60, 0, 8, 10, 0,
                                       12, 82, 0, 80, 94, 0, 60, 100, 15,
                                       40, 25, 0, 140, 5, 50 };
static const int32_t tune_lo[T_N]  = { 1,     0,     0,   0,    0,    0,
                                       1,     0,     0,     1,     0,     0,     1,     1,     0,
                                       1,     0,     0,     0,     0,  1 };
static const int32_t tune_hi[T_N]  = { 10000, 10000, 500, 2000, 1000, 10000,
                                       10000, 10000, 10000, 10000, 10000, 10000, 10000, 10000, 40,
                                       10000, 10000, 10000, 10000, 99, 100 };
static int32_t tune[T_N];   // cfg_init: tune_def
// Fixed point: 0.01 %/mbar, 0.01 min, 0.1 %, steps, 0.01 mbar (the band
// from 0, 0.10 before 0.16.16, Ryan 12 Nov), 0.01 % per mbar/s; sets 2 to 4
// as set 1; B1 to B3 0.1 mbar (B3 0: no set 4, three sets); hysteresis % of
// the boundary; Disable switch From 0 to 99 and To
// 1 to 100 % open (0.16.22, Ryan 13 Nov: "extend the limit switch ignore to
// 0 to 100% inputtable"), From below To.

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t size;               // sizeof(cfg_rec_t)
    uint32_t rev;                // 1 for the first save, +1 each save
    int32_t mot[M_N];            // ORDER as four digits, e.g. 213 for 0213
    int32_t valve[VALVE_N_SETTINGS];
    int32_t tune[T_N];
    uint32_t has_cal;            // 0: nominal over the range
    cal_rec_t cal;
    char saved_at[20];           // "YYYY-MM-DD HH:MM:SS", "" if the clock was not set
    char name[CFG_NAME_LEN + 4]; // cryostat name, NUL-terminated
    uint32_t net_ip, net_mask;   // Ethernet address, a.b.c.d as a << 24 | ...; 0 0 = build default
    uint32_t remote;             // Remote control: 0 Off, 1 On
    uint32_t crc;
} cfg_rec_t;

/* Version 10 (0.16.21 to 0.16.29), read only. mot[]: RATE, START, ACCEL,
 * MODE, HOLD, DIR, ORDER. valve[]: POSMAX, FHOME, FCREEP, BACKOFF, CLEAR,
 * the zero offset (0 since 0.13.31), RETURN. tune[]: KP, TI, OUTMIN, DB,
 * KDIR, BAND, KD, KP2, TI2, KD2, KP3, TI3, KD3, SCHED, B1, B2, HYST, DBC,
 * KP4, TI4, KD4, B3, PASSLO, PASSHI. Four sensor slots, one in use. */
typedef struct {
    uint32_t serial;
    int32_t psi10;
    uint32_t has_cal;
    cal_rec_v4_t cal;
} cfg_sens_v10_t;
typedef struct {
    uint32_t magic, version, size, rev;
    int32_t mot[7];
    int32_t valve[7];
    int32_t tune[24];
    uint32_t nsens, active;
    cfg_sens_v10_t sens[4];
    char saved_at[20];
    char name[CFG_NAME_LEN + 4];
    uint32_t net_ip, net_mask;
    uint32_t remote;
    uint32_t crc;
} cfg_rec_v10_t;
_Static_assert(sizeof(cfg_rec_v10_t) == 1508, "the version 10 record as 0.16.29 wrote it");

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
static cfg_rec_t slot[2];        // as read from each sector (a version 10 record converted)
static const char *slot_why[2];  // NULL if valid, else why not
static uint32_t slot_ver[2];     // version found in each sector
static uint32_t slot_note[2];    // NOTE_ bits: what a version 10 record held that is not kept
static int newest = -1;          // slot with the newest valid record, -1 none
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
// "with no date" if the clock was not set.
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

int32_t cfg_sensor_range_mmbar(void) { return CAL_FS_MMBAR; }

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
    r->mot[M_HOLD] = stepper3_hold();
    r->mot[M_DIR] = stepper3_reverse();
    stepper3_order(o);
    r->mot[M_ORDER] = o[0] * 1000 + o[1] * 100 + o[2] * 10 + o[3];
    valve_settings(r->valve);
    memcpy(r->tune, tune, sizeof r->tune);
    lv_strlcpy(r->name, cname, sizeof r->name);
    r->net_ip = net_ip;
    r->net_mask = net_mask;
    r->remote = remote_on;
    r->has_cal = !cal_is_default();
    if (r->has_cal)
        cal_get_record(&r->cal);
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
        if (r->tune[i] < tune_lo[i] || r->tune[i] > tune_hi[i])
            return tune_name[i];
    if (bounds_why(r->tune))
        return bounds_why(r->tune)[1] == '3' ? "B3" : "B2";
    if (pass_why(r->tune))
        return "PASSHI";
    if (!cfg_name_ok(r->name))
        return "NAME";
    if (cfg_net_why(r->net_ip, r->net_mask))
        return "NET IP";
    if (r->remote > 1)
        return "REMOTE";
    if (r->has_cal > 1 || (r->has_cal && !cal_record_ok(&r->cal)))
        return "SENSOR";
    return NULL;
}

// What a version 10 record held that version 11 does not keep: said in the
// events when that record is the one loaded (cfg_init).
enum { NOTE_MODE = 1, NOTE_KDIR = 2, NOTE_SENSORS = 4, NOTE_CAL = 8, NOTE_DBC = 16 };

// A version 10 record as version 11, value by value. NULL, or why not.
static const char *from_v10(const cfg_rec_v10_t *o, cfg_rec_t *r, uint32_t *notes)
{
    // tune[] of version 10 without KDIR (4), SCHED (13) and DBC (17).
    static const uint8_t from[T_N] = { 0, 1, 2, 3, 5, 6, 7, 8, 9, 10, 11, 12, 14, 15, 16, 18, 19, 20, 21, 22, 23 };
    memset(r, 0, sizeof *r);
    r->magic = CFG_MAGIC;
    r->version = CFG_VERSION;
    r->size = sizeof *r;
    r->rev = o->rev;
    r->mot[M_RATE] = o->mot[0];
    r->mot[M_START] = o->mot[1];
    r->mot[M_ACCEL] = o->mot[2];
    if (o->mot[3] != 1)                  // 0 WAVE, 1 FULL, 2 HALF; the settings were in full steps in each
        *notes |= NOTE_MODE;
    r->mot[M_HOLD] = o->mot[4];
    r->mot[M_DIR] = o->mot[5];
    r->mot[M_ORDER] = o->mot[6];
    for (int i = V_POSMAX; i <= V_CLEAR; i++)
        r->valve[i] = o->valve[i];
    r->valve[V_RETURN] = o->valve[6];    // [5] was the zero offset, 0 since 0.13.31
    for (int i = 0; i < T_N; i++)
        r->tune[i] = o->tune[from[i]];
    if (o->tune[4] != 1)                 // KDIR -1: opening lowered the pressure
        *notes |= NOTE_KDIR;
    if (o->tune[17] != o->tune[3])       // a closing deadband of its own: the opening one is kept (0.16.32)
        *notes |= NOTE_DBC;
    if (o->nsens < 1 || o->nsens > 4 || o->active >= o->nsens)
        return "value out of range";
    if (o->nsens > 1)
        *notes |= NOTE_SENSORS;
    const cfg_sens_v10_t *q = &o->sens[o->active];
    if (q->has_cal) {
        if (cal_from_v4(&q->cal, &r->cal))
            r->has_cal = 1;
        else
            *notes |= NOTE_CAL;
    }
    memcpy(r->saved_at, o->saved_at, sizeof r->saved_at);
    r->saved_at[sizeof r->saved_at - 1] = '\0';
    memcpy(r->name, o->name, sizeof r->name);
    r->name[sizeof r->name - 1] = '\0';
    r->net_ip = o->net_ip;
    r->net_mask = o->net_mask;
    r->remote = o->remote;
    return NULL;
}
_Static_assert(sizeof ((cfg_rec_t *)0)->saved_at == sizeof ((cfg_rec_v10_t *)0)->saved_at &&
               sizeof ((cfg_rec_t *)0)->name == sizeof ((cfg_rec_v10_t *)0)->name, "from_v10 copies these whole");

// Reads one sector into slot[i]; NULL if usable, else why not.
static const char *read_slot(int i)
{
    static cfg_rec_v10_t old;      // a version 10 record, while it is converted
    cfg_rec_t *r = &slot[i];
    uint32_t head[3];              // magic, version, size
    slot_note[i] = 0;
    extflash_read(slot_addr[i], head, sizeof head);
    slot_ver[i] = head[1];
    if (head[0] == 0xFFFFFFFFUL)
        return EMPTY;
    if (head[0] != CFG_MAGIC)
        return "not a settings record";
    if (head[1] == 10 && head[2] == sizeof old) {
        extflash_read(slot_addr[i], &old, sizeof old);
        if (old.crc != crc32_ieee(&old, offsetof(cfg_rec_v10_t, crc)))
            return "CRC mismatch";
        const char *why = from_v10(&old, r, &slot_note[i]);
        if (why)
            return why;
    } else if (head[1] >= 1 && head[1] < 10) {
        return "saved before 0.16.21, no longer read";
    } else if (head[1] != CFG_VERSION || head[2] != sizeof *r) {
        return "not a settings record of this version";
    } else {
        extflash_read(slot_addr[i], r, sizeof *r);
        if (r->crc != rec_crc(r))
            return "CRC mismatch";
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

// The same cal in both records.
static bool cal_same(const cfg_rec_t *a, const cfg_rec_t *b)
{
    return a->has_cal == b->has_cal && (!a->has_cal || memcmp(&a->cal, &b->cal, sizeof a->cal) == 0);
}

// Counts the settings in live differing from b.
static int diff(const cfg_rec_t *live, const cfg_rec_t *b)
{
    int count = 0;
    for (int i = 0; i < M_N; i++)
        count += live->mot[i] != b->mot[i];
    for (int i = 0; i < VALVE_N_SETTINGS; i++)
        count += live->valve[i] != b->valve[i];
    for (int i = 0; i < T_N; i++)
        count += live->tune[i] != b->tune[i];
    count += strcmp(live->name, b->name) != 0;
    count += live->net_ip != b->net_ip || live->net_mask != b->net_mask;
    count += live->remote != b->remote;
    count += !cal_same(live, b);
    return count;
}

int cfg_unsaved(void)
{
    capture(&scratch);
    return diff(&scratch, base());
}

// The cal counts as saved when it matches the newest saved record.
static void sync_cal_saved(void)
{
    capture(&scratch);
    cal_set_saved(newest >= 0 ? cal_same(&scratch, &slot[newest]) : cal_saved());
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

// All or nothing: checked first, applied only when idle. False if not applied.
static bool apply(const cfg_rec_t *r)
{
    uint8_t o[4];
    if (values_why(r) || busy())
        return false;
    stepper3_set_rate((uint32_t)r->mot[M_RATE]);
    stepper3_set_start((uint32_t)r->mot[M_START]);
    stepper3_set_accel((uint32_t)r->mot[M_ACCEL]);
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
    if (r->has_cal)
        cal_set_record(&r->cal, false);
    else
        cal_set_default(false);
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
    memcpy(tune, tune_def, sizeof tune);
    net_ip = net_mask = 0;
    remote_on = true;
    capture(&factory);   // with the nominal cal (cal_init)
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
        valve_event("CFG", "No saved settings, factory defaults");
        return;
    }
    apply(&slot[newest]);   // idle at start-up: cannot be refused
    net_boot_ip = net_ip;
    net_boot_mask = net_mask;
    valve_event("CFG", "Settings saved %s loaded from %c%s", when(&slot[newest]),
                slot_letter(newest), slot_ver[newest] == 10 ? " (saved by 0.16.29 or earlier)" : "");
    // What a version 10 record held that is not kept (0.16.30).
    const uint32_t n = slot_note[newest];
    if (n & NOTE_MODE)
        valve_event("CFG", "Step mode in the saved settings not kept: full step only");
    if (n & NOTE_KDIR)
        valve_event("CFG", "Pressure response Reversed in the saved settings not kept: opening raises the pressure");
    if (n & NOTE_DBC)
        valve_event("CFG", "Deadband close in the saved settings not kept: one deadband, %ld steps", (long)tune[T_DB]);
    if (n & NOTE_SENSORS)
        valve_event("CFG", "Several sensors in the saved settings: only the one in use is kept");
    if (n & NOTE_CAL)
        valve_event("CFG", "Saved calibration is not a two-point one: not loaded, calibrate the sensor");
}

// Writes the record in scratch as the next rev (cfg_save_with fills it with
// the candidate before putting it in use). why is for the screens.
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

/* --------------------------------------------------------------------------
 * Settings by key, for the screens
 * ------------------------------------------------------------------------ */
static int32_t *key_ref(cfg_rec_t *r, cfg_key_t k)
{
    switch (k) {
    case CFG_POSMAX:  return &r->valve[V_POSMAX];
    case CFG_FHOME:   return &r->valve[V_FHOME];
    case CFG_FCREEP:  return &r->valve[V_FCREEP];
    case CFG_BACKOFF: return &r->valve[V_BACKOFF];
    case CFG_RETURN:  return &r->valve[V_RETURN];
    case CFG_REVERSE: return &r->mot[M_DIR];
    case CFG_RATE:    return &r->mot[M_RATE];
    case CFG_START:   return &r->mot[M_START];
    case CFG_ACCEL:   return &r->mot[M_ACCEL];
    default:
        // The tuning keys run in tune[] order (cfg.h).
        return k >= CFG_KP && k <= CFG_TUNE_LAST ? &r->tune[k - CFG_KP] : NULL;
    }
}

static const cfg_rec_t *source(cfg_from_t from)
{
    switch (from) {
    case CFG_FROM_LIVE:     capture(&scratch); return &scratch;
    case CFG_FROM_SAVED:    return newest >= 0 ? &slot[newest] : NULL;
    }
    return NULL;
}

bool cfg_value(cfg_from_t from, cfg_key_t k, int32_t *v)
{
    // k from the simulator's test hook can be anything: no key, no value.
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
    const cfg_rec_t *r = from == CFG_FROM_SAVED ? source(from) : NULL;
    if (!r)
        return false;
    *rev = r->rev;
    *saved_at = r->saved_at;
    return true;
}

void cfg_range(cfg_key_t k, int32_t *lo, int32_t *hi)
{
    static const int32_t vlo[] = { 1000, 16, 16, 10, 10, 10 }, vhi[] = { 300000, 1000, 1000, 5000, 10000, 5000 };
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
        int i = k == CFG_POSMAX ? V_POSMAX : k == CFG_FHOME ? V_FHOME : k == CFG_FCREEP ? V_FCREEP :
                k == CFG_BACKOFF ? V_BACKOFF : V_RETURN;
        *lo = vlo[i]; *hi = vhi[i];
    }
}
_Static_assert(CFG_KP + T_N - 1 == CFG_TUNE_LAST, "tuning keys in tune[] order");
_Static_assert(CFG_KD - CFG_KP == T_KD && CFG_KP2 - CFG_KP == T_KP2 && CFG_B1 - CFG_KP == T_B1 &&
               CFG_KP4 - CFG_KP == T_KP4 && CFG_B3 - CFG_KP == T_B3 &&
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

static cfg_rec_t cand;   // cfg_save_with, cfg_reset_factory and cfg_save_system (never at once)

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
    if (!apply(&cand)) {
        // Not reached in practice: busy() was checked above and nothing ran since.
        valve_event("CFG", "Saved but not put in use (busy)");
        *why = "saved, but not in use until a restart";
        return false;
    }
    return true;
}

bool cfg_undo(const char **why)
{
    if ((*why = busy_why()) != NULL) return false;
    if (!apply(base())) { *why = "record not applied"; return false; }
    valve_event("CFG", newest >= 0 ? "Undo: settings saved %s back in use" : "Undo: start-up settings back in use",
                when(base()));
    return true;
}

bool cfg_reset_factory(const char **why)
{
    if ((*why = busy_why()) != NULL) return false;
    capture(&cand);   // the calibration in use, the name, the address and Remote control stay
    memcpy(cand.mot, factory.mot, sizeof cand.mot);
    memcpy(cand.valve, factory.valve, sizeof cand.valve);
    memcpy(cand.tune, factory.tune, sizeof cand.tune);
    // Written first, then put in use, as cfg_save_with.
    scratch = cand;
    if (!write_scratch(why))
        return false;
    if (!apply(&cand)) { *why = "saved, but not in use until a restart"; return false; }
    valve_event("CFG", "Reset to factory: motor, valve and PID settings");
    return true;
}

bool cfg_save_system(const char **why)
{
    cand = *base();
    cand.magic = CFG_MAGIC;
    cand.version = CFG_VERSION;
    cand.size = sizeof cand;
    lv_strlcpy(cand.name, cname, sizeof cand.name);
    cand.net_ip = net_ip;
    cand.net_mask = net_mask;
    cand.remote = remote_on;
    scratch = cand;
    if (!write_scratch(why))
        return false;
    sync_cal_saved();   // write_scratch marks the cal saved: only if it is the saved one
    return true;
}

/* --------------------------------------------------------------------------
 * The sensor's saved cal (Calibrate sensor: the Saved cal reading)
 * ------------------------------------------------------------------------ */
bool cfg_saved_cal(const cal_rec_t **cal)
{
    if (newest < 0)
        return false;
    *cal = slot[newest].has_cal ? &slot[newest].cal : NULL;
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

