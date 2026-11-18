#include <string.h>

#include "lvgl.h"
#include "clicks.h"
#include "stepper3.h"
#include "valve.h"
#include "valvepos.h"
#include "trend.h"
#include "ui.h"
#include "cfg.h"
#include "fixed.h"
#include "ipc_config.h"

/* --------------------------------------------------------------------------
 * Home. Pressure and Valve tiles; on the right the PID tile (KP, TI and KD
 * in use; a tap opens PID tuning: the operator does not tune, but sees the
 * values), the IP tile (the address and mask in use; a tap opens System
 * setup), Menu and STOP. There is no mode button: the header's panel shows
 * the mode, and each tile's bottom row changes the mode or sets its value:
 *   MANUAL     Pressure: Start AUTO (AUTO at the pressure shown, from where
 *              the valve is, no question). Valve: Go to.
 *   AUTO       Pressure: the SP box (the keypad; Set applies at once, no
 *              question). Valve: Stop AUTO (MANUAL, the valve
 *              stays where it is, no question).
 *   NOT HOMED  Valve: Home valve (Home or Set position). Start AUTO greyed.
 *   HOMING     Valve: the Go to box, greyed, "----".
 * A yellow edge round the Valve tile while the limit switch is pressed; a
 * valve fault's red edge wins. While the Disable switch is on, its range
 * (factory 5 to 50 %) is yellow on the valve bar.
 * The strip shows the status line, with the 5 min pressure sparkline on its
 * right as on the operator screens (ui.c). A tap on the sparkline opens
 * Trend.
 * ------------------------------------------------------------------------ */
#define TILE_W  167
#define TILE_H  160
// The bar as wide as the widest reading, so it sits right under it: the
// wider of "1013.2" and "100.00" at 40 px.
static int32_t bar_w(void)
{
    static int32_t w = 0;
    if (!w) {
        lv_point_t a, b;
        lv_text_get_size(&a, "1013.2", &ui_num_40, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        lv_text_get_size(&b, "100.00", &ui_num_40, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        w = a.x > b.x ? a.x : b.x;
    }
    return w;
}
#define BAR_W   bar_w()

typedef struct {
    lv_obj_t *tile, *reading, *bar, *box, *box_text, *box_chev, *sp_cap;
    int32_t bw;               // the bar's width: the tile's inside, 12 px each side (144; Valve 143)
    int32_t fill;             // bar fill to here, px; -1 no value (no marker)
    bool log;                 // pressure: decade lines; valve: 0/50/100
    int band;                 // valve: the Disable switch band marked yellow on the track, lo * 1000 + hi, 0 = off
} tile_t;

static tile_t pt, vt;
static lv_obj_t *pid_btn, *pid_nm[3], *pid_val[3], *start_btn, *stop_btn, *homev_btn;
static bool vt_fault;   // a valve fault (F-2, F-3): the Valve tile's red edge, over the limit's yellow

static void show_if(lv_obj_t *o, bool on)
{
    if (on == lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
        if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

// log2 in Q8 (as main_screen.c), for the log pressure bar.
static int32_t log2_q8(uint32_t v)
{
    int32_t n = 31;
    while (!(v & 0x80000000u)) {
        v <<= 1;
        n--;
    }
    uint64_t m = v;
    for (int i = 0; i < 8; i++) {
        m = (m * m) >> 31;
        n <<= 1;
        if (m >= (1ull << 32)) {
            m >>= 1;
            n |= 1;
        }
    }
    return n;
}

// The pressure as the screen shows it (1 dp), so a bar never moves while
// the number stands still.
static int32_t p_shown(int32_t p_mmbar)
{
    return p_mmbar < 0 ? 0 : (p_mmbar + 50) / 100 * 100;
}

// Pressure bar end lines, the same distance in from each end: half the
// wider of "0.1" and "1k", so that label meets its end of the bar and the
// decade lines stay evenly spaced.
static int32_t ppad(void)
{
    static int32_t pad = -1;
    if (pad < 0) {
        lv_point_t a, b;
        lv_text_get_size(&a, "0.1", &ui_font_12, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        lv_text_get_size(&b, "1k", &ui_font_12, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        pad = ((a.x > b.x ? a.x : b.x) + 1) / 2;
    }
    return pad;
}
#define PPAD ppad()

static int32_t pbar_x(int32_t p_mmbar)
{
    const int32_t lo = log2_q8(100), hi = log2_q8(1000000);   // 0.1, 1000 mbar
    p_mmbar = p_shown(p_mmbar);
    int32_t l = log2_q8(p_mmbar > 1 ? (uint32_t)p_mmbar : 1);
    int32_t x = PPAD + (l - lo) * (pt.bw - 2 * PPAD) / (hi - lo);
    return x < 0 ? 0 : x > pt.bw ? pt.bw : x;
}

static void bar_draw(lv_event_t *e)
{
    tile_t *t = lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(t->bar, &a);
    int32_t y1 = a.y1 + 3, y2 = y1 + 7;
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    const int32_t BW = t->bw;
    lv_area_t bar = { a.x1, y1, a.x1 + BW - 1, y2 };
    r.bg_color = UI_TRACK;
    lv_draw_rect(layer, &r, &bar);
    if (t->fill > 0) {
        bar.x2 = a.x1 + t->fill - 1;
        r.bg_color = UI_FILL;
        lv_draw_rect(layer, &r, &bar);
    }
    // The switch pass band (Disable switch), over the track and fill: where the
    // switch does not stop closing.
    if (t->band) {
        lv_area_t b = { a.x1 + (BW - 1) * (t->band / 1000) / 100, y1,
                        a.x1 + (BW - 1) * (t->band % 1000) / 100, y2 };
        r.bg_color = UI_YELLOW;
        lv_draw_rect(layer, &r, &b);
    }
    // Lines and end labels.
    lv_draw_label_dsc_t text;
    lv_draw_label_dsc_init(&text);
    text.font = &ui_font_12;
    text.color = UI_MUTED;
    r.bg_color = UI_TICK;
    static const char *const plab[5] = { "0.1", "1", "10", "100", "1k" };
    static const char *const vlab[3] = { "0", "50", "100" };
    int n = t->log ? 5 : 3;
    for (int k = 0; k < n; k++) {
        int32_t x = t->log ? PPAD + k * (BW - 2 * PPAD) / 4 : k * (BW - 1) / 2;
        lv_area_t tick = { a.x1 + x, y1, a.x1 + x, y2 };
        lv_draw_rect(layer, &r, &tick);
        const char *s = t->log ? plab[k] : vlab[k];
        lv_area_t l = { a.x1 + x - 20, y2 + 5, a.x1 + x + 20, y2 + 19 };
        text.align = LV_TEXT_ALIGN_CENTER;
        if (!t->log && k == 0) { l.x1 = a.x1; l.x2 = a.x1 + 40; text.align = LV_TEXT_ALIGN_LEFT; }
        if (!t->log && k == 2) { l.x1 = a.x2 - 40; l.x2 = a.x2; text.align = LV_TEXT_ALIGN_RIGHT; }
        text.text = s;
        lv_draw_label(layer, &text, &l);
    }
    if (t->fill >= 0) {
        int32_t x = t->fill - 1 < 0 ? 0 : t->fill - 1 > BW - 3 ? BW - 3 : t->fill - 1;
        // 2 px above the track and 1 below, clear of the axis labels.
        lv_area_t mark = { a.x1 + x, y1 - 2, a.x1 + x + 2, y2 + 1 };
        r.bg_color = UI_TEXT;
        lv_draw_rect(layer, &r, &mark);
    }
}

static void bar_set(tile_t *t, int32_t fill)
{
    if (fill != t->fill) {
        t->fill = fill;
        lv_obj_invalidate(t->bar);
    }
}

static void box_set(tile_t *t, bool active, const char *text)
{
    lv_obj_t *b = t->box;
    // Editable: white with a 1 px outline and the chevron, as a settings
    // field, darker while pressed.
    lv_color_t bg = active ? UI_CARD : lv_color_hex(0xEEF2F6);
    if (!lv_color_eq(lv_obj_get_style_bg_color(b, 0), bg)) {
        lv_obj_set_style_bg_color(b, bg, 0);
        lv_obj_set_style_bg_color(b, active ? UI_PRESS : bg, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(b, active ? 1 : 0, 0);
        lv_obj_set_style_pad_all(b, active ? -1 : 0, 0);   // children keep their place
        lv_obj_set_style_radius(b, 4, 0);
        lv_obj_set_width(t->box_text, active ? 60 : 72);
        if (active) lv_obj_remove_flag(t->box_chev, LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_add_flag(t->box_chev, LV_OBJ_FLAG_HIDDEN);
    }
    // A value too wide for the box (a valve past 100 % once 100 % travel is
    // shortened) at 12 px, on the same baseline.
    lv_point_t sz;
    lv_text_get_size(&sz, text, &ui_num_16, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const lv_font_t *f = sz.x <= (active ? 60 : 72) ? &ui_num_16 : &ui_font_12;
    if (lv_obj_get_style_text_font(t->box_text, 0) != f) {
        lv_obj_set_style_text_font(t->box_text, f, 0);
        int32_t a16 = lv_font_get_line_height(&ui_num_16) - ui_num_16.base_line;
        lv_obj_set_y(t->box_text, 11 + a16 - (lv_font_get_line_height(f) - f->base_line));
    }
    ui_set_color(t->box_text, active ? UI_TEXT : UI_IDLE);
    ui_set_text(t->box_text, text);
}

// The tile's 3 px edge: red for a fault (the card style's colour), yellow
// for a warning (the limit switch pressed), none.
static void edge_set(tile_t *t, bool on, lv_color_t c)
{
    int32_t w = on ? 3 : 0;
    if (lv_obj_get_style_border_width(t->tile, 0) != w) {
        lv_obj_set_style_border_width(t->tile, w, 0);
        lv_obj_set_style_pad_all(t->tile, -w, 0);   // children keep their place
    }
    if (on && !lv_color_eq(lv_obj_get_style_border_color(t->tile, 0), c))
        lv_obj_set_style_border_color(t->tile, c, 0);
}

static void alarm_set(tile_t *t, bool on) { edge_set(t, on, UI_FAULT); }

static void make_tile(lv_obj_t *scr, tile_t *t, int32_t x, const char *cap, const char *unit,
                      const char *sp, bool log, lv_event_cb_t box_cb)
{
    t->log = log;
    t->fill = -1;
    // Halves of the 343 area as on every page, 168 + 8 + 167 (common HMI template).
    int32_t w = x == 10 ? 168 : TILE_W, r = w - 12;   // right edge of the unit, reading, bar and box
    t->bw = r - 12;
    t->tile = ui_card(scr, x, 48, w, TILE_H);
    // Caption and unit 16 px (12 px is too small).
    ui_label(t->tile, 12, 6, 90, &ui_font_16, UI_MUTED, LV_TEXT_ALIGN_LEFT, cap);
    ui_label(t->tile, r - 80, 6, 80, &ui_font_16, UI_MUTED, LV_TEXT_ALIGN_RIGHT, unit);
    // The reading, 40 px; "1013.2" just fits, right edge on the bar's.
    t->reading = ui_label(t->tile, 0, 34, r, &ui_num_40, UI_TEXT, LV_TEXT_ALIGN_RIGHT, "----");
    t->bar = lv_obj_create(t->tile);
    lv_obj_remove_style_all(t->bar);
    lv_obj_remove_flag(t->bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(t->bar, 12, 76);   // 14 px from the digits, as the caption is above them
    lv_obj_set_size(t->bar, t->bw, 30);
    lv_obj_add_event_cb(t->bar, bar_draw, LV_EVENT_DRAW_MAIN, t);
    // SP / Go to at the button text size, 16 px.
    // The bottom row is centred on the Menu button beside it.
    t->sp_cap = ui_label(t->tile, 12, 124, 54, &ui_font_16, UI_MUTED, LV_TEXT_ALIGN_LEFT, sp);
    t->box = lv_obj_create(t->tile);
    lv_obj_remove_style_all(t->box);
    lv_obj_remove_flag(t->box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(t->box, r - 88, 114);   // wide enough for "100.00" and the chevron
    lv_obj_set_size(t->box, 88, 40);
    lv_obj_set_style_bg_opa(t->box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(t->box, lv_color_hex(0xEEF2F6), 0);
    lv_obj_set_style_radius(t->box, 4, 0);
    lv_obj_set_style_border_color(t->box, UI_TICK, 0);
    lv_obj_add_event_cb(t->box, box_cb, LV_EVENT_CLICKED, NULL);
    t->box_text = ui_label(t->box, 4, 11, 72, &ui_num_16, UI_IDLE, LV_TEXT_ALIGN_RIGHT, "");
    t->box_chev = ui_chev(t->box);
    lv_obj_add_flag(t->box_chev, LV_OBJ_FLAG_HIDDEN);
}

/* Actions ---------------------------------------------------------------- */
static const char *not_manual_why(void)
{
    if (valve_motion_blocked()) return "Reset the fault first";
    if (valve_mode() == VALVE_CALIBRATING) return "Homing in progress: STOP to cancel";
    if (valve_auto()) return "Stop AUTO first";
    return "Home the valve first";
}

// AUTO ------------------------------------------------------------------
static int32_t pv_milli(void) { return clicks_state()->p_mmbar; }

static void to_manual(void)
{
    const char *why;
    if (!valve_to_manual(&why))
        ui_msg(why);
}

// A typed setpoint applies at once, however large the change: no "Change
// setpoint to X mB?" question.
static bool sp_set(int32_t v10, const char **why)
{
    if (!valve_set_target(v10 * 100, why))
        return false;
    ui_net_note_drop();   // a network setpoint's note is out of date now
    return true;
}

static void keypad_sp(void)
{
    ui_kp_t k = { .title = "Setpoint, mB", .dec = 1, .min = 0,   // the range in the field's corners
                  .max = valve_range_mmbar() / 100, .live_label = "Pressure, mB", .live_milli = pv_milli,
                  .live_dec = 1, .on_set = sp_set };
    ui_keypad_ex(&k);
}

// AUTO's regulation words; unit adds " mB".
static const char *auto_text(char *t, size_t n, bool unit)
{
    char a[16];
    int32_t sp;
    valve_target(&sp);
    ui_fmt1(a, sizeof a, sp);
    const char *u = unit ? " mB" : "";
    switch (valve_regulation()) {
    case CTRL_REG_STABLE:     lv_snprintf(t, n, "Stable at %s%s", a, u); break;
    case CTRL_REG_BAND:       lv_snprintf(t, n, "In band, %s%s", a, u); break;
    case CTRL_REG_REGULATING: lv_snprintf(t, n, "Regulating to %s%s", a, u); break;
    default:                  lv_snprintf(t, n, "AUTO at %s%s, no reading", a, u); break;
    }
    return t;
}

static void warn_ok_event(lv_event_t *e)
{
    (void)e;
    valve_warning_clear();
}

static void target_event(lv_event_t *e)
{
    (void)e;
    if (valve_auto())
        keypad_sp();
    else
        ui_msg(valve_mode() == VALVE_MANUAL ? "Setpoint is used in AUTO"
                                            : not_manual_why());
}

static bool goto_set(int32_t v100, const char **why)
{
    return valve_goto_pct100(v100, why);   // its reason as valve.c gives it
}

static void goto_event(lv_event_t *e)
{
    (void)e;
    if (valve_mode() != VALVE_MANUAL || valve_motion_blocked()) {
        ui_msg(not_manual_why());
        return;
    }
    ui_keypad("Go to, %", NULL, 0, 10000, "Valve, %", valve_pct_milli, goto_set);   // no hint line (template)
}

static void calibrate(void)
{
    const char *why;
    // Home stays where it is: the strip shows the
    // steps on every screen.
    if (!valve_calibrate(&why))
        ui_msg(valve_motion_blocked() ? "Reset the fault first" : why);
}

// Home valve without a question (the start-up question is the question).
void ui_home_valve(void) { calibrate(); }

/* Home and Set position are two jobs, kept apart on every screen: Home
 * finds the limit switch (0 % is the back-off above it); Set as % takes the
 * position from the operator, not verified by homing, and logs it. It is
 * one keypad (Set as 0 % and Use last saved are typed: 0, or the last saved
 * value on its line). */
static bool setpos_set(int32_t v100, const char **why)
{
    if (valve_motion_blocked()) {
        *why = "Reset the fault first";
        return false;
    }
    if (!valve_set_position(v100, why))
        return false;
    ui_note("Position set. Not verified by homing.");
    return true;
}

void ui_set_position_ask(void)
{
    static char info[40];
    int32_t q;
    if (valvepos_offer(&q)) {
        char v[16];
        int32_t pm = valve_pos_max();
        ui_fmt2(v, sizeof v, ((int64_t)q * 100000 + (q >= 0 ? pm / 2 : -pm / 2)) / pm);
        lv_snprintf(info, sizeof info, "Last saved %s %%", v);
    } else {
        info[0] = 0;
    }
    ui_kp_t k = { 0 };
    k.title = "Set position as, %";
    k.info = info[0] ? info : NULL;
    k.dec = 2;
    k.min = 0;
    k.max = 10000;
    k.on_set = setpos_set;
    ui_keypad_ex(&k);
}

static char home_q[128];

int ui_home_minutes(void)
{
    return (int)((valve_cal_budget_s() + 59) / 60);
}

static const char *home_minutes(void)
{
    static char t[24];
    lv_snprintf(t, sizeof t, "up to %d min", ui_home_minutes());
    return t;
}

// Home only (Valve position's Home tab, the start-up question).
void ui_home_ask(void)
{
    lv_snprintf(home_q, sizeof home_q, "Home the valve? It closes onto the limit switch to find 0 %%, %s.",
                home_minutes());
    ui_confirm(home_q, "Home valve", calibrate);
}

// Home valve (Home's Valve tile) while not homed: the two jobs side by side.
void ui_position_ask(void)
{
    // Homing closes the valve: said here too.
    lv_snprintf(home_q, sizeof home_q, "Valve position unknown. Home the valve (it closes, %s) or set the position?", home_minutes());
    ui_ask3(home_q, "Cancel", NULL, "Set as %", ui_set_position_ask, "Home valve", calibrate);
}

// Stop AUTO (the Valve tile, AUTO): MANUAL, the valve stays where it is,
// no question.
static void stop_event(lv_event_t *e)
{
    if (ui_btn_disabled(e) || !valve_auto()) {
        ui_msg("Not in AUTO");
        ui_refresh_soon();
        return;
    }
    to_manual();
    ui_refresh_soon();   // the tiles and header at once, not at the next 0.5 s refresh
}

// Home valve (the Valve tile, not homed).
static void homev_event(lv_event_t *e)
{
    (void)e;
    if (valve_mode() != VALVE_NOT_CAL)
        ui_refresh_soon();   // homed meanwhile (the network): the row changes
    else if (valve_motion_blocked())
        ui_msg("Reset the fault first");
    else
        ui_position_ask();
}

// PID tile ---------------------------------------------------------------
static void pid_event(lv_event_t *e)
{
    (void)e;
    ui_open(&ui_scr_tune);   // on its PID tab
}

// Three figures, four characters at most: 0.40, 4.00, 12.5, 100.
static const char *fmt3(char *b, size_t n, int32_t v100)
{
    if (v100 < 1000) return fixed_fmt(b, n, v100, 2);
    if (v100 < 10000) return fixed_fmt(b, n, (v100 + 5) / 10, 1);
    return fixed_fmt(b, n, (v100 + 50) / 100, 0);
}

// Three columns, the name over the value, 12 px (16 px values run
// together in a 35 px column), thin rules between.
static void pid_make(lv_obj_t *scr, int32_t y)
{
    static const char *const nm[3] = { "KP", "TI", "KD" };
    pid_btn = ui_btn(scr, 361, y, 109, 48, UI_SECONDARY, "", pid_event, NULL);
    for (int i = 0; i < 3; i++) {
        int32_t x = 1 + i * 36;
        pid_nm[i] = ui_label(pid_btn, x, 8, 35, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_CENTER, nm[i]);
        pid_val[i] = ui_label(pid_btn, x, 25, 35, &ui_font_12, UI_TEXT, LV_TEXT_ALIGN_CENTER, "");
        if (i) {
            lv_obj_t *r = lv_obj_create(pid_btn);
            lv_obj_remove_style_all(r);
            lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_pos(r, x - 1, 9);
            lv_obj_set_size(r, 1, 28);
            lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(r, UI_TICK, 0);
        }
    }
}

// KP, TI and KD of the zone in use, with no zone number.
static void pid_refresh(void)
{
    const int set = valve_pid_set();
    char b[16];
    for (int i = 0; i < 3; i++) {
        int32_t v = cfg_tune(cfg_pid_key(set, i));
        // TI 0 turns the integral off.
        ui_set_text(pid_val[i], i == 1 && v == 0 ? "Off" : fmt3(b, sizeof b, v));
    }
}

// Start AUTO --------------------------------------------------------------
static bool reading_ok(void)
{
    const clicks_state_t *s = clicks_state();
    return s->pm_found && s->reading_ok && !s->boost_tripped;
}

// AUTO at the pressure as shown (0.1 mB), from where the valve is, with no
// question.
static void start_event(lv_event_t *e)
{
    const char *why;
    if (ui_btn_disabled(e) || valve_mode() != VALVE_MANUAL) {
        ui_msg(valve_auto() ? "Already in AUTO" : !reading_ok() ? "No pressure signal"
               : valve_fault_count() ? "Reset the fault first"
               : valve_mode() == VALVE_MANUAL ? "Limit switch pressed: open the valve until it releases" : not_manual_why());
        ui_refresh_soon();
        return;
    }
    const int32_t at = p_shown(clicks_state()->p_mmbar);
    if (!valve_start_auto_at(at, &why)) {
        ui_msg(why);
    } else {
        // The setpoint is the pressure now, whatever it was; the message
        // says the old one was replaced.
        char t[64], v[16];
        lv_snprintf(t, sizeof t, "AUTO started at %s mB, the pressure now", ui_fmt1(v, sizeof v, at));
        ui_note(t);
    }
    ui_refresh_soon();   // the tiles and header at once, not at the next 0.5 s refresh
}

static void menu_event(lv_event_t *e)
{
    (void)e;
    ui_open(&ui_scr_menu);
}

// IP tile: a grey "IP address" over the address and prefix the link uses,
// 192.168.1.100/24, from start-up (a new one waits for a restart), 12 px. A
// tap opens System. Trend is on Menu and on the sparkline.
static lv_obj_t *ip_val;

static void ip_event(lv_event_t *e)
{
    (void)e;
    ui_open(&ui_scr_namenet);
}

static void ip_make(lv_obj_t *scr, int32_t y)
{
    lv_obj_t *b = ui_btn(scr, 361, y, 109, 48, UI_SECONDARY, "", ip_event, NULL);
    // Centred.
    ui_label(b, 1, 8, 107, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_CENTER, "IP address");
    ip_val = ui_label(b, 1, 25, 107, &ui_font_12, UI_TEXT, LV_TEXT_ALIGN_CENTER, "");
    // As wide as its text and centred on the tile, so 192.168.100.200/24
    // (a pixel wider than the tile's inside) runs to the edges, not wraps.
    lv_obj_set_width(ip_val, LV_SIZE_CONTENT);
    lv_obj_align(ip_val, LV_ALIGN_TOP_MID, 0, 25);
}

// Prefix length of a dotted mask: 255.255.255.0 is 24.
static int ip_prefix(const char *m)
{
    int n = 0;
    for (int o = 0; o < 4 && m; o++) {
        int v = 0;
        while (*m >= '0' && *m <= '9')
            v = v * 10 + (*m++ - '0');
        for (int b = 7; b >= 0; b--)
            n += (v >> b) & 1;
        m = *m == '.' ? m + 1 : NULL;
    }
    return n;
}

static void ip_refresh(void)
{
    char ip[16], mask[16], t[24];
    cfg_net_boot(ip, mask);
    lv_snprintf(t, sizeof t, "%s/%d", ip, ip_prefix(mask));
    ui_set_text(ip_val, t);
}

void ui_reset_fault_event(lv_event_t *e)
{
    (void)e;
    char t[96];
    if (ui_reset_faults(t, sizeof t))
        ui_msg(t);
}

/* Reset fault, and what came of it: false when every fault
 * was reset, else the text: "F-1 not reset: cause still present", or with
 * another one reset "F-3 reset. F-1 not reset: cause still present". */
bool ui_reset_faults(char *t, size_t n)
{
    static const char *const tags[3] = { "F-1", "F-2", "F-3" };
    char left[40], done[16] = "";
    bool was[3];
    for (int i = 0; i < 3; i++)
        was[i] = valve_fault_is(tags[i]);
    valve_reset(left, sizeof left);
    if (!left[0])
        return false;
    for (int i = 0; i < 3; i++)
        if (was[i] && !valve_fault_is(tags[i])) {
            size_t l = strlen(done);
            lv_snprintf(done + l, sizeof done - l, "%s%s", l ? ", " : "", tags[i]);
        }
    lv_snprintf(t, n, "%s%s%s not reset: cause still present", done, done[0] ? " reset. " : "", left);
    return true;
}

/* Build and refresh ------------------------------------------------------ */
static void home_build(lv_obj_t *scr)
{
    make_tile(scr, &pt, 10, "Pressure", "mB", "SP", true, target_event);
    make_tile(scr, &vt, 186, "Valve", "%", "Go to", false, goto_event);
    // The PID tile on top, the IP tile, then Menu above STOP.
    pid_make(scr, 48);
    ip_make(scr, 104);
    ui_btn(scr, 361, 160, 109, 48, UI_NAV, LV_SYMBOL_BARS "  Menu", menu_event, NULL);
    // The mode buttons on the tiles' bottom rows: Start AUTO outside AUTO, filled dark blue with white
    // lettering, Stop AUTO in AUTO, outlined blue; Home valve while not
    // homed, filled. Under the bar and as wide (sized to the widest reading).
    start_btn = ui_btn(pt.tile, 12, 114, pt.bw, 40, UI_PRIMARY, "Start AUTO", start_event, NULL);
    stop_btn = ui_btn(vt.tile, 12, 114, vt.bw, 40, UI_SECONDARY, "Stop AUTO", stop_event, NULL);
    ui_btn_outline(stop_btn, true);
    homev_btn = ui_btn(vt.tile, 12, 114, vt.bw, 40, UI_PRIMARY, "Home valve", homev_event, NULL);
}

// A tile's reading: at the big font on its baseline, or at 26 px on the
// same baseline when it would not fit the tile (a valve past 100 % once
// 100 % travel is shortened, a reading past 9999.9).
static void reading_set(lv_obj_t *l, const lv_font_t *big, const lv_font_t *small, const char *t)
{
    lv_point_t sz;
    lv_text_get_size(&sz, t, big, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const lv_font_t *f = sz.x <= lv_obj_get_style_width(l, 0) ? big : small;
    if (lv_obj_get_style_text_font(l, 0) != f) {
        lv_obj_set_style_text_font(l, f, 0);
        int32_t a40 = lv_font_get_line_height(&ui_num_40) - ui_num_40.base_line;
        int32_t a = lv_font_get_line_height(f) - f->base_line;
        lv_obj_set_y(l, 34 + a40 - a);
    }
    ui_set_text(l, t);
}

// Valve reading and bar, at 10 Hz.
static void home_fast(void)
{
    valve_mode_t m = valve_mode();
    char buf[16];
    bool cal = valve_calibrated() && m != VALVE_CALIBRATING;
    if (m == VALVE_CALIBRATING) {
        // "Homing" at 26 px on the reading's baseline, right-aligned as the
        // number is.
        reading_set(vt.reading, &ui_font_26, &ui_font_26, "Homing");
        ui_set_color(vt.reading, UI_MUTED);
    } else {
        reading_set(vt.reading, &ui_num_40, &ui_num_26, cal ? ui_fmt2(buf, sizeof buf, valve_pct_milli()) : "----");
        ui_set_color(vt.reading, cal && !valve_auto() ? UI_TEXT : UI_IDLE);   // grey in AUTO
    }
    int32_t p = ui_pct_shown(valve_pct_milli());
    bar_set(&vt, cal ? p * vt.bw / 10000 : -1);
    // Limit switch pressed (the NC contact open): a yellow edge round the
    // Valve tile at 10 Hz, whatever the mode; a valve fault's red wins.
    bool lim = stepper3_limit_pressed();   // passed in the pass band or not
    // Above 100 % too.
    bool above = cal && stepper3_position() > valve_pos_max();
    edge_set(&vt, vt_fault || lim || above, vt_fault ? UI_FAULT : UI_YELLOW);
    // The pass band on the valve bar while it is on (Motor settings > Override > Disable switch).
    int band = valve_limit_band() ? valve_pass_lo() * 1000 + valve_pass_hi() : 0;   // To >= 1, so on is never 0
    if (band != vt.band) {
        vt.band = band;
        lv_obj_invalidate(vt.bar);
    }
}

static void home_refresh(void)
{
    pid_refresh();
    ip_refresh();
    const clicks_state_t *s = clicks_state();
    valve_mode_t m = valve_mode();
    char buf[32];

    // Pressure.
    bool ok = s->pm_found && s->reading_ok && !s->boost_tripped;
    reading_set(pt.reading, &ui_num_40, &ui_num_26, ok ? ui_fmt1(buf, sizeof buf, s->p_mmbar) : "----");
    ui_set_color(pt.reading, ok ? UI_TEXT : UI_FAULT);
    bar_set(&pt, ok ? pbar_x(s->p_mmbar) : -1);
    alarm_set(&pt, !ok || valve_fault_is("F-1"));
    int32_t sp;
    bool auto_on = valve_auto();
    box_set(&pt, auto_on, valve_target(&sp) ? ui_fmt1(buf, sizeof buf, sp) : "Not set");
    // AUTO: the SP field. Otherwise Start AUTO, greyed while it would be
    // refused (a press says why).
    show_if(pt.box, auto_on);
    show_if(pt.sp_cap, auto_on);
    show_if(start_btn, !auto_on);
    // Greyed with the switch pressed away from the homed point too, as it
    // would be refused.
    ui_btn_enable(start_btn, m == VALVE_MANUAL && !valve_fault_count() && ok && valve_switch() != 2);

    vt_fault = valve_fault_is("F-2") || valve_fault_is("F-3");
    home_fast();
    bool manual = m == VALVE_MANUAL && !valve_motion_blocked();
    // Where the valve is going, greyed when not editable; "----" while
    // homing.
    bool have = valve_calibrated() && m != VALVE_CALIBRATING;
    box_set(&vt, manual, have ? ui_fmt2(buf, sizeof buf, valve_target_pct_milli()) : "----");

    // The Valve tile's row: Stop AUTO in AUTO, Home valve while not homed,
    // otherwise the Go to box.
    bool stop_on = auto_on, homev_on = m == VALVE_NOT_CAL;
    show_if(stop_btn, stop_on);
    show_if(homev_btn, homev_on);
    ui_btn_enable(homev_btn, !valve_motion_blocked());   // greyed with F-2/F-3 latched
    show_if(vt.box, !stop_on && !homev_on);
    show_if(vt.sp_cap, !stop_on && !homev_on);

    ui_status_line();
}

// Status line for Home and the operator screens; the frame puts faults
// (with Reset fault, on every screen) and messages over it.
void ui_status_line(void)
{
    valve_mode_t m = valve_mode();
    int step = valve_cal_step();
    int nf = valve_fault_count();
    int32_t q;
    if (valve_question(&q)) {
        char t[64], v[16];
        lv_snprintf(t, sizeof t, "W-1 Limit switch pressed at %s %%",
                    ui_fmt2(v, sizeof v, valve_question_milli()));
        ui_strip_set(UI_STRIP_WARN, NULL, t);
    } else if (m == VALVE_NOT_CAL) {
        // A plain info line with the valve icon, as the others.
        ui_strip_set(UI_STRIP_INFO, UI_ICON_VALVE, "Valve not homed");
    } else if (m == VALVE_CALIBRATING) {
        static const char *const what[5] = { "Homing: moving off the switch", "Homing 1 of 4: fast close",
                                             "Homing 2 of 4: backing off",
                                             "Homing 3 of 4: slow close",
                                             "Homing 4 of 4: return" };
        // The fast close is most of a homing: how long it can take, so a
        // long step does not look stuck.
        char hs[64];
        lv_snprintf(hs, sizeof hs, "%s, up to %lu min", what[1], (unsigned long)((valve_cal_fast_s() + 59) / 60));
        ui_strip_set(UI_STRIP_INFO, LV_SYMBOL_REFRESH, step == 1 ? hs : what[step < 0 ? 0 : step]);
    } else if (m == VALVE_MANUAL && valve_warning()) {
        // No PAUSED state: W-2 drops to MANUAL; shown until OK.
        if (!nf)
            ui_strip_action("OK", NULL, warn_ok_event);
        ui_strip_set(UI_STRIP_WARN, NULL, valve_warning());
    } else if (m == VALVE_AUTO) {
        // The regulation words, moving or not: AUTO moves the valve all the time.
        // The valve at an end and the pressure off the setpoint: a caution
        // line; AUTO carries on, nothing latches.
        char t[64], a[16];
        int32_t sp = 0;
        valve_target(&sp);
        switch (valve_auto_pinned()) {
        case 1:
            lv_snprintf(t, sizeof t, "Valve held at AUTO min %s %%", fixed_fmt(a, sizeof a, cfg_tune(CFG_OUTMIN), 1));
            ui_strip_set(UI_STRIP_CAUTION, NULL, t);
            break;
        case 2:
        case 3:
            lv_snprintf(t, sizeof t, "Valve at %s %%: %s mB not reached", valve_auto_pinned() == 2 ? "100" : "0",
                        ui_fmt1(a, sizeof a, sp));
            ui_strip_set(UI_STRIP_CAUTION, NULL, t);
            break;
        default:
            ui_strip_set(UI_STRIP_INFO, UI_ICON_MODE, auto_text(t, sizeof t, true));
        }
    } else if (stepper3_busy()) {
        char t[40], v[16];
        lv_snprintf(t, sizeof t, "Moving to %s %%", ui_fmt2(v, sizeof v, valve_target_pct_milli()));
        ui_strip_set(UI_STRIP_INFO, UI_ICON_MODE, t);
    } else {
        // Idle: the valve icon, Valve idle and the pressure sparkline. How
        // the position was set (entered, last saved) is on Home valve. The
        // switch pressed: on it at the homed point, or away from it after
        // Skip.
        int sw = valve_switch();
        char t[80], v[16];
        // A position set on the keypad, by Set as closed or from the saved
        // one, not homed: said until homed.
        valve_ref_t ref = valve_reference();
        const char *idle = ref == VALVE_REF_HAND || ref == VALVE_REF_SAVED ? "Position unverified" : "Valve idle";
        if (sw == 2) {
            lv_snprintf(t, sizeof t, "Limit switch pressed at %s %%", ui_fmt2(v, sizeof v, valve_pct_milli()));
        } else if (sw != 1 && stepper3_position() > valve_pos_max()) {
            // Jogged past 100 %, or the travel shortened under it.
            ui_strip_set(UI_STRIP_CAUTION, NULL, "Valve above 100 %");
            return;
        }
        // Disable switch on: the yellow range on Home's valve bar says so,
        // not the status line.
        ui_strip_set(UI_STRIP_INFO, UI_ICON_MODE, sw == 1 ? "On the switch" : sw == 2 ? t : idle);
    }
}

const ui_screen_t ui_scr_home = { "Home", home_build, home_refresh, home_fast };

/* --------------------------------------------------------------------------
 * Menu: nine tiles in three rows of three, an icon over each name:
 *   Trend   Events       PID
 *   Motor   Valve        Pressure sensor
 *   System  Factory reset  Diagnostics
 * Factory reset opens no page: it asks, then resets (ui_factory_reset_ask,
 * ui_settings.c). Three columns of 148 (x 10, 166, 322): the template's 109
 * columns are too narrow for the names on one line.
 * ------------------------------------------------------------------------ */
typedef struct {
    const char *name, *icon;
    const ui_screen_t *scr;
} menu_item_t;

static const menu_item_t menu_items[9] = {
    { "Trend", UI_ICON_TREND, &ui_scr_trend },
    { "Events", LV_SYMBOL_LIST, &ui_scr_faults },
    { "PID", UI_ICON_PID, &ui_scr_tune },
    { "Motor", UI_ICON_MOTOR, &ui_scr_motor },               // Jog and the drive settings
    { "Valve", UI_ICON_VALVE, &ui_scr_valve_set },           // position, homing, ignore the switch
    { "Pressure sensor", UI_ICON_GAUGE, &ui_scr_sensor_cal },
    { "System", LV_SYMBOL_SETTINGS, &ui_scr_namenet },
    { "Factory reset", LV_SYMBOL_REFRESH, NULL },            // no page: asks, then resets
    { "Diagnostics", LV_SYMBOL_EYE_OPEN, &ui_scr_diag },
};

static void menu_tile_event(lv_event_t *e)
{
    const ui_screen_t *scr = lv_event_get_user_data(e);
    if (scr)
        ui_open(scr);
    else
        ui_factory_reset_ask();
}

static void menu_build(lv_obj_t *scr)
{
    for (int i = 0; i < 9; i++) {
        const menu_item_t *m = &menu_items[i];
        // Plain (centred) tiles, no chevron: the icon marks what opens.
        lv_obj_t *b = ui_btn(scr, 10 + (i % 3) * 156, 48 + (i / 3) * 56, 148, 48, UI_SECONDARY, m->name,
                             menu_tile_event, (void *)m->scr);
        ui_btn_icon(b, m->icon);
    }
}

static void menu_refresh(void)
{
    ui_status_line();
}

const ui_screen_t ui_scr_menu = { "Menu", menu_build, menu_refresh, NULL };
