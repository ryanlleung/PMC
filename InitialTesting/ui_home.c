#include <string.h>

#include "lvgl.h"
#include "clicks.h"
#include "stepper3.h"
#include "valve.h"
#include "valvepos.h"
#include "trend.h"
#include "ui.h"

/* --------------------------------------------------------------------------
 * Home (simulator v63 buildCtrl). Pressure and Valve tiles, the mode button,
 * Valve and Menu. AUTO (stage 8): the first Start AUTO asks for a target and
 * Set starts AUTO; later ones confirm the target kept or offer New target.
 * In AUTO the Target box opens the keypad (changes over 5 mB ask first) and
 * the Go to box reads "AUTO", flat. The strip shows the status line, with
 * the 5 min pressure sparkline on its right as on the operator screens
 * (ui.c, Ryan 21 Oct). A tap on the sparkline opens Trend.
 * ------------------------------------------------------------------------ */
#define TILE_W  167
#define TILE_H  160
#define BAR_W   143

typedef struct {
    lv_obj_t *tile, *reading, *bar, *box, *box_text, *box_chev;
    int32_t fill;             // bar fill to here, px; -1 no value (no marker)
    bool log;                 // pressure: decade lines; valve: 0/50/100
} tile_t;

static tile_t pt, vt;
static lv_obj_t *mode_btn, *valve_btn;

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

int32_t ui_log_px(int32_t p_mmbar, int32_t w)
{
    const int32_t lo = log2_q8(100), hi = log2_q8(1000000);   // 0.1, 1000 mbar
    p_mmbar = p_shown(p_mmbar);
    int32_t l = log2_q8(p_mmbar > 1 ? (uint32_t)p_mmbar : 1);
    int32_t x = (l - lo) * w / (hi - lo);
    return x < 0 ? 0 : x > w ? w : x;
}

// Pressure bar end lines, the same distance in from each end: half the
// wider of "0.1" and "1k", so that label meets its end of the bar (Ryan,
// 16 Oct) and the decade lines stay evenly spaced.
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
    int32_t x = PPAD + (l - lo) * (BAR_W - 2 * PPAD) / (hi - lo);
    return x < 0 ? 0 : x > BAR_W ? BAR_W : x;
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
    lv_area_t bar = { a.x1, y1, a.x1 + BAR_W - 1, y2 };
    r.bg_color = UI_TRACK;
    lv_draw_rect(layer, &r, &bar);
    if (t->fill > 0) {
        bar.x2 = a.x1 + t->fill - 1;
        r.bg_color = UI_FILL;
        lv_draw_rect(layer, &r, &bar);
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
        int32_t x = t->log ? PPAD + k * (BAR_W - 2 * PPAD) / 4 : k * (BAR_W - 1) / 2;
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
        int32_t x = t->fill - 1 < 0 ? 0 : t->fill - 1 > BAR_W - 3 ? BAR_W - 3 : t->fill - 1;
        // 2 px above the track and 1 below, clear of the axis labels (Ryan, 22 Oct).
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
    lv_color_t bg = lv_color_hex(active ? 0xE9F0F6 : 0xEEF2F6);
    if (!lv_color_eq(lv_obj_get_style_bg_color(b, 0), bg)) {
        lv_obj_set_style_bg_color(b, bg, 0);
        lv_obj_set_style_border_width(b, active ? 3 : 0, 0);
        lv_obj_set_style_radius(b, 4, 0);
        lv_obj_set_width(t->box_text, active ? 62 : 72);
        if (active) lv_obj_remove_flag(t->box_chev, LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_add_flag(t->box_chev, LV_OBJ_FLAG_HIDDEN);
    }
    ui_set_color(t->box_text, active ? UI_TEXT : UI_IDLE);
    ui_set_text(t->box_text, text);
}

static void alarm_set(tile_t *t, bool on)
{
    int32_t w = on ? 3 : 0;
    if (lv_obj_get_style_border_width(t->tile, 0) != w) {
        lv_obj_set_style_border_width(t->tile, w, 0);
        lv_obj_set_style_pad_all(t->tile, -w, 0);   // children keep their place
    }
}

static void make_tile(lv_obj_t *scr, tile_t *t, int32_t x, const char *cap, const char *unit,
                      const char *sp, bool log, lv_event_cb_t box_cb)
{
    t->log = log;
    t->fill = -1;
    t->tile = ui_card(scr, x, 48, TILE_W, TILE_H);
    // Caption and unit 16 px (Ryan, 21 Oct: 12 px was too small).
    ui_label(t->tile, 12, 6, 90, &ui_font_16, UI_MUTED, LV_TEXT_ALIGN_LEFT, cap);
    ui_label(t->tile, 75, 6, 80, &ui_font_16, UI_MUTED, LV_TEXT_ALIGN_RIGHT, unit);
    // The reading, 40 px (Ryan 16 Oct: bigger once the sparklines left the
    // tiles); "1013.2" just fits, right edge on the bar's.
    t->reading = ui_label(t->tile, 0, 34, 12 + BAR_W, &ui_num_40, UI_TEXT, LV_TEXT_ALIGN_RIGHT, "----");
    t->bar = lv_obj_create(t->tile);
    lv_obj_remove_style_all(t->bar);
    lv_obj_remove_flag(t->bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(t->bar, 12, 76);   // 14 px from the digits, as the caption is above them
    lv_obj_set_size(t->bar, BAR_W, 30);
    lv_obj_add_event_cb(t->bar, bar_draw, LV_EVENT_DRAW_MAIN, t);
    ui_label(t->tile, 12, 128, 50, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT, sp);
    t->box = lv_obj_create(t->tile);
    lv_obj_remove_style_all(t->box);
    lv_obj_remove_flag(t->box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(t->box, 67, 116);   // wide enough for "100.00" and the chevron
    lv_obj_set_size(t->box, 88, 40);
    lv_obj_set_style_bg_opa(t->box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(t->box, lv_color_hex(0xEEF2F6), 0);
    lv_obj_set_style_radius(t->box, 4, 0);
    lv_obj_set_style_border_side(t->box, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(t->box, UI_BLUE, 0);
    lv_obj_add_event_cb(t->box, box_cb, LV_EVENT_CLICKED, NULL);
    t->box_text = ui_label(t->box, 4, 11, 72, &ui_num_16, UI_IDLE, LV_TEXT_ALIGN_RIGHT, "");
    t->box_chev = ui_label(t->box, 70, 12, 0, &lv_font_montserrat_14, UI_BLUE, LV_TEXT_ALIGN_LEFT,
                           LV_SYMBOL_RIGHT);
    lv_obj_add_flag(t->box_chev, LV_OBJ_FLAG_HIDDEN);
}

/* Actions ---------------------------------------------------------------- */
static const char *not_manual_why(void)
{
    if (valve_motion_blocked()) return "Reset the fault first";
    if (valve_mode() == VALVE_CALIBRATING) return "Homing: STOP to cancel";
    if (valve_auto()) return "Switch to MANUAL first";
    return "Home the valve first";
}

// AUTO ------------------------------------------------------------------
static int32_t pv_milli(void) { return clicks_state()->p_mmbar; }

static const char *range_info(void)
{
    static char info[32];
    char r[16];
    lv_snprintf(info, sizeof info, "Allowed 0 to %s", ui_fmt1(r, sizeof r, valve_range_mmbar()));
    return info;
}

static void start_auto(void)
{
    const char *why;
    if (!valve_start_auto(&why))
        ui_msg(why);
}

static void to_manual(void)
{
    const char *why;
    if (!valve_to_manual(&why))
        ui_msg(why);
}

// Typed target, then AUTO at it with no second question (simulator keypadStart).
static bool start_set(int32_t v10, const char **why)
{
    return valve_set_target(v10 * 100, why) && valve_start_auto(why);
}

static void keypad_start(void)
{
    ui_kp_t k = { .title = "Target for AUTO, mB", .info = range_info(), .dec = 1, .min = 0,
                  .max = valve_range_mmbar() / 100, .live_label = "Now, mB", .live_milli = pv_milli,
                  .live_dec = 1, .on_set = start_set };
    ui_keypad_ex(&k);
}

static int32_t sp_pending;   // 0.1 mbar, waiting for Change target

static void sp_change(void)
{
    const char *why;
    if (!valve_set_target(sp_pending * 100, &why))
        ui_msg(why);
}

static bool sp_set(int32_t v10, const char **why)
{
    int32_t now;
    valve_target(&now);
    now /= 100;
    sp_pending = v10;
    if (v10 - now > (int32_t)(CTRL_SP_CONFIRM * 10) || now - v10 > (int32_t)(CTRL_SP_CONFIRM * 10)) {
        static char q[64];
        char b[16];
        lv_snprintf(q, sizeof q, "Change target to %s mB?", ui_fmt1(b, sizeof b, (int64_t)v10 * 100));
        ui_confirm(q, "Change\ntarget", sp_change);
        return true;
    }
    return valve_set_target(v10 * 100, why);
}

static void keypad_sp(void)
{
    ui_kp_t k = { .title = "Target, mB", .info = range_info(), .dec = 1, .min = 0,
                  .max = valve_range_mmbar() / 100, .live_label = "Now, mB", .live_milli = pv_milli,
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

void ui_manual_ask(void)
{
    // No %: the valve keeps moving while this is up (Ryan, 22 Oct).
    ui_confirm("Switch to MANUAL? Valve holds where it is.", "Switch to\nMANUAL", to_manual);
}

static void w02_ok_event(lv_event_t *e)
{
    (void)e;
    valve_w02_clear();
}

static void target_event(lv_event_t *e)
{
    (void)e;
    if (valve_auto())
        keypad_sp();
    else
        ui_msg(valve_mode() == VALVE_MANUAL ? "Target is used in AUTO: Start AUTO asks for it"
                                            : not_manual_why());
}

static bool goto_set(int32_t v100, const char **why)
{
    if (!valve_goto_pct100(v100, why)) {
        *why = valve_motion_blocked() ? "Reset the fault first" : "Limit switch pressed, cannot close";
        return false;
    }
    return true;
}

static void goto_event(lv_event_t *e)
{
    (void)e;
    if (valve_mode() != VALVE_MANUAL || valve_motion_blocked()) {
        ui_msg(not_manual_why());
        return;
    }
    ui_keypad("Go to, %", "0 closed, 100 open", 0, 10000, "Valve, %", valve_pct_milli, goto_set);
}

static void calibrate(void)
{
    const char *why;
    if (!valve_calibrate(&why))
        ui_msg(valve_motion_blocked() ? "Reset the fault first" : why);
    else if (!ui_showing(&ui_scr_calib))
        ui_open(&ui_scr_calib);   // the steps show there; Back returns (Ryan, 16 Oct)
}

static void set_closed(void)
{
    const char *why;
    if (!valve_set_closed(&why))
        ui_msg(valve_motion_blocked() ? "Reset the fault first" : why);
    else
        ui_msg("Position set to closed (0 %). Not verified by homing.");
}

static bool set_position(int32_t v100, const char **why)
{
    if (!valve_set_position(v100, why)) {
        if (valve_motion_blocked()) *why = "Reset the fault first";
        return false;
    }
    char v[16], t[64];
    lv_snprintf(t, sizeof t, "Position set to %s %%. Not verified by homing.", ui_fmt2(v, sizeof v, (int64_t)v100 * 10));
    ui_msg(t);
    return true;
}

/* Home and Set position are two jobs, kept apart on every screen (Ryan,
 * 23 Oct): Home finds 0 % on the limit switch; Set position takes the
 * position from the operator (closed, a typed %, or the saved position),
 * not verified by homing. */
static void set_position_ask(void)
{
    ui_keypad("Set position, %", "0 closed, 100 open", 0, 10000, NULL, NULL, set_position);
}

static void set_closed_ask(void)
{
    ui_confirm("Set the valve as closed (0 %)? Use only if the valve is known to be seated. Not verified by homing.",
               "Set as closed", set_closed);
}

static void use_saved(void)
{
    const char *why;
    if (!valvepos_use(&why))
        ui_msg(why);
}

void ui_set_position_pick(int i)
{
    if (i == 0)
        set_closed_ask();
    else if (i == 1)
        set_position_ask();
    else
        use_saved();
}

// The ways to set it: closed, a typed %, the saved position when offered.
void ui_set_position_ask(void)
{
    static char saved[32];
    static const char *items[3] = { "Closed, 0 %", "Enter %", saved };
    int32_t q;
    int n = 2;
    if (valvepos_offer(&q)) {
        char v[16];
        int32_t pm = valve_pos_max();
        ui_fmt2(v, sizeof v, ((int64_t)q * 100000 + (q >= 0 ? pm / 2 : -pm / 2)) / pm);
        lv_snprintf(saved, sizeof saved, "Last saved, %s %%", v);
        n = 3;
    }
    ui_pick("Set position (not verified by homing)", items, n, -1, ui_set_position_pick);
}

static char home_q[128];

static const char *home_minutes(void)
{
    static char t[24];
    uint32_t s = valve_cal_budget_s();
    lv_snprintf(t, sizeof t, "up to %lu min", (unsigned long)((s + 59) / 60));
    return t;
}

// Home only (Valve homing's Home tab, the start-up question).
void ui_home_ask(void)
{
    lv_snprintf(home_q, sizeof home_q, "Home the valve? It closes onto the limit switch to find 0 %%, %s.",
                home_minutes());
    ui_confirm(home_q, "Home", calibrate);
}

// Home's mode button while not homed: the two jobs side by side.
void ui_position_ask(void)
{
    lv_snprintf(home_q, sizeof home_q, "Valve position unknown. Home the valve on the limit switch (%s), or set the position?",
                home_minutes());
    ui_ask3(home_q, "Cancel", NULL, "Set\nposition", ui_set_position_ask, "Home", calibrate);
}

static void mode_event(lv_event_t *e)
{
    (void)e;
    static char q[64];
    char a[16];
    int32_t sp;
    bool have = valve_target(&sp);
    valve_mode_t m = valve_mode();
    if (m == VALVE_CALIBRATING)
        ui_msg("Homing: STOP to cancel");
    else if (m == VALVE_AUTO)
        ui_manual_ask();
    else if (m == VALVE_MANUAL && valve_fault_count())
        ui_msg("Reset the fault first");
    else if (m == VALVE_MANUAL && !have)
        keypad_start();   // no default target (Ryan, 10 Oct): the first Start AUTO asks for one
    else if (m == VALVE_MANUAL) {
        lv_snprintf(q, sizeof q, "Start AUTO at %s mB?", ui_fmt1(a, sizeof a, sp));
        ui_ask3(q, "Cancel", NULL, "New\ntarget", keypad_start, "Start\nAUTO", start_auto);
    } else if (valve_motion_blocked())
        ui_msg("Reset the fault first");
    else
        ui_position_ask();
}

static void valve_event_cb(lv_event_t *e)
{
    // Greyed and not opened until the valve is homed or set (Ryan, 24 Oct).
    if (ui_btn_disabled(e)) {
        ui_msg("Home the valve or set its position first");
        return;
    }
    ui_open(&ui_scr_valve);
}

static void menu_event(lv_event_t *e)
{
    (void)e;
    ui_open(&ui_scr_menu);
}

static void reset_event(lv_event_t *e)
{
    (void)e;
    char left[40], t[64];
    valve_reset(left, sizeof left);
    if (left[0]) {
        lv_snprintf(t, sizeof t, "Not reset, cause still present: %s", left);
        ui_msg(t);
    }
}

/* Build and refresh ------------------------------------------------------ */
static void home_build(lv_obj_t *scr)
{
    make_tile(scr, &pt, 10, "Pressure", "mB", "Target", true, target_event);
    make_tile(scr, &vt, 186, "Valve", "%", "Go to", false, goto_event);
    mode_btn = ui_btn(scr, 361, 48, 109, 48, UI_PRIMARY, "Home valve", mode_event, NULL);
    valve_btn = ui_btn(scr, 361, 104, 109, 48, UI_NAV, "Valve", valve_event_cb, NULL);
    ui_btn(scr, 361, 160, 109, 48, UI_NAV, "Menu", menu_event, NULL);
}

// Valve reading and bar, at 10 Hz.
static void home_fast(void)
{
    valve_mode_t m = valve_mode();
    char buf[16];
    bool cal = valve_calibrated() && m != VALVE_CALIBRATING;
    if (m == VALVE_CALIBRATING) {
        if (lv_obj_get_style_text_font(vt.reading, 0) != &ui_font_16)
            lv_obj_set_style_text_font(vt.reading, &ui_font_16, 0);
        ui_set_text(vt.reading, "Homing");
        ui_set_color(vt.reading, UI_TEXT);
    } else {
        if (lv_obj_get_style_text_font(vt.reading, 0) != &ui_num_40)
            lv_obj_set_style_text_font(vt.reading, &ui_num_40, 0);
        ui_set_text(vt.reading, cal ? ui_fmt2(buf, sizeof buf, valve_pct_milli()) : "----");
        ui_set_color(vt.reading, cal && !valve_auto() ? UI_TEXT : UI_IDLE);   // grey in AUTO, as "AUTO" (Ryan, 22 Oct)
    }
    int32_t p = ui_pct_shown(valve_pct_milli());
    bar_set(&vt, cal ? p * BAR_W / 10000 : -1);
}

static void home_refresh(void)
{
    ui_btn_enable(valve_btn, valve_mode() != VALVE_NOT_CAL);
    const clicks_state_t *s = clicks_state();
    valve_mode_t m = valve_mode();
    char buf[32];

    // Pressure.
    bool ok = s->pm_found && s->reading_ok && !s->boost_tripped;
    ui_set_text(pt.reading, ok ? ui_fmt1(buf, sizeof buf, s->p_mmbar) : "----");
    ui_set_color(pt.reading, ok ? UI_TEXT : UI_FAULT);
    bar_set(&pt, ok ? pbar_x(s->p_mmbar) : -1);
    alarm_set(&pt, !ok || valve_fault_is("F-01") || valve_fault_is("F-02"));
    int32_t sp;
    bool auto_on = valve_auto();
    box_set(&pt, auto_on, valve_target(&sp) ? ui_fmt1(buf, sizeof buf, sp) : "Not set");

    home_fast();
    alarm_set(&vt, valve_fault_is("F-03") || valve_fault_is("F-04") || valve_fault_is("F-05") ||
                   valve_fault_is("F-06"));
    bool manual = m == VALVE_MANUAL && !valve_motion_blocked();
    box_set(&vt, manual, manual ? ui_fmt2(buf, sizeof buf, valve_target_pct_milli()) : auto_on ? "AUTO" : "");

    // Mode button.
    int step = valve_cal_step();
        if (m == VALVE_CALIBRATING) {
        lv_snprintf(buf, sizeof buf, "%d of 3", step < 1 ? 1 : step);
        ui_btn_text(mode_btn, "Homing", buf);
        ui_btn_enable(mode_btn, false);
    } else if (m == VALVE_MANUAL) {
        ui_btn_text(mode_btn, "Start AUTO", NULL);   // a fault is refused on press
        ui_btn_enable(mode_btn, true);
    } else if (m == VALVE_AUTO) {
        ui_btn_text(mode_btn, "Switch to\nMANUAL", NULL);
        ui_btn_enable(mode_btn, true);
    } else {
        ui_btn_text(mode_btn, "Home valve", NULL);   // a fault is refused on press
        ui_btn_enable(mode_btn, true);
    }

    ui_status_line();
}

// Status line and Reset fault, for Home and the operator screens; the
// frame puts faults and messages over it.
void ui_status_line(void)
{
    char buf[32];
    valve_mode_t m = valve_mode();
    int step = valve_cal_step();
    int nf = valve_fault_count();
    if (nf) {
        lv_snprintf(buf, sizeof buf, "%d faults", nf);
        ui_strip_action("Reset fault", nf > 1 ? buf : NULL, reset_event);
    }
    int32_t q;
    if (valve_question(&q)) {
        char t[64], v[16];
        lv_snprintf(t, sizeof t, "W-01 Limit switch pressed at %s %%",
                    ui_fmt1(v, sizeof v, (int64_t)q * 100));
        ui_strip_set(UI_STRIP_WARN, NULL, t);
    } else if (m == VALVE_NOT_CAL) {
        // A plain info line with the valve icon, as the others (Ryan, 22 Oct).
        ui_strip_set(UI_STRIP_INFO, UI_ICON_VALVE, "Valve not homed");
    } else if (m == VALVE_CALIBRATING) {
        static const char *const what[4] = { "Homing: moving off the switch", "Homing 1 of 3: fast close to the switch",
                                             "Homing 2 of 3: backing off the switch",
                                             "Homing 3 of 3: slow close onto the switch" };
        ui_strip_set(UI_STRIP_INFO, LV_SYMBOL_REFRESH, what[step < 0 ? 0 : step]);
    } else if (m == VALVE_MANUAL && valve_w02()) {
        // No PAUSED state (Ryan, 22 Oct): W-02 drops to MANUAL; shown until OK.
        if (!nf)
            ui_strip_action("OK", NULL, w02_ok_event);
        ui_strip_set(UI_STRIP_WARN, NULL, "W-02 Not settling: switched to MANUAL, valve idle");
    } else if (m == VALVE_AUTO) {
        // The regulation words, moving or not: AUTO moves the valve all the time.
        char t[48];
        ui_strip_set(UI_STRIP_INFO, UI_ICON_MODE, auto_text(t, sizeof t, true));
    } else if (stepper3_busy()) {
        char t[40], v[16];
        lv_snprintf(t, sizeof t, "Moving to %s %%", ui_fmt2(v, sizeof v, valve_target_pct_milli()));
        ui_strip_set(UI_STRIP_INFO, UI_ICON_MODE, t);
    } else {
        // Idle: where the valve is, nothing alarming (Ryan, 16 Oct); a
        // position set rather than homed says so (Ryan, 23 Oct).
        char t[48], v[16];
        valve_ref_t how = valve_reference();
        ui_fmt2(v, sizeof v, valve_pct_milli());
        if (how == VALVE_REF_HAND || how == VALVE_REF_SAVED)
            lv_snprintf(t, sizeof t, "%s %%, %s", v, how == VALVE_REF_HAND ? "entered" : "last saved");
        else
            lv_snprintf(t, sizeof t, "Valve at %s %%", v);
        ui_strip_set(UI_STRIP_INFO, UI_ICON_MODE, t);
    }
}

const ui_screen_t ui_scr_home = { "Home", home_build, home_refresh, home_fast };

/* --------------------------------------------------------------------------
 * Menu: the six v63 tiles in three captioned rows (Trend from stage 7; it
 * is built). The 0.6.0 motor screen is Settings > Motor override.
 * ------------------------------------------------------------------------ */
static const ui_screen_t *const menu_scr[6] = { &ui_scr_trend, &ui_scr_faults, &ui_scr_calib, &ui_scr_sensor_cal,
                                                &ui_scr_diag, &ui_scr_settings };

static void menu_tile_event(lv_event_t *e)
{
    ui_open(lv_event_get_user_data(e));
}

static void menu_build(lv_obj_t *scr)
{
    // Grouped in rows with a caption on the left, as on Diagnostics.
    static const char *const rows[3] = { "Records", "Calibrate", "System" };
    static const char *const names[6] = { "Trend", "Faults and log", "Valve homing",
                                          "Sensor", "Diagnostics", "Settings" };
    // Caption and two tiles in 1 : 1.5 : 1.5, the caption centred on its row.
    for (int r = 0; r < 3; r++) {
        lv_obj_t *l = ui_label(scr, 10, 0, 111, &ui_font_16, UI_MUTED, LV_TEXT_ALIGN_LEFT, rows[r]);
        lv_obj_update_layout(l);
        lv_obj_set_y(l, 48 + r * 56 + (48 - lv_obj_get_height(l)) / 2);
    }
    for (int i = 0; i < 6; i++) {
        lv_obj_t *b = ui_btn(scr, i % 2 ? 303 : 129, 48 + (i / 2) * 56, i % 2 ? 167 : 166, 48, UI_NAV, names[i],
                             menu_tile_event, (void *)menu_scr[i]);
        ui_btn_enable(b, menu_scr[i] != NULL);
    }
}

static void menu_refresh(void)
{
    ui_status_line();
}

const ui_screen_t ui_scr_menu = { "Menu", menu_build, menu_refresh, NULL };
