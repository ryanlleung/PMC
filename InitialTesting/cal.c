#include <stddef.h>
#include <string.h>

#include "lvgl.h"        // lv_snprintf
#include "link.h"
#include "extflash.h"
#include "cal.h"
#include "cfg.h"
#include "fixed.h"
#include "clicks.h"   // live ratio for CAL VAC / CAL AIR
#include "valve.h"    // valve_auto: no cal changes under AUTO

// Nominal: 0 mV and 100 mV at 10 V = 0 and 10 mV/V over the sensor's range
// (15 psia, 1034.214 mbar).
static const cal_rec_t cal_default = {
    .magic = CAL_MAGIC, .version = CAL_VERSION,
    .r_ppb = { 0, 10000000 },
    .p_mmbar = { 0, CAL_FS_MMBAR },
};

static cal_rec_t active;
static bool active_is_default = true;
static bool active_saved;          // active matches what is in flash

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

static uint32_t rec_crc(const cal_rec_t *r)
{
    return crc32_ieee(r, offsetof(cal_rec_t, crc));
}

// The header is right and the air ratio is above the vacuum ratio.
static bool rec_check(const cal_rec_t *r)
{
    return r->magic == CAL_MAGIC && r->version == CAL_VERSION && r->r_ppb[1] > r->r_ppb[0];
}

void cal_init(void)
{
    active = cal_default;
    active_is_default = true;
    active_saved = false;
    memset(&bench, 0, sizeof bench);
}

bool cal_record_ok(const cal_rec_t *r)
{
    return rec_check(r) && r->crc == rec_crc(r);
}

bool cal_from_v4(const cal_rec_v4_t *o, cal_rec_t *r)
{
    if (o->magic != CAL_MAGIC || o->version != 4 || o->npts != 2 || o->r_ppb[1] <= o->r_ppb[0] ||
        o->crc != crc32_ieee(o, offsetof(cal_rec_v4_t, crc)))
        return false;
    memset(r, 0, sizeof *r);
    r->magic = CAL_MAGIC;
    r->version = CAL_VERSION;
    r->r_ppb[0] = o->r_ppb[0]; r->p_mmbar[0] = o->p_mmbar[0];
    r->r_ppb[1] = o->r_ppb[1]; r->p_mmbar[1] = o->p_mmbar[1];
    r->crc = rec_crc(r);
    return true;
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
    active_is_default = false;
    active_saved = saved;
    bench.have_vac = bench.have_air = true;
    bench.r_vac = r->r_ppb[0]; bench.p_vac = r->p_mmbar[0];
    bench.r_air = r->r_ppb[1]; bench.p_air = r->p_mmbar[1];
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

int64_t cal_pressure_with(const cal_rec_t *c, int64_t r)
{
    int64_t r0 = c->r_ppb[0], r1 = c->r_ppb[1];
    int64_t p0 = c->p_mmbar[0], p1 = c->p_mmbar[1];
    return p0 + (r - r0) * (p1 - p0) / (r1 - r0);
}

void cal_points(cal_point_t *vac, cal_point_t *air)
{
    vac->have = bench.have_vac; vac->r_ppb = bench.r_vac; vac->p_mmbar = bench.p_vac;
    air->have = bench.have_air; air->r_ppb = bench.r_air; air->p_mmbar = bench.p_air;
}

int cal_capturing(void) { return cap.what; }

// The line through the two points, past them too.
int64_t cal_pressure_mmbar(int64_t r)
{
    return cal_pressure_with(&active, r);
}

bool cal_is_default(void)
{
    return active_is_default;
}

bool cal_saved(void)
{
    return active_saved;
}

int cal_capture_left(void) { return cap.what ? CAPTURE_N - cap.n : 0; }

/* --------------------------------------------------------------------------
 * Commands
 * ------------------------------------------------------------------------ */


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

bool cal_capture(int what, int32_t p_mmbar, const char **why)
{
    const clicks_state_t *s = clicks_state();
    // In AUTO the line is held at the target, not at vacuum or air, and a
    // new point would move the reading AUTO regulates on (0.11.44).
    if (valve_auto())    { *why = "Stop AUTO first"; return false; }
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
    r.r_ppb[0] = bench.r_vac; r.p_mmbar[0] = bench.p_vac;
    r.r_ppb[1] = bench.r_air; r.p_mmbar[1] = bench.p_air;
    if (!rec_check(&r)) {
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

