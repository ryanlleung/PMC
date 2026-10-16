#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
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
    static uint32_t slot_ms;
    static bool started;
    uint32_t now = lv_tick_get();
    if (started) {
        int32_t early = (int32_t)(now - slot_ms);
        if (early < 0)
            return;                        // this slot already has its sample
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

    trend_pt_t q;
    // PM DIAG freezes the readings: no-data samples while it runs.
    bool ok = !s->pm_diag && s->pm_found && s->reading_ok && !s->boost_tripped;
    q.p = ok ? s->p_mmbar : TREND_NONE;
    q.sp = TREND_NONE;   // AUTO comes in stage 8
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

static void put_fixed(char *buf, size_t n, int32_t v, int dec)
{
    if (v == TREND_NONE) {
        lv_strlcpy(buf, "-", n);
        return;
    }
    int32_t div = dec == 3 ? 1000 : 100;
    int32_t a = abs(v);
    lv_snprintf(buf, n, dec == 3 ? "%s%ld.%03ld" : "%s%ld.%02ld", v < 0 ? "-" : "",
                (long)(a / div), (long)(a % div));
}

// TREND DUMP in progress: samples next..end-1 (counted from start-up),
// labelled with their age when the command came.
static struct {
    bool active, head;
    uint32_t next, end, since;
} dump;

#define DUMP_LINE  64      // room wanted before a line goes out
#define DUMP_GIVEUP 2000   // ms with no room: the reader has gone

void trend_task(void)
{
    if (!dump.active)
        return;
    for (int lines = 0; lines < 8; lines++) {     // a few per loop pass
        if (link_tx_room() < DUMP_LINE) {
            if (lv_tick_elaps(dump.since) >= DUMP_GIVEUP)
                dump.active = false;               // nobody reading: drop the rest
            return;
        }
        dump.since = lv_tick_get();
        if (dump.head) {
            link_printf("TREND age_s, pressure mbar, target mbar, valve %%\r\n");
            dump.head = false;
            continue;
        }
        if (dump.next >= dump.end) {
            link_printf("OK\r\n");
            dump.active = false;
            return;
        }
        uint32_t idx = dump.next++;
        trend_pt_t q;
        // Its age now (the ring may have moved on since the command).
        if (!trend_fine(fine_n - 1 - idx, &q))
            continue;
        char p[20], sp[20], v[20];
        put_fixed(p, sizeof p, q.p, 3);
        put_fixed(sp, sizeof sp, q.sp, 3);
        put_fixed(v, sizeof v, q.v, 2);
        link_printf("TREND -%lu, %s, %s, %s\r\n", (unsigned long)(dump.end - 1 - idx), p, sp, v);
    }
}

bool trend_command(const char *line)
{
    if (strcmp(line, "TREND?") == 0) {
        uint32_t f = fine_n < TREND_FINE_N ? fine_n : TREND_FINE_N;
        uint32_t c = coarse_n < TREND_COARSE_N ? coarse_n : TREND_COARSE_N;
        link_printf("TREND %lu s since start-up, %lu x 1 s and %lu x 10 s kept (10 min and 1 h)\r\n",
                    (unsigned long)fine_n, (unsigned long)f, (unsigned long)c);
        link_printf("OK\r\n");
        return true;
    }
    if (strncmp(line, "TREND DUMP", 10) == 0) {
        char *end;
        long s = strtol(line + 10, &end, 10);
        if (end == line + 10 || *end || s < 1 || s > TREND_FINE_N) {
            link_printf("ERR expected: TREND DUMP <1-%d>\r\n", TREND_FINE_N);
            return true;
        }
        // Sent from trend_task() a line at a time, as the link has room.
        dump.end = fine_n;
        dump.next = fine_n > (uint32_t)s ? fine_n - (uint32_t)s : 0;
        if (dump.end - dump.next > TREND_FINE_N)
            dump.next = dump.end - TREND_FINE_N;
        dump.head = true;
        dump.active = true;
        dump.since = lv_tick_get();
        return true;
    }
    return false;
}
