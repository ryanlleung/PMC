#include <math.h>
#include <string.h>

#include "lvgl.h"
#include "cfg.h"
#include "trend.h"
#include "ui.h"

/* --------------------------------------------------------------------------
 * Trend (stage 7, simulator v60 buildTrend/drawChart) and the Home
 * sparklines (drawSpark). Both draw straight from trend.c on a draw event:
 * no lv_chart, no point arrays in the LVGL heap.
 *   Trend   chart card 10,48 343x160: legend, pressure axis left (linear,
 *           3-5 rounded steps), valve 0-100 % right, -range and now under it;
 *           Up / Range / Down on the right, 1, 2, 5, 10, 30 min and 1 hour.
 *   Spark   10 min, one point per 5 s (slot means, see pts_build), one pen, no axes; pressure linear
 *           (log when the window spans more than a decade), valve 0-100 %.
 * ------------------------------------------------------------------------ */
#define COL_PV    UI_BLUE
#define COL_SP    lv_color_hex(0x3F3F3F)
#define COL_POS   UI_FILL
#define COL_GRID  lv_color_hex(0xE3E9EF)
#define COL_AREA  lv_color_hex(0xDCE3EA)   // valve area on Trend, under the grid

typedef enum { K_P, K_SP, K_V } tkey_t;

static int32_t val(const trend_pt_t *q, tkey_t k) { return k == K_P ? q->p : k == K_SP ? q->sp : q->v; }

// Points to plot: each the mean of the samples in a fixed slot of time
// (Ryan, 16 Oct). Slots are counted from start-up, not from now, so a
// point keeps its value as the plot moves left; only the newest slot,
// still filling, changes. Up to 10 min from the 1 s samples, beyond from
// the 10 s means (slots are then whole multiples of 10 s, matching them).
// The pressure pen draws each slot through its first sample, the two either
// side of its largest change and its last, so a step draws upright, not as
// a slope across the slot (Ryan, 21 Oct); lo and hi set the scale.
// The target is not a mean: each slot keeps its last target and when that
// began, so a step is one straight up or down line, one pixel wide, at the
// second it changed (Ryan, 10 Nov: a step mid-slot drew a mean level
// between the two, so it leaned and spread over two pixels).
#define PTS_MAX 128   // the most any range plots is 121 (win / slot + 1)
static struct {
    float age;            // seconds before the newest sample
    int32_t v[3];         // K_P, K_SP, K_V means, or TREND_NONE
    // Pressure, if v[K_P] is set: values and their ages (s), first, before
    // and after the largest change, last.
    int32_t p[4], p_lo, p_hi;
    int16_t p_age[4];
    int16_t sp_age;       // age (s) of the slot's first sample with its last target
} pts[PTS_MAX];
static int pts_n;

// Seconds per plotted point for a window of win seconds in n points.
static uint32_t slot_s(uint32_t win, int n)
{
    uint32_t r = win <= TREND_FINE_N ? 1 : 10;
    uint32_t slot = (win + (uint32_t)(n - 2)) / (uint32_t)(n - 1);
    return (slot + r - 1) / r * r;
}

static void pts_build(uint32_t win, int n)
{
    // The board draws in 10-row strips, and the draw event runs once per
    // strip: the points are built once per new sample or window.
    static uint32_t b_win, b_total = UINT32_MAX;
    static int b_n;
    uint32_t total = trend_seconds();
    if (win == b_win && n == b_n && total == b_total)
        return;
    b_win = win;
    b_n = n;
    b_total = total;
    uint32_t r = win <= TREND_FINE_N ? 1 : 10;            // sample spacing, s
    uint32_t slot = slot_s(win, n);
    pts_n = 0;
    if (!total)
        return;
    uint32_t t_new = total - 1;                           // newest second
    uint32_t j0 = t_new / slot;
    uint32_t nslot = win / slot + 1;
    if (nslot > PTS_MAX) nslot = PTS_MAX;
    for (uint32_t j = nslot; j-- > 0;) {                  // oldest first
        if (j > j0) continue;
        uint32_t s0 = (j0 - j) * slot, s1 = s0 + slot - 1;   // seconds since start-up
        if (s1 > t_new) s1 = t_new;
        if (t_new - s0 > win + slot) continue;
        int64_t sum[3] = { 0, 0, 0 };
        int cnt[3] = { 0, 0, 0 };
        int32_t pp[4] = { 0, 0, 0, 0 }, p_lo = 0, p_hi = 0, jump = -1, sp = TREND_NONE;
        int16_t pa[4] = { 0, 0, 0, 0 }, sp_age = 0;
        // Samples in the slot, newest second at age 0; a 10 s mean is read
        // once per 10 s (trend_coarse gives the 1 s samples still filling).
        for (uint32_t t = s0; t <= s1; t += r) {
            trend_pt_t q;
            uint32_t age = t_new - (r == 1 ? t : (t + r - 1 < t_new ? t + r - 1 : t_new));
            bool ok = r == 1 ? trend_fine(age, &q) : trend_coarse(age, &q);
            if (!ok) continue;
            for (int k = 0; k < 3; k++) {
                int32_t v = val(&q, (tkey_t)k);
                if (v == TREND_NONE) continue;
                if (k == K_P) {
                    if (!cnt[k]) {
                        pp[0] = pp[1] = pp[2] = p_lo = p_hi = v;
                        pa[0] = pa[1] = pa[2] = (int16_t)age;
                    } else {
                        int32_t d = v > pp[3] ? v - pp[3] : pp[3] - v;
                        if (d > jump) {
                            jump = d;
                            pp[1] = pp[3]; pp[2] = v;
                            pa[1] = pa[2] = (int16_t)age;
                        }
                    }
                    pp[3] = v;
                    pa[3] = (int16_t)age;
                    if (v < p_lo) p_lo = v;
                    if (v > p_hi) p_hi = v;
                }
                if (k == K_SP && (!cnt[k] || v != sp)) {   // oldest first: the last change wins
                    sp = v;
                    sp_age = (int16_t)age;
                }
                sum[k] += v;
                cnt[k]++;
            }
        }
        float mid = ((float)s0 + (float)s1) / 2.0f;
        float age = (float)t_new - mid;
        if (age > (float)win) continue;
        for (int k = 0; k < 3; k++)
            pts[pts_n].v[k] = cnt[k] ? (int32_t)(sum[k] / cnt[k]) : TREND_NONE;
        pts[pts_n].v[K_SP] = cnt[K_SP] ? sp : TREND_NONE;
        pts[pts_n].sp_age = sp_age;
        for (int q = 0; q < 4; q++) {
            pts[pts_n].p[q] = pp[q];
            pts[pts_n].p_age[q] = pa[q];
        }
        pts[pts_n].p_lo = p_lo;
        pts[pts_n].p_hi = p_hi;
        pts[pts_n].age = age;
        pts_n++;
    }
}

/* The board renders in 10-row strips and these draw events run once per
 * strip, so whatever misses the strip is skipped before LVGL allocates a
 * draw task for it (a Trend redraw was some 4000 line tasks, 16 times the
 * ~250 drawn; the page stalled on a range change). */
static bool off_strip(const lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    const lv_area_t *c = &layer->_clip_area;
    return x2 < c->x1 || x1 > c->x2 || y2 < c->y1 || y1 > c->y2;
}

static void line(lv_layer_t *layer, lv_color_t c, int32_t w, float x1, float y1, float x2, float y2, bool dash)
{
    if (off_strip(layer, (int32_t)fminf(x1, x2) - w, (int32_t)fminf(y1, y2) - w,
                  (int32_t)fmaxf(x1, x2) + w, (int32_t)fmaxf(y1, y2) + w))
        return;
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = c;
    d.width = w;
    d.round_start = d.round_end = 1;
    if (dash) {
        d.dash_width = 4;
        d.dash_gap = 3;
    }
    d.p1.x = (lv_value_precise_t)x1;
    d.p1.y = (lv_value_precise_t)y1;
    d.p2.x = (lv_value_precise_t)x2;
    d.p2.y = (lv_value_precise_t)y2;
    lv_draw_line(layer, &d);
}

static void text(lv_layer_t *layer, const char *s, lv_color_t c, lv_text_align_t al,
                 int32_t x1, int32_t y1, int32_t x2)
{
    if (off_strip(layer, x1, y1, x2, y1 + 14))
        return;
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = &ui_font_12;
    d.color = c;
    d.align = al;
    d.text = s;
    lv_area_t a = { x1, y1, x2, y1 + 14 };
    lv_draw_label(layer, &d, &a);
}

static void fill_r(lv_layer_t *layer, lv_color_t c, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t rad)
{
    if (off_strip(layer, x1, y1, x2, y2))
        return;
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = c;
    r.radius = rad;
    lv_area_t a = { x1, y1, x2, y2 };
    lv_draw_rect(layer, &r, &a);
}

static void fill(lv_layer_t *layer, lv_color_t c, int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    fill_r(layer, c, x1, y1, x2, y2, 0);
}

// The newest point, a 5 px dot.
static void dot(lv_layer_t *layer, lv_color_t c, float x, float y)
{
    fill_r(layer, c, (int32_t)x - 2, (int32_t)y - 2, (int32_t)x + 2, (int32_t)y + 2, LV_RADIUS_CIRCLE);
}

// One pen through the points of pts_build(), win seconds from x0 to x1;
// y from yof. Gaps (no reading) lift the pen. Returns the last point drawn.
// stepped (the target): one dashed line across while it holds, then one
// straight up or down on a whole pixel column at the second it changed.
typedef float (*yof_t)(void *ctx, int32_t v);
static bool pen(lv_layer_t *layer, uint32_t win, tkey_t k, float x0, float x1, yof_t yof, void *ctx,
                lv_color_t c, int32_t w, bool stepped, float *lx, float *ly)
{
    bool have = false, any = false;
    float px = 0, py = 0, rx = 0;   // rx: where the target's run at py began
    for (int i = 0; i < pts_n; i++) {
        int32_t v = pts[i].v[k];
        if (v == TREND_NONE) {
            if (have && stepped && px > rx)
                line(layer, c, w, rx, py, px, py, true);
            have = false;
            continue;
        }
        float x = x1 - (x1 - x0) * pts[i].age / (float)win, y = yof(ctx, v);
        if (k == K_P) {
            // On from the slot before through this slot's four samples.
            for (int q = 0; q < 4; q++) {
                float qx = x1 - (x1 - x0) * (float)pts[i].p_age[q] / (float)win, qy = yof(ctx, pts[i].p[q]);
                if (qx < x0) qx = x0;
                if (have && (qx != px || qy != py))
                    line(layer, c, w, px, py, qx, qy, false);
                px = qx; py = qy; have = any = true;
            }
            continue;
        }
        if (stepped) {
            if (!have) {
                rx = x;
            } else if (y != py) {
                float xs = roundf(x1 - (x1 - x0) * (float)pts[i].sp_age / (float)win);
                if (xs < rx) xs = rx;
                if (xs > rx)
                    line(layer, c, w, rx, py, xs, py, true);
                line(layer, c, w, xs, py, xs, y, true);
                rx = xs;
            }
            px = x; py = y; have = any = true;
            continue;
        }
        if (have)
            line(layer, c, w, px, py, x, y, false);
        px = x; py = y; have = any = true;
    }
    if (have && stepped && px > rx)
        line(layer, c, w, rx, py, px, py, true);
    if (any) { *lx = px; *ly = py; }
    return any;
}

/* Scales ----------------------------------------------------------------- */
typedef struct {
    float lo, hi, top, bot;   // values (mbar) and pixel rows
    bool log;
} scale_t;

static float y_scale(void *ctx, int32_t v)
{
    scale_t *s = ctx;
    float f = (float)v / 1000.0f, r;
    if (s->log)
        r = (log10f(f > 1e-3f ? f : 1e-3f) - log10f(s->lo)) / (log10f(s->hi) - log10f(s->lo));
    else
        r = (f - s->lo) / (s->hi - s->lo);
    if (r < 0) r = 0;
    if (r > 1) r = 1;
    return s->bot - r * (s->bot - s->top);
}

static float y_pct(void *ctx, int32_t v)
{
    scale_t *s = ctx;
    return s->bot - (float)v / 10000.0f * (s->bot - s->top);
}

// Valve as a shaded area under its line on Trend, so it reads apart from
// the pressure line by form, not only by its grey (Ryan, 19 Oct: which line
// is which was unclear). One column per pair of points, at their mean.
static void area(lv_layer_t *layer, uint32_t win, float x0, float x1, scale_t *s, lv_color_t c)
{
    bool have = false;
    float px = 0;
    int32_t pv = 0;
    for (int i = 0; i < pts_n; i++) {
        int32_t v = pts[i].v[K_V];
        if (v == TREND_NONE) {
            have = false;
            continue;
        }
        float x = x1 - (x1 - x0) * pts[i].age / (float)win;
        if (have) {
            int32_t xa = (int32_t)lroundf(px), xb = (int32_t)lroundf(x) - 1;
            int32_t y = (int32_t)lroundf(y_pct(s, (pv + v) / 2));
            if (xb >= xa && y < (int32_t)s->bot)
                fill(layer, c, xa, y, xb, (int32_t)s->bot);
        }
        px = x; pv = v; have = true;
    }
}

// Pressure and target range over the window; false if no reading at all.
static bool p_range(float *lo, float *hi)
{
    bool any = false;
    for (int i = 0; i < pts_n; i++) {
        for (int k = 0; k < 2; k++) {
            int32_t v = pts[i].v[k ? K_SP : K_P];
            if (v == TREND_NONE)
                continue;
            float fl = (float)(k ? v : pts[i].p_lo) / 1000.0f, fh = (float)(k ? v : pts[i].p_hi) / 1000.0f;
            if (!any || fl < *lo) *lo = fl;
            if (!any || fh > *hi) *hi = fh;
            any = true;
        }
    }
    return any;
}

// The least pressure span the sparkline and Trend show (0.16.18, Ryan 12 Nov:
// "set a limit to the trend and sparkline for pressure so that small
// fluctuations around a setpoint doesn look huge"): 20 % of the highest
// pressure or target shown, 1 mbar at least. A hold at 8 mbar wandering
// 0.1 mbar either side then fills about an eighth of the height; a step
// still fills it.
#define P_SPAN_MIN   1.0f
#define P_SPAN_FRAC  0.20f

static float p_span_min(float lo, float hi)
{
    float m = (fabsf(lo) > fabsf(hi) ? fabsf(lo) : fabsf(hi)) * P_SPAN_FRAC;
    return m > P_SPAN_MIN ? m : P_SPAN_MIN;
}

/* --------------------------------------------------------------------------
 * Home sparklines
 * ------------------------------------------------------------------------ */
#define SPARK_WIN 300   // 5 min, at half the old width (Ryan, 21 Oct)
#define SPARK_N   61    // one per 5 s

static void spark_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    bool valve = lv_event_get_user_data(e) != NULL;
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    // No background: the strip's own (Ryan, 21 Oct: the tinted box looked
    // out of place).
    scale_t s = { 0, 1, (float)a.y1 + 2, (float)a.y2 - 2, false };
    float lx, ly;
    if (valve) {
        if (pen(layer, SPARK_WIN, K_V, a.x1, a.x2 - 3, y_pct, &s, COL_POS, 2, false, &lx, &ly))
            dot(layer, COL_POS, lx, ly);
        return;
    }
    float lo, hi;
    pts_build(SPARK_WIN, SPARK_N);
    if (!p_range(&lo, &hi))
        return;
    if (lo > 0 && hi / lo > 10) {
        s.log = true;
    } else {
        // At least p_span_min (20 % of the pressure, 1 mbar; 0.16.18) or
        // 10 x the target band tall, so a steady hold looks steady.
        int32_t band = 10;
        cfg_value(CFG_FROM_LIVE, CFG_BAND, &band);
        float min = (float)band / 10.0f;   // 10 x band, band in 0.01 mbar
        float pmin = p_span_min(lo, hi);
        if (min < pmin) min = pmin;
        if (hi - lo < min) {
            float c = (lo + hi) / 2;
            lo = c - min / 2;
            hi = c + min / 2;
        } else {
            float pad = (hi - lo) * 0.08f;
            lo -= pad;
            hi += pad;
        }
    }
    s.lo = lo;
    s.hi = hi;
    pen(layer, SPARK_WIN, K_SP, a.x1, a.x2 - 3, y_scale, &s, COL_SP, 1, true, &lx, &ly);
    if (pen(layer, SPARK_WIN, K_P, a.x1, a.x2 - 3, y_scale, &s, COL_PV, 2, false, &lx, &ly))
        dot(layer, COL_PV, lx, ly);
}

static void spark_tap(lv_event_t *e)
{
    (void)e;
    ui_open(&ui_scr_trend);
}

lv_obj_t *ui_spark(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, bool valve)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_ext_click_area(o, 4);
    lv_obj_add_event_cb(o, spark_draw, LV_EVENT_DRAW_MAIN, valve ? (void *)1 : NULL);
    lv_obj_add_event_cb(o, spark_tap, LV_EVENT_CLICKED, NULL);
    return o;
}

/* --------------------------------------------------------------------------
 * Trend
 * ------------------------------------------------------------------------ */
static const uint32_t WINS[6] = { 60, 120, 300, 600, 1800, 3600 };
static const char *const WIN_NAME[6] = { "1 min", "2 min", "5 min", "10 min", "30 min", "1 hour" };
static int win_i = 3;   // 10 min; kept between visits

static struct {
    lv_obj_t *plot, *range, *up, *down;
    uint32_t seen;
    int win_seen;
} tr;

#define CARD_W 343
#define PL 46   // plot inside the card: left, right, top, bottom margins (46: "1018.0" fits)
#define PR 40
#define PT 32
#define PB 23   // 12 px under the time labels (common HMI template)

static void trend_draw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(tr.plot, &a);
    // The range the Range card shows (set with the invalidate, trend_refresh).
    int wi = tr.win_seen >= 0 ? tr.win_seen : win_i;
    uint32_t win = WINS[wi];
    int32_t x0 = a.x1 + PL, x1 = a.x2 - PR, y0 = a.y1 + PT, y1 = a.y2 - PB;
    int n = (int)(x1 - x0) / 2 + 1;   // a point every 2 px

    // Pressure axis: rounded step giving 3 to 5 intervals, never below 0
    // unless a reading is (calibration drift reads a little negative). At
    // least p_span_min tall (0.5 mbar to 0.16.17).
    float lo = 0, hi = 10;
    pts_build(win, n);
    bool any = p_range(&lo, &hi);
    float pmin = p_span_min(lo, hi);
    float span = hi - lo < pmin ? pmin : hi - lo, mid = (hi + lo) / 2;
    float raw_lo = lo;
    lo = mid - span * 0.6f;
    hi = mid + span * 0.6f;
    if (lo < 0 && raw_lo >= 0) lo = 0;
    static const float steps[] = { 0.1f, 0.2f, 0.5f, 1, 2, 5, 10, 20, 50, 100, 200, 500 };
    float step = steps[0];
    for (unsigned i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        step = steps[i];
        if ((hi - lo) / step <= 5)
            break;
    }
    lo = floorf(lo / step + 1e-4f) * step;
    hi = ceilf(hi / step - 1e-4f) * step;
    if (hi - lo < step) hi = lo + step;
    scale_t s = { lo, hi, (float)y0, (float)y1, false };

    char buf[16];
    area(layer, win, (float)x0, (float)x1, &s, COL_AREA);   // under the grid
    int nt = (int)lroundf((hi - lo) / step);
    for (int i = 0; i <= nt; i++) {
        float v = lo + step * (float)i;
        int32_t y = (int32_t)lroundf(y_scale(&s, (int32_t)lroundf(v * 1000)));
        fill(layer, COL_GRID, x0, y, x1, y);
        int32_t d10 = (int32_t)lroundf(v * 10);
        if (step < 1)
            lv_snprintf(buf, sizeof buf, "%s%ld.%ld", d10 < 0 ? "-" : "", (long)(LV_ABS(d10) / 10), (long)(LV_ABS(d10) % 10));
        else
            lv_snprintf(buf, sizeof buf, "%ld", (long)lroundf(v));
        text(layer, buf, UI_MUTED, LV_TEXT_ALIGN_RIGHT, a.x1, y - 7, x0 - 8);
    }
    for (int i = 0; i <= 4; i++) {
        int32_t y = (int32_t)lroundf(y_pct(&s, 2500 * i));
        lv_snprintf(buf, sizeof buf, "%d", 25 * i);
        text(layer, buf, UI_MUTED, LV_TEXT_ALIGN_LEFT, x1 + 8, y - 7, a.x2);
    }
    lv_snprintf(buf, sizeof buf, "-%s", WIN_NAME[wi]);
    text(layer, buf, UI_MUTED, LV_TEXT_ALIGN_LEFT, x0, y1 + 3, x0 + 80);
    text(layer, "now", UI_MUTED, LV_TEXT_ALIGN_RIGHT, x1 - 60, y1 + 3, x1);
    // Legend: a sample of each mark beside its name, each over its own axis
    // (pressure left, valve right); the names in text colour.
    // 12 px from the card's edges (common HMI template).
    fill(layer, COL_PV, a.x1 + 12, a.y1 + 15, a.x1 + 25, a.y1 + 16);
    text(layer, "Pressure, mB", UI_MUTED, LV_TEXT_ALIGN_LEFT, a.x1 + 30, a.y1 + 9, a.x1 + 142);
    fill(layer, COL_AREA, a.x2 - 25, a.y1 + 12, a.x2 - 12, a.y1 + 20);
    fill(layer, COL_POS, a.x2 - 25, a.y1 + 12, a.x2 - 12, a.y1 + 12);
    text(layer, "Valve, %", UI_MUTED, LV_TEXT_ALIGN_RIGHT, a.x2 - 142, a.y1 + 9, a.x2 - 30);

    float lx, ly;
    pen(layer, win, K_V, x0, x1, y_pct, &s, COL_POS, 1, false, &lx, &ly);   // the area's edge
    pen(layer, win, K_SP, x0, x1, y_scale, &s, COL_SP, 1, true, &lx, &ly);
    if (any && pen(layer, win, K_P, x0, x1, y_scale, &s, COL_PV, 2, false, &lx, &ly))
        dot(layer, COL_PV, lx, ly);
    if (!trend_seconds())
        text(layer, "No samples yet", UI_IDLE, LV_TEXT_ALIGN_CENTER, x0, (y0 + y1) / 2 - 7, x1);
}

static void trend_view(void);

static void range_cb(lv_event_t *e)
{
    int d = lv_event_get_user_data(e) ? 1 : -1;
    if (ui_btn_disabled(e))
        return;   // at the end of the ranges: no message (Ryan, 22 Oct)
    win_i += d;
    if (win_i < 0) win_i = 0;
    if (win_i > 5) win_i = 5;
    trend_view();   // at once, not at the next 500 ms refresh
}

static void trend_build(lv_obj_t *scr)
{
    tr.plot = ui_card(scr, 10, 48, CARD_W, 160);
    lv_obj_add_event_cb(tr.plot, trend_draw, LV_EVENT_DRAW_MAIN_END, NULL);
    tr.up = ui_btn(scr, 361, 48, 109, 48, UI_SECONDARY, "", range_cb, (void *)1);
    ui_btn_arrow(tr.up, true);
    tr.range = ui_info(scr, 361, 104, 109, 48, "Range");
    tr.down = ui_btn(scr, 361, 160, 109, 48, UI_SECONDARY, "", range_cb, NULL);
    ui_btn_arrow(tr.down, false);
    tr.seen = UINT32_MAX;
    tr.win_seen = -1;
}

// Range card, Up / Down and the plot; the plot is redrawn when a plotted
// point completes (each second on 1 and 2 min, every 5 s on 10 min, 30 s
// on 1 hour) or the range changes, not at every new sample: a redraw costs
// about as much as a whole screen change, and touch waited on it each second.
static void trend_view(void)
{
    ui_set_text(tr.range, WIN_NAME[win_i]);
    ui_btn_enable(tr.up, win_i < 5);
    ui_btn_enable(tr.down, win_i > 0);
    uint32_t done = trend_seconds() / slot_s(WINS[win_i], (CARD_W - PL - PR) / 2 + 1);
    if (done != tr.seen || win_i != tr.win_seen) {
        tr.seen = done;
        tr.win_seen = win_i;
        lv_obj_invalidate(tr.plot);
    }
}

static void trend_refresh(void)
{
    trend_view();
    ui_status_line();
}

const ui_screen_t ui_scr_trend = { "Trend", trend_build, trend_refresh, NULL };
