#include <string.h>

#include "lvgl.h"
#include "pmc_config.h"
#include "clicks.h"
#include "limit.h"
#include "stepper3.h"
#include "valve.h"
#include "valvepos.h"
#include "link.h"
#include "ui.h"
#include "fixed.h"

/* --------------------------------------------------------------------------
 * Operator screens (stage 5, simulator v63): Valve position, Events,
 * Diagnostics. The closed-switch question is in ui.c, on every screen.
 * The Valve page (hold Close or Open, 1 % and 5 % steps, Fully close and
 * Fully open) was removed in 0.14.21 (Ryan, 4 Nov): Go to on Home moves
 * the valve, Motor override (Motor settings > Override) moves it by steps.
 * ------------------------------------------------------------------------ */

static void reset_cb(lv_event_t *e)
{
    (void)e;
    char left[40], t[64];
    valve_reset(left, sizeof left);
    if (left[0]) {
        lv_snprintf(t, sizeof t, "Not reset, cause still present: %s", left);
        ui_msg(t);
    }
}

static void right(lv_obj_t *l)
{
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
}

/* --------------------------------------------------------------------------
 * Valve position: what is done to the valve now, from the Menu's valve
 * tile (0.16.2, Ryan 9 Nov: "the functionality between valve position and
 * valve setup is a bit confusing"). Home valve and the three ways to set
 * the position act on the valve; the numbers they use (travel, direction,
 * back-off, return, speeds) are drafts on Motor settings, with Motor
 * override (0.16.17), and nothing here changes a saved setting.
 * ------------------------------------------------------------------------ */
static struct {
    lv_obj_t *state, *state_cap, *turns, *start, *closed, *enter, *saved;
} cp;

/* One page (Ryan, 24 Oct: no tabs, less homing detail). Row 1 the valve's
 * state, turns and Help; then Home valve and the three ways to set the
 * position. */
static const char *const calib_help =
    "Acts on the valve now; its saved numbers are on Motor settings.\n\n"
    "Home valve: close to the switch, back off, close slowly, Return = 0 %. STOP cancels.\n\n"
    "Set as 0 %, Set as other %, Use last saved: set the position without moving.\n\n"
    "F-3 switch not found, F-4 switch stuck: check the switch, reset, home again.";

static void start_cb(lv_event_t *e)
{
    (void)e;
    if (valve_mode() == VALVE_CALIBRATING)
        ui_msg("Homing: STOP to cancel");
    else if (valve_motion_blocked())
        ui_msg("Reset the fault first");
    else if (valve_auto())
        ui_msg("Stop AUTO first");
    else
        ui_home_ask();
}

static void calib_help_cb(lv_event_t *e)
{
    (void)e;
    ui_text_view("Valve position", calib_help);
}

// Set position: the same guards as Home valve, then the list's actions.
static const char *setpos_why(void)
{
    if (valve_mode() == VALVE_CALIBRATING) return "Homing: STOP to cancel";
    if (valve_motion_blocked())            return "Reset the fault first";
    if (valve_auto())                      return "Stop AUTO first";
    if (stepper3_busy())                   return "Wait for the valve to stop";
    return NULL;
}

static void setpos_cb(lv_event_t *e)
{
    const char *why = setpos_why();
    if (why) {
        ui_msg(why);
        return;
    }
    ui_set_position_pick((int)(intptr_t)lv_event_get_user_data(e));
}

static void timer_deleted(lv_event_t *e)
{
    lv_timer_delete(lv_event_get_user_data(e));
}

// Motor override was here from 0.14.13 to 0.16.16; it is on Motor settings >
// Override from 0.16.17 (Ryan 12 Nov: "it doesnt feel right to be in valve position").

static void calib_build(lv_obj_t *scr)
{
    memset(&cp, 0, sizeof cp);
    cp.state = ui_info(scr, 10, 48, 226, 48, "Valve position");
    cp.state_cap = lv_obj_get_child(lv_obj_get_parent(cp.state), 0);
    cp.turns = ui_info(scr, 244, 48, 109, 48, "Turns");
    right(cp.turns);
    ui_btn(scr, 361, 48, 109, 48, UI_NAV, "Help", calib_help_cb, NULL);
    // Ryan, 24 Oct: set positions on the left, Home valve and Use last
    // saved on the right.
    cp.closed = ui_btn(scr, 10, 104, 226, 48, UI_SECONDARY, "Set as 0 %", setpos_cb, (void *)(intptr_t)0);
    cp.start = ui_btn(scr, 244, 104, 226, 48, UI_PRIMARY, "Home valve", start_cb, NULL);
    cp.enter = ui_btn(scr, 10, 160, 226, 48, UI_SECONDARY, "Set as other %", setpos_cb, (void *)(intptr_t)1);
    cp.saved = ui_btn(scr, 244, 160, 226, 48, UI_SECONDARY, "Use last saved", setpos_cb, (void *)(intptr_t)2);
}

// Motor turns from closed at 10 Hz (Ryan, 24 Oct: turns read more
// plainly than steps); counts through every homing stage.
static void calib_fast(void)
{
    char buf[16];
    int64_t full = (int64_t)stepper3_position();
    ui_set_text(cp.turns, fixed_fmt(buf, sizeof buf, fixed_round(full * 1000 / STEPPER3_STEPS_PER_TURN, 1), 2));
}

static void calib_refresh(void)
{
    char w[24], d[20], st[48];
    bool calib = valve_mode() == VALVE_CALIBRATING;
    int step = valve_cal_step();
    valve_ref_t how = valve_reference();
    // While homing the caption says Homing and the box the stage, the
    // strip's "Homing 2 of 4: backing off" in two parts (too long for 226).
    static const char *const doing[5] = { "Off the switch", "1 of 4: fast close",
                                          "2 of 4: backing off", "3 of 4: slow close",
                                          "4 of 4: return" };
    uint32_t age = 0, took = 0;
    bool ran = valve_last_cal(&age, &took);
    const char *iso = valve_last_cal_iso();
    const char *when = !ran ? NULL : iso[0] ? ui_date(d, sizeof d, iso) : ui_when(w, sizeof w, age);
    if (calib)
        lv_strlcpy(st, doing[step < 0 ? 0 : step > 4 ? 4 : step], sizeof st);
    else if (how == VALVE_REF_HOMED && when)
        lv_snprintf(st, sizeof st, "Homed %s", when);
    else if (how == VALVE_REF_HAND)
        lv_strlcpy(st, "Set by hand, not homed", sizeof st);
    else if (how == VALVE_REF_SAVED)
        lv_strlcpy(st, "Last saved, not homed", sizeof st);
    else if (how == VALVE_REF_PARKED)
        lv_strlcpy(st, "Kept at shut down", sizeof st);
    else
        lv_strlcpy(st, "Not homed", sizeof st);
    ui_set_text(cp.state, st);
    ui_set_text(cp.state_cap, calib ? "Homing" : "Valve position");
    calib_fast();
    ui_btn_text(cp.start, calib ? "Homing" : "Home valve", NULL);
    ui_btn_enable(cp.start, !calib && !valve_motion_blocked() && !valve_auto());
    ui_btn_primary(cp.start, !valve_calibrated());   // filled only while needed (H5)

    bool can = !setpos_why();
    int32_t q;
    bool offer = valvepos_offer(&q);
    char v[16], sv[32];
    if (offer) {
        int32_t pm = valve_pos_max();
        ui_fmt2(v, sizeof v, ((int64_t)q * 100000 + (q >= 0 ? pm / 2 : -pm / 2)) / pm);
        lv_snprintf(sv, sizeof sv, "%s %%", v);
    }
    ui_btn_text(cp.saved, "Use last saved", offer ? sv : NULL);
    ui_btn_enable(cp.closed, can);
    ui_btn_enable(cp.enter, can);
    ui_btn_enable(cp.saved, can && offer);
    ui_status_line();
}

const ui_screen_t ui_scr_calib = { "Valve position", calib_build, calib_refresh, calib_fast };

/* --------------------------------------------------------------------------
 * Events
 * ------------------------------------------------------------------------ */
#define LOG_ROWS 40
static struct {
    lv_obj_t *list, *none, *reset, *log, *log_time, *log_tag, *box, *page[2];
    int nf;                   // rows built
    char sig[64];             // tags shown, to rebuild on change
    uint32_t log_total;
    int detail;               // fault shown in the detail, -1 none
    lv_obj_t *d_title, *d_when, *d_text[4], *d_next;
} fp;
static char log_text[LOG_ROWS * 112];
// Time and tag columns, one line per wrapped text line (Ryan, 22 Oct: the
// event texts start at one x).
static char log_time[LOG_ROWS * 20], log_tag[LOG_ROWS * 12];

typedef struct {
    const char *tag, *happened, *controller, *check, *reset;
} fault_help_t;

static const fault_help_t fault_help[] = {
    { "F-1", "The Druck reading is missing or outside 0.005-11 mV/V.",
      "Pressure shows ----. The valve still moves.",
      "The Druck cable and plug, and the Power Monitor Click.",
      "The reading is back." },
    { "F-2", "The Druck excitation left 9.5-10.5 V, or the Boost 10 tripped.",
      "Pressure not trusted. The valve still moves.",
      "The Boost 10 and the 5 V supply. A trip needs a power cycle.",
      "The excitation is back in the window." },
    { "F-3", "Homing, the valve closed 1.1 x full travel without the limit switch.",
      "Stopped. The valve is not homed.",
      "The switch and its wire, and that the valve turns.",
      "Any time. Then home again." },
    { "F-4", "The limit switch stayed pressed after the valve moved off it, or pressed during an opening move.",
      "Stopped. The valve is not homed.",
      "The switch is not stuck (ice), its wire is not broken. On an opening move: Direction and coil order.",
      "Any time. Then home again." },
    { "F-6", "The valve moved in AUTO but the pressure did not respond, 5 windows of 30 s.",
      "Stopped the valve and holds it. MANUAL.",
      "Drive slipping, motor unplugged, needle seized.",
      "Any time. Check before starting AUTO." },
};

static const fault_help_t *help_for(const char *tag)
{
    for (size_t i = 0; i < sizeof fault_help / sizeof fault_help[0]; i++)
        if (strcmp(fault_help[i].tag, tag) == 0)
            return &fault_help[i];
    return &fault_help[0];
}

static const char *mode_word(valve_mode_t m)
{
    switch (m) {
    case VALVE_CALIBRATING: return "HOMING";
    case VALVE_MANUAL:      return "MANUAL";
    case VALVE_AUTO:        return "AUTO";
    case VALVE_SHUTDOWN:    return "SHUTDOWN";
    case VALVE_OFF:         return "OFF";
    default:                return "NOT HOMED";
    }
}

static void detail_fill(void)
{
    valve_fault_t f;
    int n = valve_fault_count();
    if (fp.detail >= n)
        fp.detail = n - 1;
    if (fp.detail < 0 || !valve_fault_get(fp.detail, &f)) {
        ui_overlay_close();
        fp.detail = -1;
        return;
    }
    const fault_help_t *h = help_for(f.tag);
    char buf[160], w[16];
    lv_snprintf(buf, sizeof buf, "%s  %s", f.tag, f.text);
    ui_set_text(fp.d_title, buf);
    lv_snprintf(buf, sizeof buf, "%s, %s", ui_when(w, sizeof w, f.age_ms), mode_word(f.mode));
    ui_set_text(fp.d_when, buf);
    ui_set_text(fp.d_text[0], h->happened);
    ui_set_text(fp.d_text[1], h->controller);
    ui_set_text(fp.d_text[2], h->check);
    lv_snprintf(buf, sizeof buf, "%s%s", h->reset, f.can_reset ? " It can be reset now." : " Not yet.");
    ui_set_text(fp.d_text[3], buf);
    lv_snprintf(buf, sizeof buf, "%d of %d", fp.detail + 1, n);
    ui_btn_text(fp.d_next, "Next fault", buf);
    ui_btn_enable(fp.d_next, n > 1);
}

static void d_next_cb(lv_event_t *e)
{
    (void)e;
    int n = valve_fault_count();
    if (n > 1) {
        fp.detail = (fp.detail + 1) % n;
        detail_fill();
    }
}

static void d_reset_cb(lv_event_t *e)
{
    reset_cb(e);
    if (valve_fault_count())
        detail_fill();
    else {
        ui_overlay_close();
        fp.detail = -1;
    }
}

static void d_close_cb(lv_event_t *e)
{
    (void)e;
    ui_overlay_close();
    fp.detail = -1;
}

// The detail closes in many ways (Close, Reset, Back, Home, or another
// overlay such as the switch question replacing it): its labels go with it.
static void detail_deleted(lv_event_t *e)
{
    (void)e;
    fp.detail = -1;
}

static void detail_open(int i)
{
    lv_obj_t *o = ui_overlay_full();   // its buttons sit in the strip's row
    fp.detail = i;
    lv_obj_add_event_cb(o, detail_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_t *c = ui_card(o, 10, 8, 460, 160);
    fp.d_title = ui_label(c, 12, 6, 258, &ui_font_16, UI_FAULT, LV_TEXT_ALIGN_LEFT, "");
    fp.d_when = ui_label(c, 270, 8, 178, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_RIGHT, "");
    static const char *const caps[4] = { "Happened", "Controller", "Check", "Reset when" };
    for (int k = 0; k < 4; k++) {
        ui_label(c, 12, 30 + k * 32, 84, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT, caps[k]);
        fp.d_text[k] = ui_label(c, 98, 30 + k * 32, 350, &ui_font_12, UI_TEXT, LV_TEXT_ALIGN_LEFT, "");
        lv_label_set_long_mode(fp.d_text[k], LV_LABEL_LONG_WRAP);
    }
    fp.d_next = ui_btn(o, 10, 176, 109, 48, UI_SECONDARY, "Next fault", d_next_cb, NULL);
    ui_btn(o, 127, 176, 109, 48, UI_PRIMARY, "Reset fault", d_reset_cb, NULL);
    ui_btn(o, 244, 176, 109, 48, UI_SECONDARY, "Close", d_close_cb, NULL);
    detail_fill();
}

static void row_cb(lv_event_t *e)
{
    detail_open((int)(intptr_t)lv_event_get_user_data(e));
}

static void faults_reset_cb(lv_event_t *e)
{
    if (ui_btn_disabled(e)) {
        ui_msg("No fault to reset");
        return;
    }
    reset_cb(e);
}

// Events Up / Down, as on Help: on press, repeating while held, greyed at
// the ends. The list itself does not drag.
static void log_buttons(void)
{
    lv_obj_update_layout(fp.box);
    ui_btn_enable(fp.page[0], lv_obj_get_scroll_top(fp.box) > 0);
    ui_btn_enable(fp.page[1], lv_obj_get_scroll_bottom(fp.box) > 0);
}

static void log_page(lv_event_t *e)
{
    if (ui_btn_disabled(e))
        return;
    int32_t dir = lv_event_get_user_data(e) ? 1 : -1;
    int32_t room = dir > 0 ? lv_obj_get_scroll_bottom(fp.box) : lv_obj_get_scroll_top(fp.box);
    int32_t d = room < 48 ? room : 48;   // three lines
    if (d > 0)
        lv_obj_scroll_by(fp.box, 0, -dir * d, LV_ANIM_OFF);
    log_buttons();
}

static void faults_build(lv_obj_t *scr)
{
    memset(&fp, 0, sizeof fp);
    fp.detail = -1;
    fp.nf = -1;
    fp.list = ui_card(scr, 10, 48, 343, 48);
    lv_obj_add_flag(fp.list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(fp.list, LV_DIR_VER);
    lv_obj_set_scroll_snap_y(fp.list, LV_SCROLL_SNAP_START);
    fp.reset = ui_btn(scr, 361, 48, 109, 48, UI_PRIMARY, "Reset fault", faults_reset_cb, NULL);

    lv_obj_t *ev = ui_card(scr, 10, 104, 343, 104);
    ui_label(ev, 12, 4, 300, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT, "Events, newest first");
    lv_obj_t *box = lv_obj_create(ev);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, 12, 22);
    lv_obj_set_size(box, 319, 80);
    lv_obj_set_scroll_dir(box, LV_DIR_VER);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_MOMENTUM |
                            LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_ON);
    lv_obj_set_style_width(box, 6, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(box, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(box, UI_IDLE, LV_PART_SCROLLBAR);
    fp.box = box;
    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = ui_btn(scr, 361, i ? 160 : 104, 109, 48, UI_SECONDARY, "", NULL, NULL);
        ui_btn_arrow(b, !i);
        lv_obj_add_event_cb(b, log_page, LV_EVENT_PRESSED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(b, log_page, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)i);
        fp.page[i] = b;
    }
    fp.log_time = ui_label(box, 0, 0, 0, &ui_font_12, UI_TEXT, LV_TEXT_ALIGN_LEFT, "");
    fp.log_tag = ui_label(box, 0, 0, 0, &ui_font_12, UI_TEXT, LV_TEXT_ALIGN_LEFT, "");
    fp.log = ui_label(box, 0, 0, 307, &ui_font_12, UI_TEXT, LV_TEXT_ALIGN_LEFT, "");
    lv_label_set_long_mode(fp.log, LV_LABEL_LONG_WRAP);
    for (int i = 0; i < 3; i++)
        lv_obj_set_style_text_line_space(i == 0 ? fp.log_time : i == 1 ? fp.log_tag : fp.log, 2, 0);
    fp.log_total = (uint32_t)-1;
}

static void faults_rows(void)
{
    char sig[64] = "";
    valve_fault_t f;
    int n = valve_fault_count();
    for (int i = 0; i < n && valve_fault_get(i, &f); i++) {
        size_t len = strlen(sig);
        lv_snprintf(sig + len, sizeof sig - len, "%s ", f.tag);
    }
    if (n == fp.nf && strcmp(sig, fp.sig) == 0)
        return;
    fp.nf = n;
    lv_strlcpy(fp.sig, sig, sizeof fp.sig);
    lv_obj_clean(fp.list);
    if (!n) {
        ui_label(fp.list, 12, 14, 300, &ui_font_16, UI_IDLE, LV_TEXT_ALIGN_LEFT, "No faults");
        return;
    }
    for (int i = 0; i < n && valve_fault_get(i, &f); i++) {
        lv_obj_t *r = lv_obj_create(fp.list);
        lv_obj_remove_style_all(r);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(r, 0, i * 48);
        lv_obj_set_size(r, 343, 48);
        lv_obj_add_flag(r, LV_OBJ_FLAG_SCROLL_ON_FOCUS | LV_OBJ_FLAG_SNAPPABLE);
        lv_obj_add_event_cb(r, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        char t[48], w[16];
        lv_snprintf(t, sizeof t, "%s  %s", f.tag, f.text);
        ui_label(r, 12, 5, 300, &ui_font_16, UI_FAULT, LV_TEXT_ALIGN_LEFT, t);
        lv_snprintf(t, sizeof t, "%s  %s", ui_when(w, sizeof w, f.age_ms), mode_word(f.mode));
        ui_label(r, 12, 27, 300, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT, t);
        ui_label(r, 318, 14, 0, &lv_font_montserrat_14, UI_IDLE, LV_TEXT_ALIGN_LEFT, LV_SYMBOL_RIGHT);
    }
}

static void faults_log(void)
{
    uint32_t total = valve_event_total();
    if (total == fp.log_total)
        return;
    fp.log_total = total;
    // Column widths from the widest time and tag shown.
    const lv_font_t *f = &ui_font_12;
    int32_t tw = 0, gw = 0;
    int n = 0;
    for (; n < LOG_ROWS; n++) {
        uint32_t age;
        const char *tag, *text;
        char w[16];
        if (!valve_event_get(n, &age, &tag, &text))
            break;
        lv_point_t a, b;
        lv_text_get_size(&a, ui_when(w, sizeof w, age), f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        lv_text_get_size(&b, tag, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (a.x > tw) tw = a.x;
        if (b.x > gw) gw = b.x;
    }
    const int32_t gap = 8, x_tag = tw + gap, x_text = x_tag + gw + gap, text_w = 307 - x_text;
    size_t lt = 0, lg = 0, lx = 0;
    log_text[0] = log_time[0] = log_tag[0] = '\0';
    for (int i = 0; i < n; i++) {
        uint32_t age;
        const char *tag, *text;
        char w[16];
        if (!valve_event_get(i, &age, &tag, &text))
            break;
        // Lines the text wraps to: the time and tag columns skip as many.
        lv_point_t sz;
        lv_text_get_size(&sz, text, f, 0, 2, text_w, LV_TEXT_FLAG_NONE);
        int lines = (sz.y + 2) / (lv_font_get_line_height(f) + 2);
        if (lines < 1) lines = 1;
        lv_snprintf(log_time + lt, sizeof log_time - lt, "%s%s", i ? "\n" : "", ui_when(w, sizeof w, age));
        lv_snprintf(log_tag + lg, sizeof log_tag - lg, "%s%s", i ? "\n" : "", tag);
        lv_snprintf(log_text + lx, sizeof log_text - lx, "%s%s", i ? "\n" : "", text);
        lt += strlen(log_time + lt);
        lg += strlen(log_tag + lg);
        lx += strlen(log_text + lx);
        for (int k = 1; k < lines; k++) {
            if (lt + 1 < sizeof log_time) { log_time[lt++] = '\n'; log_time[lt] = '\0'; }
            if (lg + 1 < sizeof log_tag)  { log_tag[lg++] = '\n'; log_tag[lg] = '\0'; }
        }
    }
    lv_obj_set_x(fp.log_tag, x_tag);
    lv_obj_set_x(fp.log, x_text);
    lv_obj_set_width(fp.log, text_w);
    lv_label_set_text_static(fp.log_time, log_time);
    lv_label_set_text_static(fp.log_tag, log_tag);
    lv_label_set_text_static(fp.log, log_text);
    log_buttons();
}

static void faults_refresh(void)
{
    faults_rows();
    faults_log();
    ui_btn_enable(fp.reset, valve_fault_count() > 0);
    if (fp.detail >= 0)   // the detail overlay is open (detail_deleted)
        detail_fill();
    char t[48];
    int n = valve_fault_count();
    lv_snprintf(t, sizeof t, n ? "%d latched. Tap one for detail." : "No faults latched", n);
    ui_strip_set(UI_STRIP_INFO, n ? LV_SYMBOL_WARNING : LV_SYMBOL_OK, t);
}

const ui_screen_t ui_scr_faults = { "Events", faults_build, faults_refresh, NULL };

/* --------------------------------------------------------------------------
 * Diagnostics
 * ------------------------------------------------------------------------ */
static struct {
    lv_obj_t *v[8];   // v[5] the board temperature from 0.13.4; v[7] the room sensor from 0.14.5
} dp;

// Valve motor row: position and limit switch, at 50 Hz so the switch
// shows as the hardware reads it (limit.c samples every 5 ms).
static void diag_motor(lv_timer_t *t)
{
    (void)t;
    char buf[16];
    lv_snprintf(buf, sizeof buf, "%ld", (long)stepper3_position());
    ui_set_text(dp.v[3], buf);
    ui_set_text(dp.v[4], limit_open_now() ? "Pressed" : "Released");
}

static const char diag_help[] =
    "Live raw readings.\n"
    "Raw: the pressure before calibration, near Room in air.\n"
    "----: no reading.";

static void diag_help_cb(lv_event_t *e)
{
    (void)e;
    ui_text_view("Diagnostics", diag_help);
}

static void diag_build(lv_obj_t *scr)
{
    // 0.16.27 (Ryan 13 Nov: "use fable again to review it so review the
    // display order, whether things are grouped shown intuitively, and make
    // it more coherent to the other screens"; Fable's review): no row
    // captions, cards from x 10 and Help at the top right as on the other
    // pages. A row each for the transducer, in signal order (excitation,
    // bridge output, pressure before calibration), the valve motor (steps,
    // limit switch, last move with how long it took, 226 wide) and the
    // surroundings (room pressure, board temperature). Row captions
    // Pressure, Valve motor and Controller to 0.16.26.
    static const struct { int8_t v; int16_t x, w; uint8_t row; const char *cap; } c[8] = {
        { 1, 10, 109, 0, "Excitation, V" }, { 0, 127, 109, 0, "Bridge, mV" }, { 2, 244, 109, 0, "Raw, mB" },
        { 3, 10, 109, 1, "Motor, steps" },  { 4, 127, 109, 1, "Limit switch" }, { 6, 244, 226, 1, "Last move" },
        { 7, 10, 109, 2, "Room, mB" },      { 5, 127, 109, 2, "Board, \xC2\xB0" "C" },
    };
    for (int i = 0; i < 8; i++)
        dp.v[c[i].v] = ui_info(scr, c[i].x, 48 + c[i].row * 56, c[i].w, 48, c[i].cap);   // rows at y 48, 104, 160
    ui_btn(scr, 361, 48, 109, 48, UI_NAV, "Help", diag_help_cb, NULL);
    lv_timer_t *t = lv_timer_create(diag_motor, 20, NULL);
    lv_obj_add_event_cb(scr, timer_deleted, LV_EVENT_DELETE, t);
    diag_motor(t);
}

static void diag_refresh(void)
{
    const clicks_state_t *s = clicks_state();
    char buf[48];
    bool sig = s->pm_found;
    int32_t uv = s->shunt_nv / 1000;    // Druck output, 0.001 mV
    int32_t au = uv < 0 ? -uv : uv;
    lv_snprintf(buf, sizeof buf, "%s%ld.%03ld", uv < 0 ? "-" : "", (long)(au / 1000), (long)(au % 1000));
    ui_set_text(dp.v[0], sig ? buf : "----");
    lv_snprintf(buf, sizeof buf, "%ld.%02ld", (long)(s->bus_mv / 1000), (long)(s->bus_mv % 1000 / 10));
    ui_set_text(dp.v[1], sig ? buf : "----");
    ui_set_text(dp.v[2], sig ? ui_fmt1(buf, sizeof buf, (int64_t)s->r_ppb * valve_range_mmbar() / 10000000) : "----");
    ui_set_text(dp.v[7], s->baro_ok ? ui_fmt1(buf, sizeof buf, s->baro_mmbar) : "----");   // Pressure 21

    // Last move: how it ended and how long it took, "At target, 41.3 s"
    // (Move took on a card of its own to 0.16.26).
    int32_t from;
    uint32_t ms;
    bool any = stepper3_last_move(&from, &ms);
    const char *res = NULL;
    if (any && stepper3_busy()) res = "Moving";
    else switch (stepper3_last_stop()) {
        case STEPPER3_STOP_DONE:    res = "At target"; break;
        case STEPPER3_STOP_LIMIT:   res = "At limit"; break;
        case STEPPER3_STOP_CMD:     res = "Stopped"; break;
        case STEPPER3_STOP_TIMEOUT: res = "Timed out"; break;
        case STEPPER3_STOP_REVERSED: res = "Switch on opening"; break;
        case STEPPER3_STOP_STALL:   res = "Loop stalled"; break;
        default: res = NULL; break;   // no move yet
    }
    char took[16], lm[48];
    if (!any || !res)
        lv_strlcpy(lm, "None yet", sizeof lm);
    else if (!stepper3_busy())
        lv_snprintf(lm, sizeof lm, "%s, %s s", res, ui_fmt1(took, sizeof took, ms));
    else
        lv_strlcpy(lm, res, sizeof lm);   // Moving
    ui_set_text(dp.v[6], lm);

    // Raw readings only (0.13.2, Ryan 28 Oct); the firmware, address, MAC
    // and Restart are on System. The strip is Home's
    // status line from 0.13.4 (Ryan, 28 Oct).
    ui_set_text(dp.v[5], ui_fmt1(buf, sizeof buf, s->die_mc));
    ui_status_line();
}

const ui_screen_t ui_scr_diag = { "Diagnostics", diag_build, diag_refresh, NULL };
