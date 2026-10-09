#include <string.h>

#include "lvgl.h"   // lv_snprintf
#include "link.h"
#include "cal.h"
#include "fixed.h"
#include "uncert.h"

#define DRUCK_ACC_MMBAR   414      // 0.04 % of 1034.214 mbar (15 psia FS)
#define INA_OFFSET_NV     1000     // INA228 shunt offset, 1 uV max
#define REF_P_DEFAULT     1013250  // assumed reference when none was stored
#define NOISE_N           10

static int32_t hist[NOISE_N];
static int hist_n, hist_i;

typedef struct {
    bool ok;            // a figure can be given
    bool assumed;       // reference pressure or uncertainty assumed
    const char *why;    // when !ok
    int32_t druck, ref, offset, noise, total;   // 0.001 mbar
    int32_t ref_unc, ref_p;
} terms_t;

static terms_t last;
static const clicks_state_t *last_s;

static int32_t abs32(int64_t v) { return (int32_t)(v < 0 ? -v : v); }

static void compute(const clicks_state_t *s, terms_t *t)
{
    memset(t, 0, sizeof *t);
    if (!s->reading_ok) { t->why = "no reading"; return; }
    if (cal_is_default()) { t->why = "not calibrated"; return; }
    if (!cal_atm_applied()) { t->why = "no CAL ATM"; return; }

    t->ok = true;
    t->druck = DRUCK_ACC_MMBAR;

    // Gain-type: the reference fixes the gain at ref_p, so its uncertainty
    // scales with distance from the table's zero-signal pressure p0.
    int64_t p0 = cal_pressure_mmbar(0);
    t->ref_unc = cal_atm_unc_mmbar();
    t->ref_p = cal_atm_ref_mmbar();
    if (t->ref_unc <= 0) { t->ref_unc = CAL_ATM_UNC_DEFAULT; t->assumed = true; }
    if (t->ref_p <= 0) { t->ref_p = REF_P_DEFAULT; t->assumed = true; }
    if (t->ref_p > p0)
        t->ref = abs32((int64_t)t->ref_unc * (s->p_mmbar - p0) / (t->ref_p - p0));

    // Offset-type: 1 uV of signal as a ratio, through the local table slope.
    if (s->bus_mv > 0) {
        int64_t dr = (int64_t)INA_OFFSET_NV * 1000 / s->bus_mv;   // ppb
        t->offset = abs32(cal_pressure_mmbar(s->r_ppb + dr) - cal_pressure_mmbar(s->r_ppb));
    }

    if (hist_n >= 3) {
        int64_t sum = 0, sq = 0;
        for (int i = 0; i < hist_n; i++) sum += hist[i];
        int64_t mean = sum / hist_n;
        for (int i = 0; i < hist_n; i++) sq += (int64_t)(hist[i] - mean) * (hist[i] - mean);
        t->noise = 2 * (int32_t)isqrt64((uint64_t)(sq / (hist_n - 1)));
    }

    uint64_t ss = (uint64_t)((int64_t)t->druck * t->druck) + (uint64_t)((int64_t)t->ref * t->ref) +
                  (uint64_t)((int64_t)t->offset * t->offset) + (uint64_t)((int64_t)t->noise * t->noise);
    t->total = (int32_t)isqrt64(ss);
}

void uncert_update(const clicks_state_t *s)
{
    last_s = s;
    if (s->reading_ok && !s->pm_diag) {
        hist[hist_i] = s->p_mmbar;
        hist_i = (hist_i + 1) % NOISE_N;
        if (hist_n < NOISE_N) hist_n++;
    } else if (!s->reading_ok) {
        hist_n = hist_i = 0;
    }
    compute(s, &last);
}

const char *uncert_screen_text(bool *warn)
{
    static char t[32];
    char v[16];
    if (!last.ok) {
        *warn = last.why && strcmp(last.why, "no reading") != 0;
        if (!*warn) return "";
        lv_snprintf(t, sizeof t, "+/- ? (%s)", last.why);
        return t;
    }
    *warn = last.assumed;
    fixed_fmt(v, sizeof v, fixed_round(last.total, 1), 2);
    lv_snprintf(t, sizeof t, "+/- %s mbar", v);
    return t;
}

static void term(const char *name, int32_t v, const char *note)
{
    char b[16];
    link_printf("UNC %-14s %s mbar  %s\r\n", name, fixed_fmt(b, sizeof b, v, 3), note);
}

bool uncert_command(const char *line)
{
    if (strcmp(line, "UNC?") != 0)
        return false;
    if (!last.ok) {
        link_printf("UNC none: %s\r\nOK\r\n", last.why ? last.why : "no reading yet");
        return true;
    }
    char b[96], p[16], u[16];
    fixed_fmt(p, sizeof p, last.ref_p, 3);
    fixed_fmt(u, sizeof u, last.ref_unc, 3);
    term("total (RSS)", last.total, "about 95 %");
    term("druck", last.druck, "datasheet, 0.04 % FS");
    lv_snprintf(b, sizeof b, "CAL ATM reference +/-%s at %s mbar%s, scaled to this pressure",
                u, p, last.assumed ? " (ASSUMED, give it: CAL ATM <mbar> <unc>)" : "");
    term("atm_ref", last.ref, b);
    term("ina228_offset", last.offset, "datasheet, 1 uV");
    term("noise", last.noise, "2 sd of the last 10 readings");
    link_printf("UNC not included: INA228 gain (taken out by CAL ATM), Druck thermal effects, "
                "drift of the input-loading correction (RDIFF typical only)\r\nOK\r\n");
    return true;
}
