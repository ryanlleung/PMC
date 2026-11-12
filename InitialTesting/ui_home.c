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
#include "pmc_config.h"

/* --------------------------------------------------------------------------
 * Home (simulator v63 buildCtrl). Pressure and Valve tiles; on the right
 * the PID tile (KP, TI and KD in use; a tap opens PID tuning; 0.14.21, Ryan
 * 4 Nov: the operator does not tune, but wants to see the values), the IP
 * tile (the address and mask in use; a tap opens System setup; Trend until
 * 0.15.5, Ryan 4 Nov), Menu and STOP. No mode button from 0.15.3 (Ryan, 4 Nov: it did what the
 * tiles' buttons do): the header's panel shows the mode, and each tile's
 * bottom row changes the mode or sets its value:
 *   MANUAL     Pressure: Start AUTO (AUTO at the pressure shown, from where
 *              the valve is, no question). Valve: Go to.
 *   AUTO       Pressure: the SP box (the keypad; Set applies at once, no
 *              question since 0.14.22). Valve: Stop AUTO (MANUAL, the valve
 *              stays where it is, no question).
 *   NOT HOMED  Valve: Home valve (Home or Set position). Start AUTO greyed.
 *   HOMING     Valve: the Go to box, greyed, "----".
 * A yellow edge round the Valve tile while the limit switch is pressed
 * (0.16.4; a "Limit" tag beside the caption in 0.16.1-0.16.3); a valve
 * fault's red edge wins. While the switch pass band is on (0.16.5; Motor
 * settings > Override > Switch pass from 0.16.19), 5 to 50 % (10 to 20 % until 0.16.8, 5 to 25 % until
 * 0.16.11) is yellow on the valve bar.
 * The strip shows the status line, with the 5 min pressure sparkline on its
 * right as on the operator screens (ui.c, Ryan 21 Oct). A tap on the
 * sparkline opens Trend.
 * ------------------------------------------------------------------------ */
#define TILE_W  167
#define TILE_H  160
// The bar as wide as the widest reading, so it sits right under it: the
// wider of "1013.2" and "100.00" at 40 px (Ryan, 4 Nov).
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
    int32_t fill;             // bar fill to here, px; -1 no value (no marker)
    bool log;                 // pressure: decade lines; valve: 0/50/100
    bool band;                // valve: the switch pass band (5 to 50 %) marked yellow on the track
} tile_t;

static tile_t pt, vt;
static lv_obj_t *pid_btn, *pid_nm[3], *pid_val[3], *start_btn, *stop_btn, *homev_btn;
static bool vt_fault;   // a valve fault (F-3 to F-6): the Valve tile's red edge, over the limit's yellow

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
    // The switch pass band (0.16.5), over the track and fill: where the
    // switch does not stop closing.
    if (t->band) {
        lv_area_t b = { a.x1 + (BAR_W - 1) * VALVE_BAND_LO_PCT / 100, y1,
                        a.x1 + (BAR_W - 1) * VALVE_BAND_HI_PCT / 100, y2 };
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

// The tile's 3 px edge: red for a fault (the card style's colour), yellow
// for a warning (the limit switch pressed, 0.16.4), none.
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
    t->tile = ui_card(scr, x, 48, w, TILE_H);
    // Caption and unit 16 px (Ryan, 21 Oct: 12 px was too small).
    ui_label(t->tile, 12, 6, 90, &ui_font_16, UI_MUTED, LV_TEXT_ALIGN_LEFT, cap);
    ui_label(t->tile, r - 80, 6, 80, &ui_font_16, UI_MUTED, LV_TEXT_ALIGN_RIGHT, unit);
    // The reading, 40 px (Ryan 16 Oct: bigger once the sparklines left the
    // tiles); "1013.2" just fits, right edge on the bar's.
    t->reading = ui_label(t->tile, 0, 34, r, &ui_num_40, UI_TEXT, LV_TEXT_ALIGN_RIGHT, "----");
    t->bar = lv_obj_create(t->tile);
    lv_obj_remove_style_all(t->bar);
    lv_obj_remove_flag(t->bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(t->bar, r - BAR_W, 76);   // 14 px from the digits, as the caption is above them
    lv_obj_set_size(t->bar, BAR_W, 30);
    lv_obj_add_event_cb(t->bar, bar_draw, LV_EVENT_DRAW_MAIN, t);
    // SP / Go to at the button text size, 16 px (Ryan, 4 Nov).
    t->sp_cap = ui_label(t->tile, 12, 126, 54, &ui_font_16, UI_MUTED, LV_TEXT_ALIGN_LEFT, sp);
    t->box = lv_obj_create(t->tile);
    lv_obj_remove_style_all(t->box);
    lv_obj_remove_flag(t->box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(t->box, r - 88, 116);   // wide enough for "100.00" and the chevron
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
// setpoint to X mB?" question (Ryan, 4 Nov, 0.14.22).
static bool sp_set(int32_t v10, const char **why)
{
    return valve_set_target(v10 * 100, why);
}

static void keypad_sp(void)
{
    ui_kp_t k = { .title = "Setpoint, mB", .dec = 1, .min = 0,   // the range in the field's corners
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
        ui_msg("Position set to 0 %. Not verified by homing.");
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
 * 23 Oct): Home finds the limit switch (0 % is the back-off above it); Set position takes the
 * position from the operator (closed, a typed %, or the saved position),
 * not verified by homing. */
static void set_position_ask(void)
{
    ui_keypad("Set position, %", NULL, 0, 10000, NULL, NULL, set_position);
}

static void set_closed_ask(void)
{
    ui_confirm("Set the present position as 0 %? Not verified by homing.",
               "Set as 0 %", set_closed);
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
    static const char *items[3] = { "Set as 0 %", "Set as other %", saved };
    int32_t q;
    int n = 2;
    if (valvepos_offer(&q)) {
        char v[16];
        int32_t pm = valve_pos_max();
        ui_fmt2(v, sizeof v, ((int64_t)q * 100000 + (q >= 0 ? pm / 2 : -pm / 2)) / pm);
        lv_snprintf(saved, sizeof saved, "Use last saved, %s %%", v);
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
    lv_snprintf(home_q, sizeof home_q, "Valve position unknown. Home the valve on the limit switch (%s), or set the position?",
                home_minutes());
    ui_ask3(home_q, "Cancel", NULL, "Set\nposition", ui_set_position_ask, "Home\nvalve", calibrate);
}

// Stop AUTO (the Valve tile, AUTO): MANUAL, the valve stays where it is,
// no question (Switch to MANUAL had none from 28 Oct).
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

// KP, TI and KD of the set in use. With the PID table on, no set number
// (0.15.20, Ryan 6 Nov: "just KP ti kd"; 0.15.15 to 0.15.19 Set, KP, TI).
static void pid_refresh(void)
{
    const int set = valve_pid_set();
    char b[16];
    for (int i = 0; i < 3; i++)
        ui_set_text(pid_val[i], fmt3(b, sizeof b, cfg_tune(cfg_pid_key(set, i))));
}

// Start AUTO --------------------------------------------------------------
static bool reading_ok(void)
{
    const clicks_state_t *s = clicks_state();
    return s->pm_found && s->reading_ok && !s->boost_tripped;
}

// AUTO at the pressure as shown (0.1 mB), from where the valve is, with no
// question (Ryan, 4 Nov: Maintain in 0.14.21, Start AUTO from 0.15.3).
static void start_event(lv_event_t *e)
{
    const char *why;
    if (ui_btn_disabled(e) || valve_mode() != VALVE_MANUAL) {
        ui_msg(valve_auto() ? "Already in AUTO" : !reading_ok() ? "No pressure reading"
               : valve_fault_count() ? "Reset the fault first" : not_manual_why());
        ui_refresh_soon();
        return;
    }
    if (!valve_start_auto_at(p_shown(clicks_state()->p_mmbar), &why))
        ui_msg(why);
    ui_refresh_soon();   // the tiles and header at once, not at the next 0.5 s refresh (Ryan, 4 Nov)
}

static void menu_event(lv_event_t *e)
{
    (void)e;
    ui_open(&ui_scr_menu);
}

// IP tile (0.15.6, Ryan 4 Nov: in place of Trend; kept over the template's
// Trend in 0.15.9, Ryan 5 Nov "No trend show ip"): a grey "IP address"
// over the address and prefix the link uses, 192.168.1.100/24 (Ryan 5 Nov,
// option A), from start-up (a new one waits for a restart), 12 px. A tap
// opens System. Trend stays on Menu and on the sparkline.
static lv_obj_t *ip_val;

static void ip_event(lv_event_t *e)
{
    (void)e;
    ui_open(&ui_scr_namenet);
}

static void ip_make(lv_obj_t *scr, int32_t y)
{
    lv_obj_t *b = ui_btn(scr, 361, y, 109, 48, UI_SECONDARY, "", ip_event, NULL);
    // Centred (Ryan 5 Nov: left-aligned in 0.15.10, back to centred in 0.15.12).
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
#if PMC_REMOTE_SWITCH
    char ip[16], mask[16], t[24];
    cfg_net_boot(ip, mask);
    lv_snprintf(t, sizeof t, "%s/%d", ip, ip_prefix(mask));
    ui_set_text(ip_val, t);
#else
    ui_set_text(ip_val, "USB build");
#endif
}

void ui_reset_fault_event(lv_event_t *e)
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
    make_tile(scr, &pt, 10, "Pressure", "mB", "SP", true, target_event);
    make_tile(scr, &vt, 186, "Valve", "%", "Go to", false, goto_event);
    // The PID tile on top, the IP tile, then Menu above STOP (Ryan, 4 Nov,
    // 0.15.3; the IP tile in place of Trend from 0.15.6).
    pid_make(scr, 48);
    ip_make(scr, 104);
    ui_btn(scr, 361, 160, 109, 48, UI_NAV, "Menu", menu_event, NULL);
    // The mode buttons on the tiles' bottom rows, in place of the SP and Go
    // to boxes (0.15.3): Start AUTO outside AUTO, filled dark blue with
    // white lettering in the template (Ryan, 4 Nov; outlined to 0.15.7),
    // Stop AUTO in AUTO, outlined blue; Home valve while not homed, filled.
    // Under the bar and as wide (Ryan, 4 Nov: sized to the widest reading).
    start_btn = ui_btn(pt.tile, 156 - BAR_W, 116, BAR_W, 40, UI_PRIMARY, "Start AUTO", start_event, NULL);
    stop_btn = ui_btn(vt.tile, 155 - BAR_W, 116, BAR_W, 40, UI_SECONDARY, "Stop AUTO", stop_event, NULL);
    ui_btn_outline(stop_btn, true);
    homev_btn = ui_btn(vt.tile, 155 - BAR_W, 116, BAR_W, 40, UI_PRIMARY, "Home valve", homev_event, NULL);
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
        ui_set_color(vt.reading, cal && !valve_auto() ? UI_TEXT : UI_IDLE);   // grey in AUTO (Ryan, 22 Oct)
    }
    int32_t p = ui_pct_shown(valve_pct_milli());
    bar_set(&vt, cal ? p * BAR_W / 10000 : -1);
    // Limit switch pressed (the NC contact open): a yellow edge round the
    // Valve tile at 10 Hz, whatever the mode; a valve fault's red wins
    // (0.16.4, Ryan PR #43: "add yellow border instead of having the word
    // limit"; a yellow "Limit" tag beside the caption in 0.16.1-0.16.3).
    bool lim = stepper3_limit_pressed();   // passed in the pass band or not
    edge_set(&vt, vt_fault || lim, vt_fault ? UI_FAULT : UI_YELLOW);
    // The pass band on the valve bar while it is on (Motor settings > Override > Switch pass).
    bool band = valve_limit_band();
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
    ui_set_text(pt.reading, ok ? ui_fmt1(buf, sizeof buf, s->p_mmbar) : "----");
    ui_set_color(pt.reading, ok ? UI_TEXT : UI_FAULT);
    bar_set(&pt, ok ? pbar_x(s->p_mmbar) : -1);
    alarm_set(&pt, !ok || valve_fault_is("F-1") || valve_fault_is("F-2"));
    int32_t sp;
    bool auto_on = valve_auto();
    box_set(&pt, auto_on, valve_target(&sp) ? ui_fmt1(buf, sizeof buf, sp) : "Not set");
    // AUTO: the SP field. Otherwise Start AUTO, greyed while it would be
    // refused (a press says why).
    show_if(pt.box, auto_on);
    show_if(pt.sp_cap, auto_on);
    show_if(start_btn, !auto_on);
    ui_btn_enable(start_btn, m == VALVE_MANUAL && !valve_fault_count() && ok);

    vt_fault = valve_fault_is("F-3") || valve_fault_is("F-4") || valve_fault_is("F-5") || valve_fault_is("F-6");
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
    show_if(vt.box, !stop_on && !homev_on);
    show_if(vt.sp_cap, !stop_on && !homev_on);

    ui_status_line();
}

// Status line for Home and the operator screens; the frame puts faults
// (with Reset fault, on every screen from 0.13.19) and messages over it.
void ui_status_line(void)
{
    valve_mode_t m = valve_mode();
    int step = valve_cal_step();
    int nf = valve_fault_count();
    int32_t q;
    if (valve_question(&q)) {
        char t[64], v[16];
        lv_snprintf(t, sizeof t, "W-1 Limit switch pressed at %s %%",
                    ui_fmt1(v, sizeof v, (int64_t)q * 100));
        ui_strip_set(UI_STRIP_WARN, NULL, t);
    } else if (m == VALVE_NOT_CAL) {
        // A plain info line with the valve icon, as the others (Ryan, 22 Oct).
        ui_strip_set(UI_STRIP_INFO, UI_ICON_VALVE, "Valve not homed");
    } else if (m == VALVE_CALIBRATING) {
        static const char *const what[5] = { "Homing: moving off the switch", "Homing 1 of 4: fast close",
                                             "Homing 2 of 4: backing off",
                                             "Homing 3 of 4: slow close",
                                             "Homing 4 of 4: return" };
        ui_strip_set(UI_STRIP_INFO, LV_SYMBOL_REFRESH, what[step < 0 ? 0 : step]);
    } else if (m == VALVE_MANUAL && valve_warning()) {
        // No PAUSED state (Ryan, 22 Oct): W-2, W-4 and W-5 drop to
        // MANUAL; shown until OK.
        if (!nf)
            ui_strip_action("OK", NULL, warn_ok_event);
        ui_strip_set(UI_STRIP_WARN, NULL, valve_warning());
    } else if (m == VALVE_AUTO) {
        // The regulation words, moving or not: AUTO moves the valve all the time.
        char t[48];
        ui_strip_set(UI_STRIP_INFO, UI_ICON_MODE, auto_text(t, sizeof t, true));
    } else if (stepper3_busy()) {
        char t[40], v[16];
        lv_snprintf(t, sizeof t, "Moving to %s %%", ui_fmt2(v, sizeof v, valve_target_pct_milli()));
        ui_strip_set(UI_STRIP_INFO, UI_ICON_MODE, t);
    } else {
        // Idle: the valve icon, Valve idle and the pressure sparkline
        // (0.13.16, Ryan 28 Oct; "Idle. Pressure, last 5 min" without the
        // icon from 0.13.8). How the position was set (entered, last saved)
        // is on Home valve.
        ui_strip_set(UI_STRIP_INFO, UI_ICON_MODE, "Valve idle");
    }
}

const ui_screen_t ui_scr_home = { "Home", home_build, home_refresh, home_fast };

/* --------------------------------------------------------------------------
 * Menu: nine tiles in three rows of three, an icon over each name, no row
 * captions (0.16.2, Ryan 9 Nov: "remove the settings page and put
 * everything in the menu. remove the labels on the menu. show some icons
 * for each button"). Every page is one tap away:
 *   Trend           Events          PID tuning
 *   Motor settings  Valve position  Pressure setup
 *   System          Diagnostics     Review and save
 * Valve position and PID tuning swapped places in 0.16.3 (Ryan 9 Nov, PR #43:
 * "swap location of valve position and pid tuning"): the two valve tiles
 * side by side in the middle row. Calibration was renamed Pressure setup in
 * 0.16.7 (Ryan 10 Nov, PR #43: "something with pressure in the name and
 * doesnt overflow the line"): "Pressure settings" is 141 px and wraps.
 * Review and save is filled dark blue, as Ryan asked for it and Settings on
 * 6 Nov (0.15.23); with Settings gone it is the one dark tile, bottom right
 * where Settings was. Diagnostics stays on System too (0.15.19) and fills
 * the ninth slot. To 0.16.1: six tiles in captioned rows Records (Trend,
 * Events), Service (Valve position, Calibration), Setup (System, Settings),
 * with Choose sensor, Valve setup, PID tuning and Review and save a tap
 * further on Settings.
 * Three columns of 148 (x 10, 166, 322): "Review and save" at 16 px is 136
 * wide, too wide for the template's 109 columns, and Ryan does not want
 * two-line tile names (6 Nov).
 * ------------------------------------------------------------------------ */
typedef struct {
    const char *name, *icon;
    const ui_screen_t *scr;
} menu_item_t;

static const menu_item_t menu_items[9] = {
    { "Trend", UI_ICON_TREND, &ui_scr_trend },
    { "Events", LV_SYMBOL_LIST, &ui_scr_faults },
    { "PID tuning", LV_SYMBOL_LOOP, &ui_scr_tune },            // the loop, as the status line shows AUTO
    { "Motor settings", LV_SYMBOL_EDIT, &ui_scr_valve_set },   // travel, homing, drive: saved numbers (the pencil, as a draft's)
    { "Valve position", UI_ICON_VALVE, &ui_scr_calib },        // home, set the position, the motor: the valve now
    { "Pressure setup", UI_ICON_TARGET, &ui_scr_sensor_cal },   // Calibrate sensor
    { "System", LV_SYMBOL_SETTINGS, &ui_scr_namenet },
    { "Diagnostics", LV_SYMBOL_EYE_OPEN, &ui_scr_diag },
    { "Review and save", LV_SYMBOL_SAVE, &ui_scr_review },     // the header's unsaved icon
};

static void menu_tile_event(lv_event_t *e)
{
    ui_open(lv_event_get_user_data(e));
}

static void menu_build(lv_obj_t *scr)
{
    for (int i = 0; i < 9; i++) {
        const menu_item_t *m = &menu_items[i];
        // Plain (centred) tiles, no chevron: the icon marks what opens.
        lv_obj_t *b = ui_btn(scr, 10 + (i % 3) * 156, 48 + (i / 3) * 56, 148, 48, UI_SECONDARY, m->name,
                             menu_tile_event, (void *)m->scr);
        ui_btn_icon(b, m->icon);
        if (m->scr == &ui_scr_review)
            ui_btn_blue(b);
    }
}

static void menu_refresh(void)
{
    ui_status_line();
}

const ui_screen_t ui_scr_menu = { "Menu", menu_build, menu_refresh, NULL };
