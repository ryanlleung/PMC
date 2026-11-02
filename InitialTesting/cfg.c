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
 * Version 6 (0.13.6) adds the homing return (valve[6], RETURN: steps off
 * the switch after the slow close); older records load it as their
 * back-off, which homing used for that step until then.
 *
 * Version 5 (0.13.4) adds the Remote control switch (0 Off, 1 On); older
 * records load with it Off.
 *
 * Version 4 (0.12.13) adds the Ethernet fallback address and mask (0 for
 * the build's PMC_FALLBACK_IP); version 3 records load with 0.
 *
 * Version 3 (0.10.9) adds the cryostat name; version 2 records load with
 * the default name.
 *
 * Version 2 (0.9.0) adds the control tuning, the sensors on the board (each
 * with its own cal) and the save time. Version 1 records (0.6.0 to 0.8.x)
 * still load, as one sensor (the bench Druck) with default tuning; the
 * first save writes version 2 to the other sector, so an older firmware
 * flashed back still finds the version 1 record as its newest.
 * ------------------------------------------------------------------------ */
#define CFG_ADDR_A   0x7FD000UL
#define CFG_ADDR_B   0x7FE000UL
#define CFG_MAGIC    0x47464350UL   // "PCFG"
#define CFG_VERSION  6
#define OLD_VALVE_N  6       // valve[] in records before version 6

enum { M_RATE, M_START, M_ACCEL, M_MODE, M_HOLD, M_DIR, M_ORDER, M_N };
static const char *const mot_name[M_N] = { "RATE", "START", "ACCEL", "MODE", "HOLD", "DIR", "ORDER" };

// Control tuning, used from stage 8 (AUTO). Fixed point as noted.
enum { T_KP, T_TI, T_OUTMIN, T_DB, T_KDIR, T_BAND, T_N };
static const char *const tune_name[T_N] = { "KP", "TI", "OUTMIN", "DB", "KDIR", "BAND" };
static const int32_t tune_def[T_N] = { 400, 25, 0, 8, 1, 10 };
static const int32_t tune_lo[T_N]  = { 1,     0,     0,   0,    -1, 10 };
static const int32_t tune_hi[T_N]  = { 10000, 10000, 500, 2000,  1, 1000 };
static int32_t tune[T_N] = { 400, 25, 0, 8, 1, 10 };
// Fixed point: 0.01 %/mbar, 0.01 min, 0.1 %, steps, +-1, 0.01 mbar.
static const int tune_dec[T_N] = { 2, 2, 1, 0, 0, 2 };
static const char *const tune_unit[T_N] = { " %/mbar", " min", " %", " steps", "", " mbar" };

// One tuning value as PID? prints it, without the unit.
static const char *tune_fmt(char *buf, size_t n, int i, int32_t v)
{
    if (i == T_KDIR) {
        lv_snprintf(buf, n, "%s", v > 0 ? "+1" : "-1");
        return buf;
    }
    return fixed_fmt(buf, n, v, tune_dec[i]);
}

#if PMC_ENG_COMMANDS
static const char *tune_text(char *buf, size_t n, int i)
{
    return tune_fmt(buf, n, i, tune[i]);
}
#endif

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
    uint32_t nsens;              // 1 to CFG_SENS_MAX
    uint32_t active;             // index of the sensor in use
    cfg_sens_t sens[CFG_SENS_MAX];
    char saved_at[20];           // "YYYY-MM-DD HH:MM:SS", "" if the clock was not set
    char name[CFG_NAME_LEN + 4]; // cryostat name, NUL-terminated (version 3)
    uint32_t net_ip, net_mask;   // fallback address, a.b.c.d as a << 24 | ...; 0 0 = build default (version 4)
    uint32_t remote;             // Remote control: 0 Off, 1 On (version 5)
    uint32_t crc;
} cfg_rec_t;

// Version 5 (0.13.4 and 0.13.5), read only: version 6 with one valve
// setting fewer.
typedef struct {
    uint32_t magic, version, size, rev;
    int32_t mot[M_N];
    int32_t valve[OLD_VALVE_N];
    int32_t tune[T_N];
    uint32_t nsens, active;
    cfg_sens_t sens[CFG_SENS_MAX];
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
    int32_t tune[T_N];
    uint32_t nsens, active;
    cfg_sens_t sens[CFG_SENS_MAX];
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
    int32_t tune[T_N];
    uint32_t nsens, active;
    cfg_sens_t sens[CFG_SENS_MAX];
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
    int32_t tune[T_N];
    uint32_t nsens, active;
    cfg_sens_t sens[CFG_SENS_MAX];
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
#define FACTORY_PSI10   150

// Live sensor list. The cal of the sensor in use lives in cal.c.
static uint32_t nsens = 1, active_sens;
static cfg_sens_t sens[CFG_SENS_MAX] = { { FACTORY_SERIAL, FACTORY_PSI10, 0, { 0 } } };
// Live cryostat name, in the header ("CF-11 pressure controller").
#define FACTORY_NAME "CF-11"
static char cname[CFG_NAME_LEN + 1] = FACTORY_NAME;
// Live fallback address (unsaved until a save, like the name), and the one
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

int32_t cfg_psi10_mmbar(int32_t psi10)
{
    return (int32_t)(((int64_t)psi10 * 6894757 + 500) / 1000);   // 68.94757 mbar/psi
}

static void serial_text(char *buf, size_t n, uint32_t serial)
{
    lv_snprintf(buf, n, "%lu", (unsigned long)serial);
}

// Tells cal.c which sensor is in use (bench cal id, nominal range).
static void tell_cal(void)
{
    char sn[12];
    serial_text(sn, sizeof sn, sens[active_sens].serial);
    cal_set_sensor(sn, cfg_psi10_mmbar(sens[active_sens].psi10));
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
    r->nsens = nsens;
    r->active = active_sens;
    memcpy(r->sens, sens, sizeof r->sens);
    lv_strlcpy(r->name, cname, sizeof r->name);
    r->net_ip = net_ip;
    r->net_mask = net_mask;
    r->remote = remote_on;
    cfg_sens_t *a = &r->sens[active_sens];
    a->has_cal = !cal_is_default();
    if (a->has_cal)
        cal_get_record(&a->cal);
    else
        memset(&a->cal, 0, sizeof a->cal);
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
    if (r->nsens < 1 || r->nsens > CFG_SENS_MAX || r->active >= r->nsens)
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

// A version 1 record as version 6: one sensor, default tuning, the return
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

// A version 2 to 5 record as version 6. They are version 6 without the
// return (valve[6]) and without the fields after crc_at; the return is
// their back-off, so homing ends where it did. Fields they lack are 0 (the
// build's fallback address), except Remote control, On as the default; a
// version 5 record has its own.
static void from_old(const void *v, size_t tune_at, size_t crc_at, cfg_rec_t *r)
{
    memset(r, 0, sizeof *r);
    r->remote = 1;                                                 // version 5 overwrites it
    memcpy(r, v, tune_at);                                         // header, mot, valve[0..5]
    memcpy(r->tune, (const uint8_t *)v + tune_at, crc_at - tune_at);   // tune on
    r->valve[6] = r->valve[3];
    r->version = CFG_VERSION;
    r->size = sizeof *r;
}
_Static_assert(offsetof(cfg_rec_v5_t, tune) == offsetof(cfg_rec_t, valve) + OLD_VALVE_N * 4, "old valve[] then tune");
_Static_assert(offsetof(cfg_rec_v5_t, crc) - offsetof(cfg_rec_v5_t, tune) ==
               offsetof(cfg_rec_t, crc) - offsetof(cfg_rec_t, tune), "v5 is v6 from tune on");
#define FROM_OLD(T, v, r) from_old((v), offsetof(T, tune), offsetof(T, crc), (r))

// Reads one sector into slot[i]; NULL if usable, else why not.
static const char *read_slot(int i)
{
    static union {                 // one old version per read
        cfg_rec_v1_t v1;
        cfg_rec_v2_t v2;
        cfg_rec_v3_t v3;
        cfg_rec_v4_t v4;
        cfg_rec_v5_t v5;
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
        FROM_OLD(cfg_rec_v2_t, &old.v2, r);
        lv_strlcpy(r->name, FACTORY_NAME, sizeof r->name);   // no name before version 3
    } else if (r->magic == CFG_MAGIC && r->version == 3 && r->size == sizeof old.v3) {
        memcpy(&old.v3, r, sizeof old.v3);
        if (old.v3.crc != crc32_ieee(&old.v3, offsetof(cfg_rec_v3_t, crc)))
            return "CRC mismatch";
        FROM_OLD(cfg_rec_v3_t, &old.v3, r);
    } else if (r->magic == CFG_MAGIC && r->version == 4 && r->size == sizeof old.v4) {
        memcpy(&old.v4, r, sizeof old.v4);
        if (old.v4.crc != crc32_ieee(&old.v4, offsetof(cfg_rec_v4_t, crc)))
            return "CRC mismatch";
        FROM_OLD(cfg_rec_v4_t, &old.v4, r);
    } else if (r->magic == CFG_MAGIC && r->version == 5 && r->size == sizeof old.v5) {
        memcpy(&old.v5, r, sizeof old.v5);
        if (old.v5.crc != crc32_ieee(&old.v5, offsetof(cfg_rec_v5_t, crc)))
            return "CRC mismatch";
        FROM_OLD(cfg_rec_v5_t, &old.v5, r);
    } else if (r->magic != CFG_MAGIC || r->version != CFG_VERSION || r->size != sizeof *r) {
        return "not a settings record of this version";
    } else if (r->crc != rec_crc(r)) {
        return "CRC mismatch";
    }
    slot_offset[i] = r->valve[5];   // the zero offset, removed in 0.13.31
    r->valve[5] = 0;
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

static bool sens_same(const cfg_sens_t *a, const cfg_sens_t *b)
{
    return a->serial == b->serial && a->psi10 == b->psi10 && a->has_cal == b->has_cal &&
           (!a->has_cal || memcmp(&a->cal, &b->cal, sizeof a->cal) == 0);
}

static const cfg_sens_t *find_sens(const cfg_rec_t *r, uint32_t serial)
{
    for (uint32_t i = 0; i < r->nsens; i++)
        if (r->sens[i].serial == serial)
            return &r->sens[i];
    return NULL;
}

// Counts the settings in live differing from b. With print, lists them on
// the link; with buf, writes one line each for those the screens have no
// field for (sensors, calibrations, MOT MODE / HOLD / ORDER, VALVE CLEAR).
static int diff(const cfg_rec_t *live, const cfg_rec_t *b, bool print, char *buf, size_t n)
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
            LINE("Motor %s (serial only)\n", mot_name[i]);
        if (!print)
            continue;
        if (i == M_MODE)
            link_printf("CFG DIFF MOT MODE saved %s, now %s\r\n",
                        stepper3_mode_name((stepper3_mode_t)b->mot[i]),
                        stepper3_mode_name((stepper3_mode_t)live->mot[i]));
        else if (i == M_ORDER)
            link_printf("CFG DIFF MOT ORDER saved %04ld, now %04ld\r\n", (long)b->mot[i], (long)live->mot[i]);
        else
            link_printf("CFG DIFF MOT %s saved %ld, now %ld\r\n", mot_name[i], (long)b->mot[i], (long)live->mot[i]);
    }
    for (int i = 0; i < VALVE_N_SETTINGS; i++) {
        if (live->valve[i] == b->valve[i])
            continue;
        count++;
        if (buf && strcmp(valve_setting_name(i), "CLEAR") == 0)
            LINE("Valve CLEAR (serial only)\n");
        if (print)
            link_printf("CFG DIFF VALVE %s saved %ld, now %ld\r\n", valve_setting_name(i),
                        (long)b->valve[i], (long)live->valve[i]);
    }
    for (int i = 0; i < T_N; i++) {
        if (live->tune[i] == b->tune[i])
            continue;
        count++;
        if (print) {
            char x[2][16];
            link_printf("CFG DIFF TUNE %s saved %s, now %s%s\r\n", tune_name[i],
                        tune_fmt(x[0], sizeof x[0], i, b->tune[i]), tune_fmt(x[1], sizeof x[1], i, live->tune[i]),
                        tune_unit[i]);
        }
    }
    if (strcmp(live->name, b->name) != 0) {
        count++;
        LINE("Cryostat name: %s\n", live->name[0] ? live->name : "none");
        if (print)
            link_printf("CFG DIFF NAME saved \"%s\", now \"%s\"\r\n", b->name, live->name);
    }
    if (live->net_ip != b->net_ip || live->net_mask != b->net_mask) {
        char x[2][48];
        count++;
        cfg_net_text(x[0], sizeof x[0], live->net_ip, live->net_mask);
        LINE("Fallback address: %s\n", x[0]);
        if (print) {
            cfg_net_text(x[1], sizeof x[1], b->net_ip, b->net_mask);
            link_printf("CFG DIFF NET saved %s, now %s\r\n", x[1], x[0]);
        }
    }
    if (live->remote != b->remote) {
        count++;
        LINE("Remote control: %s\n", live->remote ? "On" : "Off");
        if (print)
            link_printf("CFG DIFF REMOTE saved %s, now %s\r\n", b->remote ? "On" : "Off", live->remote ? "On" : "Off");
    }
    if (live->sens[live->active].serial != b->sens[b->active].serial) {
        count++;
        if (print)
            link_printf("CFG DIFF SENSOR in use saved s/n %lu, now s/n %lu\r\n",
                        (unsigned long)b->sens[b->active].serial, (unsigned long)live->sens[live->active].serial);
    }
    for (uint32_t i = 0; i < live->nsens; i++) {
        const cfg_sens_t *q = &live->sens[i], *o = find_sens(b, q->serial);
        if (o && sens_same(q, o))
            continue;
        count++;
        const char *what = !o ? "added" : q->psi10 != o->psi10 ? "range changed" : "new calibration";
        LINE("Sensor s/n %lu: %s\n", (unsigned long)q->serial, what);
        if (print)
            link_printf("CFG DIFF SENSOR s/n %lu %s\r\n", (unsigned long)q->serial, what);
    }
    for (uint32_t i = 0; i < b->nsens; i++) {
        if (find_sens(live, b->sens[i].serial))
            continue;
        count++;
        LINE("Sensor s/n %lu: removed\n", (unsigned long)b->sens[i].serial);
        if (print)
            link_printf("CFG DIFF SENSOR s/n %lu removed\r\n", (unsigned long)b->sens[i].serial);
    }
#undef LINE
    if (buf && used && buf[used - 1] == '\n')
        buf[used - 1] = '\0';
    return count;
}

int cfg_unsaved(void)
{
    capture(&scratch);
    return diff(&scratch, base(), false, NULL, 0);
}

int cfg_unsaved_other(char *buf, size_t n)
{
    capture(&scratch);
    diff(&scratch, base(), false, buf, n);
    return buf[0] != '\0';
}

// The cal counts as saved when it matches the newest saved record.
static void sync_cal_saved(void)
{
    capture(&scratch);
    const cfg_sens_t *o = newest >= 0 ? find_sens(&slot[newest], sens[active_sens].serial) : NULL;
    cal_set_saved(newest >= 0 ? o && sens_same(&scratch.sens[active_sens], o) : cal_saved());
}

// Why the settings cannot change now, or NULL. In AUTO only the tuning can
// (cfg_save_with, PID).
static const char *busy_why(void)
{
    if (valve_auto()) return "Switch to MANUAL first";
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
static bool apply(const cfg_rec_t *r, bool with_sensors)
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
    if (with_sensors) {
        nsens = r->nsens;
        active_sens = r->active;
        memcpy(sens, r->sens, sizeof sens);
        const cfg_sens_t *a = &sens[active_sens];
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
    nsens = 1;
    active_sens = 0;
    memset(sens, 0, sizeof sens);
    sens[0].serial = FACTORY_SERIAL;
    sens[0].psi10 = FACTORY_PSI10;
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
}

// Saves the live settings as the next rev. why is for the screens.
// Writes the record in scratch as the next rev (cfg_save_with fills it with
// the candidate before putting it in use; save() with the live settings).
static bool write_scratch(const char **why)
{
    const char *bad;
    if (!flash_ok) {
        link_printf("ERR Serial flash not found, nothing saved\r\n");
        *why = "serial flash not found, nothing saved";
        return false;
    }
    if ((bad = values_why(&scratch)) != NULL) {
        link_printf("ERR %s out of range, nothing saved\r\n", bad);
        *why = "a value is out of range, nothing saved";
        return false;
    }
    scratch.rev = newest >= 0 ? slot[newest].rev + 1 : 1;
    lv_strlcpy(scratch.saved_at, rtclock_iso(), sizeof scratch.saved_at);
    scratch.crc = rec_crc(&scratch);
    int t = newest == 0 ? 1 : 0;
    uint32_t t0 = lv_tick_get();
    bool ok = extflash_erase_4k(slot_addr[t]) &&
              extflash_write(slot_addr[t], &scratch, sizeof scratch) &&
              verify(slot_addr[t], &scratch, sizeof scratch);
    uint32_t ms = lv_tick_elaps(t0);
    if (!ok) {
        scan();
        valve_event("CFG", "Save failed in %c, nothing saved", slot_letter(t));
        if (newest >= 0)
            link_printf("ERR Flash write or read-back failed in %c, nothing saved, rev %lu in %c kept\r\n",
                        slot_letter(t), (unsigned long)slot[newest].rev, slot_letter(newest));
        else
            link_printf("ERR Flash write or read-back failed in %c, nothing saved\r\n", slot_letter(t));
        *why = "flash write failed, nothing saved";
        return false;
    }
    slot[t] = scratch;
    slot_why[t] = NULL;
    slot_ver[t] = CFG_VERSION;
    newest = t;
    cal_set_saved(true);
    valve_event("CFG", "Settings saved %s in %c", when(&scratch), slot_letter(t));
    link_printf("OK saved rev %lu in %c (%lu ms)\r\n", (unsigned long)scratch.rev, slot_letter(t),
                (unsigned long)ms);
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
    case CFG_KP:      return &r->tune[T_KP];
    case CFG_TI:      return &r->tune[T_TI];
    case CFG_OUTMIN:  return &r->tune[T_OUTMIN];
    case CFG_DB:      return &r->tune[T_DB];
    case CFG_KDIR:    return &r->tune[T_KDIR];
    case CFG_BAND:    return &r->tune[T_BAND];
    default:          return NULL;
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
    const cfg_rec_t *r = source(from);
    if (!r)
        return false;
    if (k == CFG_SENSOR) {
        if (from == CFG_FROM_DEFAULTS)
            return false;   // Factory defaults keep the sensors
        *v = (int32_t)r->sens[r->active].serial;
        return true;
    }
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
    if (k >= CFG_KP && k <= CFG_BAND) {
        *lo = tune_lo[k - CFG_KP]; *hi = tune_hi[k - CFG_KP];
    } else if (k == CFG_RATE || k == CFG_START) {
        *lo = STEPPER3_RATE_MIN; *hi = STEPPER3_RATE_MAX;
    } else if (k == CFG_ACCEL) {
        *lo = STEPPER3_ACCEL_MIN; *hi = STEPPER3_ACCEL_MAX;
    } else if (k == CFG_REVERSE) {
        *hi = 1;
    } else if (k != CFG_SENSOR) {
        int i = k == CFG_POSMAX ? 0 : k == CFG_FHOME ? 1 : k == CFG_FCREEP ? 2 : k == CFG_BACKOFF ? 3 : 6;
        *lo = vlo[i]; *hi = vhi[i];   // the indices key_ref() uses
    }
}
_Static_assert(CFG_KP + T_N - 1 == CFG_BAND, "tuning keys in tune[] order");

int32_t cfg_tune(cfg_key_t k)
{
    return k >= CFG_KP && k <= CFG_BAND ? tune[k - CFG_KP] : 0;
}

static cfg_rec_t cand;   // cfg_save_with, cfg_sensor_use and cfg_load_live (never at once)

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
        if (tune_only && (k < CFG_KP || k > CFG_BAND)) {
            // cand holds the live values here (only tuning keys are written).
            bool same = k == CFG_SENSOR ? (uint32_t)v[k] == cand.sens[cand.active].serial
                                        : v[k] == *key_ref(&cand, (cfg_key_t)k);
            if (!same) { *why = "Switch to MANUAL first"; return false; }
            continue;
        }
        if (k == CFG_SENSOR) {
            const cfg_sens_t *q = find_sens(&cand, (uint32_t)v[k]);
            if (!q) {
                // A sensor from a loaded save that is no longer on the board.
                const cfg_rec_t *from[2] = { source(CFG_FROM_SAVED), source(CFG_FROM_PREVIOUS) };
                for (int j = 0; j < 2 && !q; j++)
                    if (from[j] && (q = find_sens(from[j], (uint32_t)v[k])) != NULL) {
                        if (cand.nsens >= CFG_SENS_MAX) { *why = "four sensors already on the board"; return false; }
                        cand.sens[cand.nsens++] = *q;
                        q = &cand.sens[cand.nsens - 1];
                    }
                if (!q) { *why = "that sensor is not on the board"; return false; }
            }
            cand.active = (uint32_t)(q - cand.sens);
            continue;
        }
        int32_t lo, hi;
        cfg_range((cfg_key_t)k, &lo, &hi);
        if (v[k] < lo || v[k] > hi) { *why = "a value is out of range, nothing saved"; return false; }
        *key_ref(&cand, (cfg_key_t)k) = v[k];
    }
    if (cand.mot[M_START] > cand.mot[M_RATE]) { *why = "Start speed must not exceed Run speed"; return false; }
    if (values_why(&cand)) { *why = "a value is out of range, nothing saved"; return false; }
    if (!tune_only && (*why = mode_why(&cand)) != NULL) return false;
    // Written first, then put in use: a failed write changes nothing, so
    // "Not saved" also means "not in use" (review of 0.10.11). The active cal
    // was captured into cand; a switch of sensor applies the new one.
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
        capture(d);   // keep the sensors and their cals
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
 * Sensors on the board
 * ------------------------------------------------------------------------ */
int cfg_sensor_count(void) { return (int)nsens; }
int cfg_sensor_active(void) { return (int)active_sens; }

bool cfg_sensor(int i, cfg_sensor_t *out)
{
    if (i < 0 || i >= (int)nsens)
        return false;
    const cfg_sens_t *q = &sens[i];
    out->serial = q->serial;
    out->psi10 = q->psi10;
    out->fs_mmbar = cfg_psi10_mmbar(q->psi10);
    out->in_use = i == (int)active_sens;
    bool has = out->in_use ? !cal_is_default() : q->has_cal != 0;
    const cal_rec_t *c = out->in_use ? NULL : &q->cal;
    static cal_rec_t act;
    if (out->in_use && has) { cal_get_record(&act); c = &act; }
    out->has_cal = has;
    out->cal = has ? c : NULL;
    const cfg_sens_t *sv = newest >= 0 ? find_sens(&slot[newest], q->serial) : NULL;
    if (out->in_use) {
        capture(&scratch);
        out->saved = sv && sens_same(&scratch.sens[i], sv);
    } else {
        out->saved = sv && sens_same(q, sv);
    }
    return true;
}

bool cfg_saved_cal(uint32_t serial, const cal_rec_t **cal)
{
    const cfg_sens_t *sv = newest >= 0 ? find_sens(&slot[newest], serial) : NULL;
    if (!sv)
        return false;
    *cal = sv->has_cal ? &sv->cal : NULL;
    return true;
}

bool cfg_sensor_add(uint32_t serial, int32_t psi10, const char **why)
{
    if (serial == 0)                 { *why = "serial number 0"; return false; }
    if (nsens >= CFG_SENS_MAX)        { *why = "four sensors already on the board"; return false; }
    for (uint32_t i = 0; i < nsens; i++)
        if (sens[i].serial == serial) { *why = "that serial number is already on the board"; return false; }
    memset(&sens[nsens], 0, sizeof sens[nsens]);
    sens[nsens].serial = serial;
    sens[nsens].psi10 = psi10;
    nsens++;
    valve_event("CFG", "Sensor s/n %lu, %ld psi added, not in use, not calibrated",
                (unsigned long)serial, (long)(psi10 / 10));
    return true;
}

bool cfg_sensor_remove(uint32_t serial, const char **why)
{
    for (uint32_t i = 0; i < nsens; i++) {
        if (sens[i].serial != serial)
            continue;
        if (i == active_sens) { *why = "that sensor is in use"; return false; }
        memmove(&sens[i], &sens[i + 1], (nsens - i - 1) * sizeof sens[0]);
        nsens--;
        if (active_sens > i)
            active_sens--;
        valve_event("CFG", "Sensor s/n %lu removed from the board", (unsigned long)serial);
        return true;
    }
    *why = "no such sensor";
    return false;
}

bool cfg_sensor_use(uint32_t serial, const char **why)
{
    if ((*why = busy_why()) != NULL) return false;
    capture(&cand);
    const cfg_sens_t *q = find_sens(&cand, serial);
    if (!q) { *why = "that sensor is not on the board"; return false; }
    if (q - cand.sens == (ptrdiff_t)cand.active)
        return true;
    cand.active = (uint32_t)(q - cand.sens);
    if (!apply(&cand, true)) { *why = "not applied"; return false; }
    valve_event("CFG", "Sensor s/n %lu in use, not saved", (unsigned long)serial);
    return true;
}

/* --------------------------------------------------------------------------
 * Commands
 * ------------------------------------------------------------------------ */
#if PMC_ENG_COMMANDS   // CFG?, CFG LOAD, CFG TEST
static void show(void)
{
    link_printf("CFG flash %s\r\n", flash_ok ? "ok" : "not found");
    for (int i = 0; flash_ok && i < 2; i++) {
        if (slot_why[i])
            link_printf("CFG %c: %s\r\n", slot_letter(i), slot_why[i]);
        else
            link_printf("CFG %c: rev %lu ok, version %lu%s%s%s\r\n", slot_letter(i), (unsigned long)slot[i].rev,
                        (unsigned long)slot_ver[i], slot[i].saved_at[0] ? ", saved " : "", slot[i].saved_at,
                        i == newest ? " (newest)" : "");
    }
    if (loaded >= 0)
        link_printf("CFG start-up: rev %lu loaded from %c\r\n", (unsigned long)loaded_rev,
                    slot_letter(loaded));
    else
        link_printf("CFG start-up: factory defaults, no saved record\r\n");
    char t[T_N][16];
    for (int i = 0; i < T_N; i++)
        tune_text(t[i], sizeof t[i], i);
    link_printf("CFG TUNE KP %s %%/mbar, TI %s min, OUTMIN %s %%, DB %s steps, KDIR %s, BAND %s mbar\r\n",
                t[T_KP], t[T_TI], t[T_OUTMIN], t[T_DB], t[T_KDIR], t[T_BAND]);
    for (uint32_t i = 0; i < nsens; i++)
        link_printf("CFG SENSOR s/n %lu, %ld.%ld psi%s\r\n", (unsigned long)sens[i].serial,
                    (long)(sens[i].psi10 / 10), (long)(sens[i].psi10 % 10), i == active_sens ? ", in use" : "");
    int n = cfg_unsaved();
    if (n)
        link_printf("CFG unsaved: %d setting%s from %s, CFG DIFF? to list\r\n", n,
                    n == 1 ? " differs" : "s differ",
                    newest >= 0 ? "the newest save" : "the start-up values");
    else
        link_printf("CFG unsaved: none\r\n");
    link_printf("OK\r\n");
}

static void load(const char *what)
{
    cfg_from_t from;
    const char *why;
    if (strcmp(what, "LAST") == 0)          from = CFG_FROM_SAVED;
    else if (strcmp(what, "PREVIOUS") == 0) from = CFG_FROM_PREVIOUS;
    else if (strcmp(what, "DEFAULTS") == 0) from = CFG_FROM_DEFAULTS;
    else {
        link_printf("ERR Expected: CFG LOAD LAST|PREVIOUS|DEFAULTS\r\n");
        return;
    }
    if (!source(from)) {
        link_printf(from == CFG_FROM_SAVED ? "ERR Nothing saved\r\n" : "ERR No previous record\r\n");
        return;
    }
    const cfg_rec_t *src = source(from);
    if (!cfg_load_live(from, &why)) {
        link_err(why);
        return;
    }
    if (from == CFG_FROM_DEFAULTS)
        valve_event("CFG", "Factory motor, valve and tuning settings loaded (sensors kept)");
    else
        valve_event("CFG", "Settings saved %s loaded from %c", when(src), slot_letter((int)(src - slot)));
    int n = cfg_unsaved();
    link_printf("OK loaded, %d unsaved%s\r\n", n, n ? ": CFG SAVE to keep" : "");
}

static void test(const char *what)
{
    if (!flash_ok) { link_printf("ERR Serial flash not found\r\n"); return; }
    if (strcmp(what, "CORRUPT") == 0) {
        if (newest < 0) { link_printf("ERR Nothing saved\r\n"); return; }
        static const uint8_t zero[4];
        int was = newest;
        // Programming clears bits without an erase: the CRC no longer matches.
        uint32_t at = slot_ver[was] == 1 ? offsetof(cfg_rec_v1_t, crc) : offsetof(cfg_rec_t, crc);
        extflash_write(slot_addr[was] + at, zero, sizeof zero);
        scan();
        valve_event("CFG", "Test: record in %c spoiled", slot_letter(was));
        link_printf("OK record in %c spoiled (%s), restart to check the start-up\r\n", slot_letter(was),
                    slot_why[was] ? slot_why[was] : "still valid?");
    } else if (strcmp(what, "ERASE") == 0) {
        bool ok = extflash_erase_4k(CFG_ADDR_A) && extflash_erase_4k(CFG_ADDR_B);
        scan();
        valve_event("CFG", "Test: both settings sectors erased");
        link_printf(ok ? "OK both erased, restart for factory defaults (old cal record kept)\r\n"
                       : "ERR Erase timed out\r\n");
    } else {
        link_printf("ERR Expected: CFG TEST CORRUPT|ERASE\r\n");
    }
}
#endif

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
 * Ethernet fallback address (0.12.13, Ryan 26 Oct): used when no DHCP server
 * answers. Set live and unsaved until a save, like the name; the link takes
 * the one loaded at start-up, so a change applies after the next restart.
 * 0 0 is the build's PMC_FALLBACK_IP and PMC_FALLBACK_MASK.
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
        lv_snprintf(buf, n, "%s %s (build default)", PMC_FALLBACK_IP, PMC_FALLBACK_MASK);
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
        lv_strlcpy(ip, PMC_FALLBACK_IP, 16);
        lv_strlcpy(mask, PMC_FALLBACK_MASK, 16);
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

#if PMC_ENG_COMMANDS   // NET?, NET IP, PID?, PID, CFG (engineering)
// NET? and NET IP <address> <mask> | NET IP DEFAULT. Over Ethernet NET IP is
// refused, so nobody sets an address that cuts off their own link; the
// screen (Settings > System setup) or the USB build sets it.
static void net_command(const char *a)
{
    char t[2][48];
    if (strcmp(a, "?") == 0) {
        char ip[16], m[16];
        cfg_net_boot(ip, m);
        cfg_net_text(t[0], sizeof t[0], net_ip, net_mask);
        link_printf("NET fallback %s, used if no DHCP server answers\r\n", t[0]);
        link_printf("NET in use since start-up %s %s%s\r\n", ip, m,
                    cfg_net_restart_due() ? "; the change applies after a save and a restart" : "");
        link_printf("OK\r\n");
        return;
    }
    if (strncmp(a, " IP ", 4) != 0) {
        link_printf("ERR Expected: NET? | NET IP <address> <mask> | NET IP DEFAULT\r\n");
        return;
    }
#if PMC_LINK_ETHERNET
    link_printf("ERR NET IP is set on the screen (Settings > System setup), not over the network\r\n");
#else
    uint32_t ip = 0, mask = 0;
    const char *why;
    a += 4;
    if (strcmp(a, "DEFAULT") != 0) {
        char x[20];
        const char *sp = strchr(a, ' ');
        size_t n = sp ? (size_t)(sp - a) : 0;
        if (!sp || n >= sizeof x) {
            link_printf("ERR Expected: NET IP <address> <mask>, e.g. NET IP 192.168.1.50 255.255.255.0\r\n");
            return;
        }
        memcpy(x, a, n);
        x[n] = '\0';
        if (!cfg_net_parse(x, &ip) || !cfg_net_parse(sp + 1, &mask)) {
            link_printf("ERR Expected: NET IP <address> <mask>, e.g. NET IP 192.168.1.50 255.255.255.0\r\n");
            return;
        }
        if (ip == 0 && mask == 0) {
            link_printf("ERR Use NET IP DEFAULT for the build default\r\n");
            return;
        }
    }
    if (!cfg_set_net(ip, mask, &why)) {
        link_err(why);
        return;
    }
    cfg_net_text(t[0], sizeof t[0], net_ip, net_mask);
    valve_event("CFG", "Fallback address set to %s, not saved", t[0]);
    link_printf("OK NET fallback %s, unsaved, used after a restart\r\n", t[0]);
#endif
}

/* PID? and PID <name> <value>: the simulator's console names. The six
 * tuning values are set live (AUTO picks them up at its next step) and
 * kept by CFG SAVE; the rest are fixed in ctrl.h. */

static void pid_show(void)
{
    char a[6][16];
    for (int i = 0; i < T_N; i++)
        tune_text(a[i], sizeof a[i], i);
    link_printf("PID KP %s %%/mbar, TI %s min, KD 0, DB %s steps, TS %d s, KDIR %s\r\n", a[T_KP], a[T_TI],
                a[T_DB], CTRL_TS_S, a[T_KDIR]);
    link_printf("PID BAND %s mbar, STABLE %d s, OUTMIN %s %%\r\n", a[T_BAND], CTRL_STABLE_S, a[T_OUTMIN]);
    link_printf("PID TCHECK %d s, NRESP %d, DPMIN 0.05 mbar, TCONV %d s, CHECKMIN %d steps\r\n", CTRL_TCHECK_S,
                CTRL_NRESP, CTRL_TCONV_S, CTRL_CHECKMIN);
    link_printf("PID LEAD %d steps, TLAG %d s\r\n", CTRL_LEAD_MAX, CTRL_TLAG_S);
    link_printf("OK\r\n");
}

static void pid_set(const char *a)
{
    static const char *const fixed_names[] = { "KD", "TS", "STABLE", "TCHECK", "NRESP", "DPMIN", "TCONV",
                                               "CHECKMIN", "LEAD", "TLAG", "PMIN", "PMAX" };
    for (int i = 0; i < T_N; i++) {
        size_t len = strlen(tune_name[i]);
        if (strncmp(a, tune_name[i], len) != 0 || a[len] != ' ')
            continue;
        const char *s = a + len;
        int64_t v;
        char lo[16], hi[16], t[16];
        if (!fixed_parse(&s, tune_dec[i], &v) || *s || v < tune_lo[i] || v > tune_hi[i] ||
            (i == T_KDIR && v == 0)) {
            if (i == T_KDIR)
                link_printf("ERR Expected: PID KDIR +1|-1\r\n");
            else
                link_printf("ERR Expected: PID %s <%s-%s>\r\n", tune_name[i],
                            fixed_fmt(lo, sizeof lo, tune_lo[i], tune_dec[i]),
                            fixed_fmt(hi, sizeof hi, tune_hi[i], tune_dec[i]));
            return;
        }
        tune[i] = (int32_t)v;
        valve_event("CFG", "PID %s set to %s, not saved", tune_name[i], tune_text(t, sizeof t, i));
        link_printf("OK PID %s %s%s, unsaved\r\n", tune_name[i], t, tune_unit[i]);
        return;
    }
    for (size_t i = 0; i < sizeof fixed_names / sizeof fixed_names[0]; i++) {
        size_t len = strlen(fixed_names[i]);
        if (strncmp(a, fixed_names[i], len) == 0 && (a[len] == ' ' || a[len] == '\0')) {
            link_printf("ERR PID %s is fixed in this firmware (PID? lists them)\r\n", fixed_names[i]);
            return;
        }
    }
    link_printf("ERR Expected: PID KP|TI|OUTMIN|DB|KDIR|BAND <value>\r\n");
}

bool cfg_command(const char *line)
{
    if (strcmp(line, "PID?") == 0) {
        pid_show();
        return true;
    }
    if (strncmp(line, "NET", 3) == 0 && (line[3] == ' ' || line[3] == '?')) {
        net_command(line + 3);
        return true;
    }
    if (strncmp(line, "PID ", 4) == 0) {
        pid_set(line + 4);
        return true;
    }
    if (strncmp(line, "CFG", 3) != 0 || (line[3] != ' ' && line[3] != '?' && line[3] != '\0'))
        return false;
    const char *a = line + 3;

    if (strcmp(a, "?") == 0) {
        show();
    } else if (strcmp(a, " DIFF?") == 0) {
        capture(&scratch);
        int n = diff(&scratch, base(), true, NULL, 0);
        const char *from = newest >= 0 ? "the newest save" : "the start-up values";
        if (n)
            link_printf("CFG DIFF %d difference%s from %s\r\nOK\r\n", n, n == 1 ? "" : "s", from);
        else
            link_printf("CFG DIFF none from %s\r\nOK\r\n", from);
    } else if (strcmp(a, " SAVE") == 0) {
        if (valve_auto())
            link_printf("ERR Switch to MANUAL first\r\n");   // as CAL SAVE
        else
            cfg_save();
    } else if (strncmp(a, " LOAD ", 6) == 0) {
        load(a + 6);
    } else if (strcmp(a, " NAME?") == 0) {
        link_printf("CFG NAME \"%s\"\r\nOK\r\n", cname);
    } else if (strncmp(a, " NAME", 5) == 0 && (a[5] == ' ' || a[5] == '\0')) {
        const char *why;
        if (cfg_set_name(a[5] ? a + 6 : "", &why))
            link_printf("OK NAME \"%s\", unsaved\r\n", cname);
        else
            link_err(why);
    } else if (strncmp(a, " TEST ", 6) == 0) {
        test(a + 6);
    } else {
        link_printf("ERR Unknown CFG command\r\n");
    }
    return true;
}
#endif
