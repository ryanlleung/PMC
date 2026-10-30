#include <stddef.h>
#include <string.h>

#include "lvgl.h"        // lv_snprintf
#include "link.h"
#include "extflash.h"
#include "cal.h"
#include "cfg.h"
#include "fixed.h"
#include "rtclock.h"
#include "pmc_config.h"   // PMC_DRUCK_SERIAL
#include "clicks.h"   // live ratio for CAL VAC / CAL AIR
#include "valve.h"    // valve_auto: no cal changes under AUTO

/* --------------------------------------------------------------------------
 * Old stand-alone record (0.3.x to 0.5.x), read at start-up only. From 0.6.0
 * the table is saved inside the settings record (cfg.c); this sector is left
 * as it was, so flashing an older firmware still finds its cal.
 * ------------------------------------------------------------------------ */
#define CAL_FLASH_ADDR  0x7FF000UL

// Nominal: 0 mV and 100 mV at 10 V = 0 and 10 mV/V over the sensor's range
// (15 psia, 1034.214 mbar, unless cal_set_sensor() says otherwise).
static cal_rec_t cal_default = {
    .magic = CAL_MAGIC, .version = CAL_VERSION, .npts = 2,
    .id = "nominal",
    .r_ppb = { 0, 10000000 },
    .p_mmbar = { 0, 1034214 },
    .kind = CAL_KIND_TABLE,
};
static char sensor_serial[12] = PMC_DRUCK_SERIAL;

static cal_rec_t active;
#if PMC_ENG_COMMANDS
static cal_rec_t staging;   // CAL NEW / CAL PT / CAL APPLY
#endif
static bool active_is_default = true;
static bool active_saved;          // active matches what is in flash
static bool flash_ok;
static uint32_t old_version;       // version of a stored record not loaded, 0 = none
static bool from_old;              // active came from the old record at start-up

/*
 * Bench two-point capture. Each point is the mean of CAPTURE_N readings
 * (one a second) of the live ratio, taken through the INA228 as it is
 * wired, so the input loading and INA228 gain and offset are in the table.
 */
#define CAPTURE_N   CAL_CAPTURE_N
static struct {
    bool have_vac, have_air;
    int32_t r_vac, p_vac;             // ppb, 0.001 mbar
    int32_t r_air, p_air;
} bench;
static struct {
    int what;                          // 0 idle, 1 vacuum, 2 air
    int32_t p;
    int n;
    int64_t sum, sq;                   // ppb, ppb^2 about the first reading
    int32_t first;
    bool baro;                         // air pressure from the Pressure 21 (0.14.0)
    int64_t baro_sum;                  // 0.001 mbar
} cap;

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

static uint32_t rec_crc(const cal_rec_t *r)
{
    return crc32(r, offsetof(cal_rec_t, crc));
}

// NULL if the table is usable, else the reason.
static const char *rec_check(const cal_rec_t *r)
{
    if (r->magic != CAL_MAGIC || r->version < 1 || r->version > CAL_VERSION)
        return "bad header";
    if (r->npts < 2 || r->npts > CAL_MAX_POINTS)
        return "need 2 to 32 points";
    for (uint32_t i = 1; i < r->npts; i++)
        if (r->r_ppb[i] <= r->r_ppb[i - 1])
            return "ratios must be strictly ascending";
    return NULL;
}

void cal_init(void)
{
    cal_rec_t r;

    active = cal_default;
    active_is_default = true;

    flash_ok = extflash_init();
    if (!flash_ok)
        return;

    extflash_read(CAL_FLASH_ADDR, &r, sizeof r);
    if (r.magic == CAL_MAGIC && r.version >= 1 && r.version < CAL_VERSION) {
        // CAL ATM era (cert table scaled at one point). Not loaded: redo the
        // bench cal. CAL? reports it until a new table is saved or erased.
        old_version = r.version;
        return;
    }
    if (rec_check(&r) == NULL && r.crc == rec_crc(&r)) {
        cal_set_record(&r, true);
        from_old = true;
    }
}

bool cal_from_old_record(void) { return from_old; }

bool cal_record_ok(const cal_rec_t *r)
{
    return r->version == CAL_VERSION && rec_check(r) == NULL && r->crc == rec_crc(r);
}

void cal_get_record(cal_rec_t *r)
{
    *r = active;
    r->crc = rec_crc(r);
}

bool cal_set_record(const cal_rec_t *r, bool saved)
{
    if (!cal_record_ok(r))
        return false;
    active = *r;
    active.id[CAL_ID_LEN - 1] = '\0';
    active_is_default = false;
    active_saved = saved;
    old_version = 0;
    memset(&bench, 0, sizeof bench);
    if (r->kind == CAL_KIND_BENCH) {
        bench.have_vac = bench.have_air = true;
        bench.r_vac = r->r_ppb[0]; bench.p_vac = r->p_mmbar[0];
        bench.r_air = r->r_ppb[1]; bench.p_air = r->p_mmbar[1];
    }
    return true;
}

void cal_set_default(bool saved)
{
    active = cal_default;
    active_is_default = true;
    active_saved = saved;
    memset(&bench, 0, sizeof bench);
}

void cal_set_saved(bool saved) { active_saved = saved; }

void cal_set_sensor(const char *serial, int32_t fs_mmbar)
{
    lv_strlcpy(sensor_serial, serial, sizeof sensor_serial);
    cal_default.p_mmbar[1] = fs_mmbar;
    if (active_is_default)
        active = cal_default;
    cap.what = 0;
}

int64_t cal_pressure_with(const cal_rec_t *c, int64_t r)
{
    uint32_t i = 0;
    while (i + 2 < c->npts && r > c->r_ppb[i + 1])
        i++;
    int64_t r0 = c->r_ppb[i], r1 = c->r_ppb[i + 1];
    int64_t p0 = c->p_mmbar[i], p1 = c->p_mmbar[i + 1];
    return p0 + (r - r0) * (p1 - p0) / (r1 - r0);
}

void cal_points(cal_point_t *vac, cal_point_t *air)
{
    vac->have = bench.have_vac; vac->r_ppb = bench.r_vac; vac->p_mmbar = bench.p_vac;
    air->have = bench.have_air; air->r_ppb = bench.r_air; air->p_mmbar = bench.p_air;
}

int cal_capturing(void) { return cap.what; }

// The end segments extend past the table.
int64_t cal_pressure_mmbar(int64_t r)
{
    return cal_pressure_with(&active, r);
}

bool cal_is_default(void)
{
    return active_is_default;
}

const char *cal_id(void)
{
    return active.id;
}

bool cal_saved(void)
{
    return active_saved;
}

uint32_t cal_old_version(void) { return old_version; }

bool cal_bench(int32_t *r_vac, int32_t *p_vac, int32_t *r_air, int32_t *p_air)
{
    if (active_is_default || active.kind != CAL_KIND_BENCH)
        return false;
    *r_vac = active.r_ppb[0]; *p_vac = active.p_mmbar[0];
    *r_air = active.r_ppb[1]; *p_air = active.p_mmbar[1];
    return true;
}

int cal_capture_left(void) { return cap.what ? CAPTURE_N - cap.n : 0; }

/* --------------------------------------------------------------------------
 * Commands
 * ------------------------------------------------------------------------ */

#if PMC_ENG_COMMANDS   // CAL? and the CAL commands (engineering)
static void show(void)
{
    char r[20], p[20];
    link_printf("CAL source=%s id=%s points=%lu flash=%s\r\n",
                      active_is_default ? "default" : active_saved ? "flash" : "ram", active.id,
                      (unsigned long)active.npts, flash_ok ? "ok" : "missing");
    if (old_version)
        link_printf("CAL old v%lu record in flash (CAL ATM era) not loaded: redo the bench cal on the screen\r\n",
                    (unsigned long)old_version);
    if (!active_is_default && active.kind == CAL_KIND_BENCH) {
        fixed_fmt(p, sizeof p, active.p_mmbar[0], 3);
        link_printf("CAL bench vacuum point %s mbar\r\n", p);
        fixed_fmt(p, sizeof p, active.p_mmbar[1], 3);
        link_printf("CAL bench air point %s mbar\r\n", p);
    } else if (bench.have_vac != bench.have_air) {
        link_printf("CAL bench %s point captured, %s point still to do\r\n",
                    bench.have_vac ? "vacuum" : "air", bench.have_vac ? "air" : "vacuum");
    }
    for (uint32_t i = 0; i < active.npts; i++) {
        fixed_fmt(r, sizeof r, active.r_ppb[i], 6);
        fixed_fmt(p, sizeof p, active.p_mmbar[i], 3);
        link_printf("CAL point %s mV/V %s mbar\r\n", r, p);
    }
    link_printf("OK\r\n");
}
#endif

/*
 * Bench two-point cal, through the INA228 as wired:
 *   CAL VAC [mbar]   pump the sensor down; pressure taken as 0 unless given
 *                    (0 to 50 mbar)
 *   CAL AIR [mbar]   vent to the room and give the room pressure (500 to
 *                    1100 mbar); without one, the Pressure 21's reading
 *                    averaged over the same readings (0.14.0)
 * A number after the pressure (the uncertainty, to 0.11.36) is ignored.
 * Each averages the live ratio over CAPTURE_N readings, then logs a CAL
 * event (the command itself is answered at the start). Once
 * both points are held the two-point table is applied (RAM only, then CAL
 * SAVE). Either point can be redone alone; the other is kept.
 */
#if PMC_ENG_COMMANDS
static void capture_start(int what, const char *s)
{
    int64_t p = 0;
    bool vac = what == 1;

    while (*s == ' ') s++;
    bool given = *s;
    if (given && (!fixed_parse(&s, 3, &p) || (vac ? p < 0 || p > 50000 : p < 500000 || p > 1100000))) {
        link_printf(vac ? "ERR Expected: CAL VAC [mbar 0 to 50]\r\n"
                        : "ERR Expected: CAL AIR [mbar 500 to 1100]\r\n");
        return;
    }
    if (!given && !vac)
        p = CAL_P_BARO;   // the room pressure from the Pressure 21
    const char *why;
    if (!cal_capture(what, (int32_t)p, &why)) {
        link_err(why);
        return;
    }
    link_printf("OK capturing %s point, %d readings, keep the pressure steady\r\n",
                vac ? "vacuum" : "air", CAPTURE_N);
}
#endif

bool cal_capture(int what, int32_t p_mmbar, const char **why)
{
    const clicks_state_t *s = clicks_state();
    // In AUTO the line is held at the target, not at vacuum or air, and a
    // new point would move the reading AUTO regulates on (0.11.44).
    if (valve_auto())    { *why = "Switch to MANUAL first"; return false; }
    if (cap.what)        { *why = "a capture is running"; return false; }
    if (!s->reading_ok)  { *why = "no valid Druck reading"; return false; }
    // A vacuum reads under 5 % of span (0.5 mV/V); room air at least 40 % of
    // what 1013 mbar gives on this sensor's range.
    int64_t air_ppb = 1013250LL * 10000000 / cal_default.p_mmbar[1];
    if (what == 1 && s->r_ppb > 500000)              { *why = "not a vacuum: pump the sensor down first"; return false; }
    if (what == 2 && s->r_ppb < air_ppb * 4 / 10)    { *why = "not room air: vent the sensor first"; return false; }
    bool baro = what == 2 && p_mmbar == CAL_P_BARO;
    if (baro && !s->baro_ok) { *why = "no room pressure sensor: give the pressure"; return false; }
    memset(&cap, 0, sizeof cap);
    cap.what = what;
    cap.p = p_mmbar;
    cap.baro = baro;
    return true;
}

static void bench_apply(void)
{
    cal_rec_t r;
    memset(&r, 0, sizeof r);
    r.magic = CAL_MAGIC;
    r.version = CAL_VERSION;
    r.kind = CAL_KIND_BENCH;
    r.npts = 2;
    // "5880156 bench 26-10-16": the date for the screens, when the clock is set.
    const char *iso = rtclock_iso();
    if (iso[0])
        lv_snprintf(r.id, CAL_ID_LEN, "%s bench %.8s", sensor_serial, iso + 2);
    else
        lv_snprintf(r.id, CAL_ID_LEN, "%s bench", sensor_serial);
    r.r_ppb[0] = bench.r_vac; r.p_mmbar[0] = bench.p_vac;
    r.r_ppb[1] = bench.r_air; r.p_mmbar[1] = bench.p_air;
    if (rec_check(&r) != NULL) {
        valve_event("CAL", "Air ratio not above vacuum ratio: check the pump-down and venting, redo both");
        return;
    }
    active = r;
    active_is_default = false;
    active_saved = false;
    char z[20], sp[20];
    fixed_fmt(z, sizeof z, (int64_t)r.r_ppb[0] * 10, 6);
    fixed_fmt(sp, sizeof sp, (int64_t)(r.r_ppb[1] - r.r_ppb[0]) * 10, 6);
    valve_event("CAL", "Bench cal applied: zero %s mV, %s mV per (air - vacuum) at 10 V, unsaved", z, sp);
}

void cal_update(const clicks_state_t *s)
{
    if (!cap.what)
        return;
    if (!s->reading_ok) {
        valve_event("CAL", "Capture stopped: Druck reading lost");
        cap.what = 0;
        return;
    }
    if (cap.baro && !s->baro_ok) {
        valve_event("CAL", "Capture stopped: room pressure sensor reading lost");
        cap.what = 0;
        return;
    }
    if (cap.baro)
        cap.baro_sum += s->baro_mmbar;
    if (cap.n == 0)
        cap.first = s->r_ppb;
    int64_t d = (int64_t)s->r_ppb - cap.first;
    cap.sum += d;
    cap.sq += d * d;
    if (++cap.n < CAPTURE_N)
        return;

    int64_t mean = cap.sum / CAPTURE_N;
    int64_t var = (cap.sq - cap.sum * mean) / (CAPTURE_N - 1);
    int32_t r = (int32_t)(cap.first + mean);
    if (cap.baro)
        cap.p = (int32_t)((cap.baro_sum + CAPTURE_N / 2) / CAPTURE_N);
    char a[20], b[20];
    fixed_fmt(a, sizeof a, r, 6);
    fixed_fmt(b, sizeof b, isqrt64((uint64_t)(var > 0 ? var : 0)), 6);
    bool vac = cap.what == 1;
    if (vac) { bench.have_vac = true; bench.r_vac = r; bench.p_vac = cap.p; }
    else     { bench.have_air = true; bench.r_air = r; bench.p_air = cap.p; }
    cap.what = 0;
    // The command was answered when the capture started: the result is an
    // EVT line, so nothing late reads as the reply to the next command.
    bool both = bench.have_vac && bench.have_air;
    char rp[40] = "";
    if (cap.baro) {
        char m[20];
        lv_snprintf(rp, sizeof rp, " at %s mbar from the room sensor", fixed_fmt(m, sizeof m, fixed_round(cap.p, 1), 2));
    }
    valve_event("CAL", "%s point %s mV/V%s (sd %s over %d readings)%s", vac ? "Vacuum" : "Air", a, rp, b,
                CAPTURE_N, both ? "" : vac ? ", CAL AIR next" : ", CAL VAC next");
    if (both)
        bench_apply();
}

#if PMC_ENG_COMMANDS
// Back to nominal: erase the old record too, then save the settings so
// neither comes back at the next start-up.
static void erase(void)
{
    if (!flash_ok) { link_printf("ERR Serial flash not found\r\n"); return; }
    if (!extflash_erase_4k(CAL_FLASH_ADDR)) { link_printf("ERR Erase timed out\r\n"); return; }
    cal_set_default(false);
    old_version = 0;
    from_old = false;
    link_printf("CAL old record erased, using nominal\r\n");
    cfg_save();
}

bool cal_command(const char *line)
{
    if (strncmp(line, "CAL", 3) != 0)
        return false;
    const char *a = line + 3;

    if (strcmp(a, "?") == 0) {
        show();
        return true;
    }
    if (valve_auto()) {
        link_printf("ERR Switch to MANUAL first\r\n");   // a cal change moves the reading AUTO uses
    } else if (strncmp(a, " NEW", 4) == 0) {
        const char *id = a + 4;
        while (*id == ' ') id++;
        memset(&staging, 0, sizeof staging);
        staging.magic = CAL_MAGIC;
        staging.version = CAL_VERSION;
        staging.kind = CAL_KIND_TABLE;
        lv_strlcpy(staging.id, *id ? id : "unnamed", CAL_ID_LEN);
        link_printf("OK new %s\r\n", staging.id);
    } else if (strncmp(a, " PT ", 4) == 0) {
        const char *s = a + 4;
        int64_t r, p;
        if (staging.magic != CAL_MAGIC) { link_printf("ERR CAL NEW first\r\n"); return true; }
        if (staging.npts >= CAL_MAX_POINTS) { link_printf("ERR Table full (32)\r\n"); return true; }
        if (!fixed_parse(&s, 6, &r) || !fixed_parse(&s, 3, &p) ||
            r < -50000000 || r > 50000000 || p < -10000000 || p > 100000000) {
            link_printf("ERR Expected: CAL PT <mV/V> <mbar>\r\n");
            return true;
        }
        staging.r_ppb[staging.npts] = (int32_t)r;
        staging.p_mmbar[staging.npts] = (int32_t)p;
        staging.npts++;
        link_printf("OK pt %lu\r\n", (unsigned long)staging.npts);
    } else if (strcmp(a, " APPLY") == 0) {
        const char *why = rec_check(&staging);
        if (why) { link_err(why); return true; }
        active = staging;
        active_is_default = false;
        active_saved = false;
        memset(&bench, 0, sizeof bench);
        link_printf("OK applied %s, unsaved\r\n", active.id);
    } else if (strcmp(a, " VAC") == 0 || strncmp(a, " VAC ", 5) == 0) {
        capture_start(1, a + 4);
    } else if (strcmp(a, " AIR") == 0 || strncmp(a, " AIR ", 5) == 0) {
        capture_start(2, a + 4);
    } else if (strcmp(a, " SAVE") == 0) {
        cfg_save();   // the whole settings record, cal included
    } else if (strcmp(a, " ERASE") == 0) {
        erase();
    } else {
        link_printf("ERR Unknown CAL command\r\n");
    }
    return true;
}
#endif
