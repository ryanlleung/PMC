#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "pmc_config.h"
#include "link.h"
#include "valve.h"
#include "trend.h"

/*
 * Two rings: fine[] holds the last 600 one-second samples, coarse[] the
 * last 360 ten-second means (an hour): 960 samples of 12 bytes, 11.5 KB
 * of RAM. The newest sample of each is at index (count - 1) % N.
 */
static trend_pt_t fine[TREND_FINE_N], coarse[TREND_COARSE_N];
static uint32_t fine_n, coarse_n;
static struct {
    int64_t p, v;
    int np, nv;
    int32_t sp;
} acc;

static int32_t mean(int64_t sum, int n)
{
    if (!n)
        return TREND_NONE;
    return (int32_t)(sum >= 0 ? (sum + n / 2) / n : (sum - n / 2) / n);
}

static void push(const trend_pt_t *qp)
{
    trend_pt_t q = *qp;
    fine[fine_n % TREND_FINE_N] = q;
    fine_n++;

    if (q.p != TREND_NONE) { acc.p += q.p; acc.np++; }
    if (q.v != TREND_NONE) { acc.v += q.v; acc.nv++; }
    acc.sp = q.sp;
    if (fine_n % 10 == 0) {
        trend_pt_t c = { mean(acc.p, acc.np), acc.sp, mean(acc.v, acc.nv) };
        coarse[coarse_n % TREND_COARSE_N] = c;
        coarse_n++;
        memset(&acc, 0, sizeof acc);
    }
}

void trend_second(const clicks_state_t *s)
{
    // One sample per one-second slot of real time, the slots fixed from the
    // first call: a late poll still fills its own slot, and slots the main
    // loop skipped (a stall) go in as no-data samples, so ages stay true on
    // the plot. slot_ms is where the next slot starts.
    static uint32_t slot_ms, last_ms;
    static bool started;
    uint32_t now = lv_tick_get();
    if (started) {
        int32_t early = (int32_t)(now - slot_ms);
        if (early < 0) {
            if (now - last_ms <= 500)
                return;                    // this slot already has its sample
            // A new second that falls just before the slot edge: the first
            // call came later in its second than this one (main.c calls on
            // its own 1 s grid, a few ms late or more). Without this, about
            // every other reading was dropped. The slots start here now.
            slot_ms = now;
            early = 0;
        }
        uint32_t gap = (uint32_t)early / 1000, fill = gap;
        if (fill > TREND_FINE_N * 6)
            fill = TREND_FINE_N * 6;       // an hour is all that is kept
        static const trend_pt_t none = { TREND_NONE, TREND_NONE, TREND_NONE };
        for (uint32_t i = 0; i < fill; i++)
            push(&none);
        slot_ms += (gap + 1) * 1000;
    } else {
        started = true;
        slot_ms = now + 1000;
    }
    last_ms = now;

    trend_pt_t q;
    bool ok = s->pm_found && s->reading_ok && !s->boost_tripped;
    q.p = ok ? s->p_mmbar : TREND_NONE;
    int32_t sp;
    q.sp = valve_auto() && valve_target(&sp) ? sp : TREND_NONE;   // the target, in AUTO only
    if (valve_calibrated() && valve_mode() != VALVE_CALIBRATING) {
        int32_t v = (valve_pct_milli() + 5) / 10;
        q.v = v < 0 ? 0 : v > 10000 ? 10000 : v;
    } else {
        q.v = TREND_NONE;
    }
    push(&q);
}

uint32_t trend_seconds(void) { return fine_n; }

bool trend_fine(uint32_t age_s, trend_pt_t *o)
{
    if (age_s >= fine_n || age_s >= TREND_FINE_N)
        return false;
    *o = fine[(fine_n - 1 - age_s) % TREND_FINE_N];
    return true;
}

bool trend_coarse(uint32_t age_s, trend_pt_t *o)
{
    // The newest coarse sample covers the 10 s before the last multiple of
    // 10; the seconds since are only in fine[].
    uint32_t r = fine_n % 10;
    if (age_s < r)
        return trend_fine(age_s, o);
    uint32_t k = (age_s - r) / 10;
    if (k >= coarse_n || k >= TREND_COARSE_N)
        return false;
    *o = coarse[(coarse_n - 1 - k) % TREND_COARSE_N];
    return true;
}


