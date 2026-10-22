#include <string.h>

#include "lvgl.h"
#include "pmc_config.h"
#include "clicks.h"
#include "limit.h"
#include "stepper3.h"
#include "valve.h"
#include "ui.h"

/* --------------------------------------------------------------------------
 * Operator screens for MANUAL (stage 5, simulator v63): Valve, Calibrate
 * valve, Faults and log, Diagnostics. The closed-switch question is in
 * ui.c, on every screen.
 * ------------------------------------------------------------------------ */

// Close, Open, -5 % and +5 % work before a calibration too (by steps);
// Go to, Fully close and Fully open need the zero.
static const char *why_no_steps(void)
{
    if (valve_motion_blocked()) return "Reset the fault first";
    if (valve_mode() == VALVE_CALIBRATING) return "Homing: STOP to cancel";
    if (valve_auto()) return "Switch to MANUAL first";
    return NULL;
}

static const char *why_not_manual(void)
{
    const char *why = why_no_steps();
    if (why) return why;
    if (valve_mode() != VALVE_MANUAL) return "Home the valve first";
    return NULL;
}

// Before a calibration: a move by steps (d in 0.1 % of the travel).
static bool vp_steps(int32_t d)
{
    const char *why = why_no_steps();
    if (!why && d < 0 && limit_open_now())
        why = "Limit switch pressed, cannot close";
    if (!why && !valve_move_steps((int32_t)((int64_t)d * valve_pos_max() / 1000), &why))
        why = "Limit switch pressed, cannot close";
    if (why)
        ui_msg(why);
    return why == NULL;
}

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

static const char *pressure_text(char *buf, size_t n, bool *ok)
{
    const clicks_state_t *s = clicks_state();
    *ok = s->pm_found && s->reading_ok && !s->boost_tripped;
    return *ok ? ui_fmt1(buf, n, s->p_mmbar) : "----";
}

static const char *pct_text(char *buf, size_t n, int32_t milli)
{
    return ui_fmt2(buf, n, milli);
}

static void right(lv_obj_t *l)
{
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
}

/* --------------------------------------------------------------------------
 * Valve page
 * ------------------------------------------------------------------------ */
static struct {
    lv_obj_t *p, *v, *go, *go_card, *go_chev, *fill, *pfill, *btn[8];
    bool jog;                 // a Close or Open hold is moving the valve
    int go_live;              // Go to card styled as live (1) or flat (0), -1 not yet
} vp;

static const char *const valve_help =
    "Close, Open: hold to move, let go to stop.\n"
    "-5 %, +5 %: step from where the valve is going.\n"
    "Go to: 0 closed to 100 open.\n"
    "Fully close, Fully open: to 0 % or 100 %.\n\n"
    "Moves only with no fault. Before homing Close, Open, -5 % and +5 % still move, by steps "
    "(5 % of the travel setting); Go to, Fully close and Fully open need the valve homed.\n"
    "The limit switch stops any closing move.\n"
    "STOP holds the valve where it is.";

static bool vp_goto(int32_t pct10)
{
    vp.jog = false;   // a new move has its own target (jog_press sets it again)
    const char *why = why_not_manual();
    if (!why && pct10 < valve_pct10() && limit_open_now())
        why = "Limit switch pressed, cannot close";
    if (!why && !valve_goto_pct10(pct10 < 0 ? 0 : pct10 > 1000 ? 1000 : pct10, &why))
        why = "Limit switch pressed, cannot close";
    if (why)
        ui_msg(why);
    return why == NULL;
}

static void jog_press(lv_event_t *e)
{
    bool open = lv_event_get_user_data(e) != NULL;
    if (valve_mode() == VALVE_NOT_CAL)
        vp.jog = vp_steps(open ? 1000 : -1000);   // a full travel; let go to stop
    else
        vp.jog = vp_goto(open ? 1000 : 0);
}

// The hold stays marked until the ramp-down ends (valve_refresh clears it):
// the move's target is still 0 or 100 %, and Go to would show it meanwhile.
static void jog_release(lv_event_t *e)
{
    (void)e;
    if (vp.jog)
        stepper3_stop_ramped();
}

static void step_cb(lv_event_t *e)
{
    int32_t d = (int32_t)(intptr_t)lv_event_get_user_data(e);
    bool jog = vp.jog;
    vp.jog = false;   // a new move has its own target
    if (valve_mode() == VALVE_NOT_CAL)
        vp_steps(d);
    else
        // From where a move will end; a hold's end (0 or 100 %) is not one.
        vp_goto((stepper3_busy() && !jog ? valve_target_pct10() : valve_pct10()) + d);
}

static bool goto_set(int32_t v100, const char **why)
{
    vp.jog = false;   // a new move has its own target
    if (!valve_goto_pct100(v100, why)) {
        *why = why_not_manual();
        if (!*why)
            *why = "Limit switch pressed, cannot close";
        return false;
    }
    return true;
}

static void goto_cb(lv_event_t *e)
{
    (void)e;
    if (valve_auto() && !valve_motion_blocked()) {
        ui_manual_ask();   // in AUTO a tap on Go to asks Switch to MANUAL
        return;
    }
    const char *why = why_not_manual();
    if (why) {
        ui_msg(why);
        return;
    }
    ui_keypad("Go to, %", "0 closed, 100 open", 0, 10000, "Valve, %", valve_pct_milli, goto_set);
}

static void full_cb(lv_event_t *e)
{
    bool open = lv_event_get_user_data(e) != NULL;
    const char *why = why_not_manual();
    if (!why && !open && limit_open_now())
        why = "Limit switch pressed, cannot close";
    if (why)
        ui_msg(why);
    else
        vp_goto(open ? 1000 : 0);
}

static void help_cb(lv_event_t *e)
{
    ui_text_view(lv_event_get_user_data(e) == valve_help ? "Valve" : "Valve homing",
                 lv_event_get_user_data(e));
}

/* Readings 64 px tall with 26 px numbers; move buttons 40 px in two rows
 * (Ryan chose this over one row of six, 15 Oct). */

// 3 px bar near the bottom of a 148 px card; returns the fill.
static lv_obj_t *thin_bar(lv_obj_t *card, int32_t y)
{
    lv_obj_t *track = ui_card(card, 10, y, 128, 3);
    lv_obj_set_style_radius(track, 0, 0);
    lv_obj_set_style_bg_color(track, UI_TRACK, 0);
    lv_obj_t *fill = ui_card(track, 0, 0, 0, 3);
    lv_obj_set_style_radius(fill, 0, 0);
    lv_obj_set_style_bg_color(fill, UI_FILL, 0);
    return fill;
}

static void bar_width(lv_obj_t *fill, int32_t w)
{
    if (lv_obj_get_width(fill) != w)
        lv_obj_set_width(fill, w);
}

// Reading card: caption left and unit right as on the Home tiles (Ryan,
// 21 Oct), 26 px number right-aligned, optional bar.
static lv_obj_t *reading(lv_obj_t *scr, int32_t x, int32_t h, const char *cap, const char *unit, lv_obj_t **fill)
{
    lv_obj_t *v = ui_info(scr, x, 48, 148, h, cap);
    lv_obj_t *card = lv_obj_get_parent(v);
    ui_label(card, 10, 5, 128, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_RIGHT, unit);
    right(v);
    lv_obj_set_style_text_font(v, &ui_num_26, 0);
    lv_obj_set_y(v, 20);
    if (fill)
        *fill = thin_bar(card, h - 10);
    return v;
}

static void hold_btn(lv_obj_t *b, bool open)
{
    lv_obj_add_event_cb(b, jog_press, LV_EVENT_PRESSED, (void *)(intptr_t)open);
    lv_obj_add_event_cb(b, jog_release, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(b, jog_release, LV_EVENT_PRESS_LOST, NULL);
}

static void valve_build(lv_obj_t *scr)
{
    memset(&vp, 0, sizeof vp);
    vp.go_live = -1;
    vp.p = reading(scr, 10, 64, "Pressure", "mB", &vp.pfill);
    vp.v = reading(scr, 166, 64, "Valve", "%", &vp.fill);
    // Go to is the card itself: tinted with a blue underline and a chevron
    // when a tap opens the keypad, as Home's boxes (Ryan, 21 Oct).
    vp.go = reading(scr, 322, 64, "Go to", "%", NULL);
    vp.go_card = lv_obj_get_parent(vp.go);
    lv_obj_add_flag(vp.go_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(vp.go_card, goto_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_border_side(vp.go_card, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(vp.go_card, UI_BLUE, 0);
    lv_obj_set_width(vp.go, 110);
    vp.go_chev = ui_label(vp.go_card, 126, 28, 0, &lv_font_montserrat_14, UI_BLUE, LV_TEXT_ALIGN_LEFT, LV_SYMBOL_RIGHT);

    // Row 1: hold to close or open, 1 % steps; row 2: fully close or open
    // under them, 5 % steps.
    static const char *const row1[4] = { "Close", "-1 %", "+1 %", "Open" };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = ui_btn(scr, 10 + i * 118, 120, 106, 40, UI_SECONDARY, row1[i],
                             i == 1 || i == 2 ? step_cb : NULL, (void *)(intptr_t)(i == 1 ? -10 : 10));
        if (i == 0 || i == 3) {
            hold_btn(b, i == 3);
            ui_btn_text(b, row1[i], "hold");
        }
        vp.btn[i] = b;
    }
    vp.btn[4] = ui_btn(scr, 128, 168, 106, 40, UI_SECONDARY, "-5 %", step_cb, (void *)(intptr_t)-50);
    vp.btn[5] = ui_btn(scr, 246, 168, 106, 40, UI_SECONDARY, "+5 %", step_cb, (void *)(intptr_t)50);
    vp.btn[6] = ui_btn(scr, 10, 168, 106, 40, UI_SECONDARY, "Fully close", full_cb, NULL);
    vp.btn[7] = ui_btn(scr, 364, 168, 106, 40, UI_SECONDARY, "Fully open", full_cb, (void *)1);
}

// Valve %, its bar and Go to, at 10 Hz.
static void valve_fast(void)
{
    char buf[16];
    bool cal = valve_calibrated() && valve_mode() != VALVE_CALIBRATING;
    ui_set_text(vp.v, cal ? pct_text(buf, sizeof buf, valve_pct_milli()) : "----");
    ui_set_color(vp.v, cal && !valve_auto() ? UI_TEXT : UI_IDLE);   // grey in AUTO, as "auto"
    int32_t p = cal ? ui_pct_shown(valve_pct_milli()) : 0;
    bar_width(vp.fill, p * 128 / 10000);
    // A dialog over a hold (the switch question, a fault) takes the touch
    // away from the button without a release: stop the hold.
    if (vp.jog && ui_overlay_open())
        stepper3_stop_ramped();
    // Where the valve is going (or last went); "--" during a hold, which
    // has no target, and before homing; "auto" in AUTO.
    bool have = cal && !(stepper3_busy() && vp.jog);
    ui_set_text(vp.go, valve_auto() ? "AUTO" : have ? pct_text(buf, sizeof buf, valve_target_pct_milli()) : "--");
    ui_set_color(vp.go, have && !valve_auto() && why_not_manual() == NULL ? UI_TEXT : UI_IDLE);
}

static void valve_refresh(void)
{
    char buf[16];
    bool ok;
    ui_set_text(vp.p, pressure_text(buf, sizeof buf, &ok));
    ui_set_color(vp.p, ok ? UI_TEXT : UI_FAULT);
    bar_width(vp.pfill, ok ? ui_log_px(clicks_state()->p_mmbar, 128) : 0);   // log, 0.1 to 1000 mB
    valve_fast();
    bool on = why_not_manual() == NULL, steps = why_no_steps() == NULL;
    for (int i = 0; i < 8; i++)
        ui_btn_enable(vp.btn[i], i < 6 ? steps : on);
    // The Go to card: live (tint, underline, chevron) when a tap opens the
    // keypad; flat otherwise. In AUTO a tap asks Switch to MANUAL.
    if (on != vp.go_live) {
        vp.go_live = on;
        lv_obj_set_style_bg_color(vp.go_card, lv_color_hex(on ? 0xE9F0F6 : 0xFFFFFF), 0);
        lv_obj_set_style_border_width(vp.go_card, on ? 3 : 0, 0);
        if (on) lv_obj_remove_flag(vp.go_chev, LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_add_flag(vp.go_chev, LV_OBJ_FLAG_HIDDEN);
    }
    if (vp.jog && !stepper3_busy())
        vp.jog = false;
    ui_status_line();
}

const ui_screen_t ui_scr_valve = { "Valve", valve_build, valve_refresh, valve_fast };

/* --------------------------------------------------------------------------
 * Calibrate valve
 * ------------------------------------------------------------------------ */
static struct {
    lv_obj_t *state, *v, *last, *seat, *start;
    lv_obj_t *tile[3], *tcap[3], *tval[3], *time;
} cp;

static const char *const calib_help =
    "Finds 0 % at the limit switch in three steps: 1 fast close to the switch, "
    "2 back off it, 3 slow close onto it. The step running is filled dark; "
    "Motor, steps counts the motor as it moves; Time counts up, then shows how long it took.\n"
    "Takes up to a few minutes. STOP cancels it.\n\n"
    "F-07 switch not found, F-09 switch stuck: check the switch and its wire, "
    "reset the fault, home again.";

static void start_cb(lv_event_t *e)
{
    (void)e;
    if (valve_mode() == VALVE_CALIBRATING)
        ui_msg("Homing: STOP to cancel");
    else if (valve_motion_blocked())
        ui_msg("Reset the fault first");
    else if (valve_auto())
        ui_msg("Switch to MANUAL first");
    else
        ui_calibrate_ask();
}

// Limit switch at 50 Hz, as on Diagnostics.
static void calib_switch(lv_timer_t *t)
{
    (void)t;
    ui_set_text(cp.seat, limit_open_now() ? "Pressed" : "Released");
}

static void timer_deleted(lv_event_t *e)
{
    lv_timer_delete(lv_event_get_user_data(e));
}

static void calib_build(lv_obj_t *scr)
{
    cp.state = ui_info(scr, 10, 48, 226, 48, "Homing");
    cp.v = ui_info(scr, 244, 48, 109, 48, "Motor, steps");
    right(cp.v);
    cp.last = ui_info(scr, 10, 104, 226, 48, "Last homed");   // "15/10/26 14:28"
    cp.seat = ui_info(scr, 244, 104, 109, 48, "Limit switch");
    ui_btn(scr, 361, 48, 109, 48, UI_NAV, "Help", help_cb, (void *)calib_help);
    cp.start = ui_btn(scr, 361, 104, 109, 48, UI_PRIMARY, "Start homing", start_cb, NULL);
    // The three steps as one card split in three, the one running filled
    // dark (Ryan, 16 Oct: the row should read as one sequence).
    static const char *const cap[3] = { "1 Fast close", "2 Back off", "3 Slow close" };
    lv_obj_t *steps = ui_card(scr, 10, 160, 343, 48);
    lv_obj_set_style_clip_corner(steps, true, 0);
    for (int i = 0; i < 3; i++) {
        cp.tval[i] = ui_info(steps, i * 114 + (i > 1), 0, i == 1 ? 115 : 114, 48, cap[i]);
        cp.tile[i] = lv_obj_get_parent(cp.tval[i]);
        cp.tcap[i] = lv_obj_get_child(cp.tile[i], 0);
        lv_obj_set_style_radius(cp.tile[i], 0, 0);
        if (i) {
            lv_obj_set_style_border_side(cp.tile[i], LV_BORDER_SIDE_LEFT, 0);
            lv_obj_set_style_border_width(cp.tile[i], 1, 0);
            lv_obj_set_style_border_color(cp.tile[i], UI_TICK, 0);
        }
    }
    // Time into the calibration, then how long the last one took.
    cp.time = ui_info(scr, 361, 160, 109, 48, "Time taken");
    lv_timer_t *t = lv_timer_create(calib_switch, 20, NULL);
    lv_obj_add_event_cb(scr, timer_deleted, LV_EVENT_DELETE, t);
    calib_switch(t);
}

// Step count at 10 Hz: it moves in every step, where the valve % cannot.
static void calib_fast(void)
{
    char buf[16];
    lv_snprintf(buf, sizeof buf, "%ld", (long)stepper3_position());
    ui_set_text(cp.v, buf);
}

static void tile_set(int i, int state)   // 0 waiting, 1 running, 2 done
{
    static const char *const word[3] = { "--", "Moving", "Done" };
    lv_color_t bg = state == 1 ? UI_TEXT : UI_CARD;
    ui_set_text(cp.tval[i], word[state]);
    if (!lv_color_eq(lv_obj_get_style_bg_color(cp.tile[i], 0), bg)) {
        lv_obj_set_style_bg_color(cp.tile[i], bg, 0);
        lv_obj_set_style_text_color(cp.tcap[i], state == 1 ? lv_color_white() : UI_MUTED, 0);
        lv_obj_set_style_text_color(cp.tval[i], state == 1 ? lv_color_white() : UI_TEXT, 0);
    }
}

static void calib_refresh(void)
{
    char w[16];
    bool calib = valve_mode() == VALVE_CALIBRATING;
    int step = valve_cal_step();
    static const char *const doing[4] = { "Moving off the switch", "Fast close to switch",
                                          "Backing off switch", "Slow close to switch" };
    if (calib)
        ui_set_text(cp.state, doing[step < 0 ? 0 : step]);
    else
        ui_set_text(cp.state, valve_calibrated() ? "Homed" : "Not homed");
    // Done after a calibration this power-up, not after Use saved position.
    uint32_t age = 0, took = 0;
    bool ran = valve_last_cal(&age, &took);
    for (int i = 0; i < 3; i++)
        tile_set(i, calib ? (step > i + 1 ? 2 : step == i + 1 ? 1 : 0) : valve_calibrated() && ran ? 2 : 0);
    char tm[16];
    uint32_t s = calib ? valve_cal_elapsed_s() : took;
    if (calib || (ran && valve_calibrated()))
        lv_snprintf(tm, sizeof tm, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
    else
        lv_strlcpy(tm, "--", sizeof tm);
    ui_set_text(cp.time, tm);
    calib_fast();
    if (ran) {
        const char *iso = valve_last_cal_iso();
        char d[20];
        ui_set_text(cp.last, iso[0] ? ui_date(d, sizeof d, iso) : ui_when(w, sizeof w, age));
    } else {
        ui_set_text(cp.last, "Not since power-up");
    }
    ui_btn_text(cp.start, calib ? "Homing" : "Start homing", NULL);
    ui_btn_enable(cp.start, !calib && !valve_motion_blocked() && !valve_auto());
    ui_status_line();
}

const ui_screen_t ui_scr_calib = { "Valve homing", calib_build, calib_refresh, calib_fast };

/* --------------------------------------------------------------------------
 * Faults and log
 * ------------------------------------------------------------------------ */
#define LOG_ROWS 40
static struct {
    lv_obj_t *list, *none, *reset, *log, *box, *page[2];
    int nf;                   // rows built
    char sig[64];             // tags shown, to rebuild on change
    uint32_t log_total;
    int detail;               // fault shown in the detail, -1 none
    lv_obj_t *d_title, *d_when, *d_text[4], *d_next;
} fp;
static char log_text[LOG_ROWS * 112];

typedef struct {
    const char *tag, *happened, *controller, *check, *reset;
} fault_help_t;

static const fault_help_t fault_help[] = {
    { "F-01", "The Druck reading is missing or outside 0.005-11 mV/V.",
      "Pressure shows ----. The valve still moves.",
      "The Druck cable and plug, and the Power Monitor Click.",
      "The reading is back." },
    { "F-03", "The Druck excitation left 9.5-10.5 V, or the Boost 10 tripped.",
      "Pressure not trusted. The valve still moves.",
      "The Boost 10 and the 5 V supply. A trip needs a power cycle.",
      "The excitation is back in the window." },
    { "F-07", "Homing, the valve closed 1.1 x full travel without the limit switch.",
      "Stopped. The valve is not homed.",
      "The switch and its wire, and that the valve turns.",
      "Any time. Then home again." },
    { "F-09", "The limit switch stayed pressed after the valve moved off it.",
      "Stopped. The valve is not homed.",
      "The switch is not stuck (ice) and its wire is not broken.",
      "Any time. Then home again." },
    { "F-10", "The valve moved in AUTO but the pressure did not respond, 5 windows of 30 s.",
      "Stopped the valve and holds it. MANUAL.",
      "Drive slipping, motor unplugged, needle seized.",
      "Any time. Check before starting AUTO." },
    { "F-16", "The valve kept opening in AUTO while the pressure kept falling.",
      "Stopped the valve and holds it. MANUAL.",
      "Helium supply (bath empty, capillary blocked). On first use, Pressure response.",
      "Any time. Check before starting AUTO." },
    { "F-17", "The valve kept closing in AUTO while the pressure kept rising.",
      "Stopped the valve and holds it. MANUAL.",
      "Pump stopped, pumping valve shut, heat load. On first use, Pressure response.",
      "Any time. Check before starting AUTO." },
    { "F-13", "A move took longer than its distance at the set rate, plus 2 s.",
      "Stopped the valve and holds it. Homing kept.",
      "The valve turns freely and the motor 12 V supply.",
      "Any time." },
    { "F-02", "The pressure stayed above the sensor range for 5 s in AUTO.",
      "Stopped the valve and holds it. MANUAL. The valve still moves.",
      "Pumping line and pump; the sensor range.",
      "The pressure is back in the range. Then start AUTO again." },
    { "F-12", "STOP was pressed in AUTO, or CTRL STOP sent.",
      "Stopped the valve and holds it, not closed.",
      "Why it was stopped.",
      "Any time." },
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
    case VALVE_PAUSED:      return "AUTO PAUSED";
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

static void detail_open(int i)
{
    lv_obj_t *o = ui_overlay();
    fp.detail = i;
    lv_obj_t *c = ui_card(o, 10, 8, 460, 160);
    fp.d_title = ui_label(c, 10, 6, 260, &ui_font_16, UI_FAULT, LV_TEXT_ALIGN_LEFT, "");
    fp.d_when = ui_label(c, 270, 8, 180, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_RIGHT, "");
    static const char *const caps[4] = { "Happened", "Controller", "Check", "Reset when" };
    for (int k = 0; k < 4; k++) {
        ui_label(c, 10, 30 + k * 32, 84, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT, caps[k]);
        fp.d_text[k] = ui_label(c, 98, 30 + k * 32, 352, &ui_font_12, UI_TEXT, LV_TEXT_ALIGN_LEFT, "");
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
    ui_label(ev, 10, 4, 300, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT, "Events, newest first");
    lv_obj_t *box = lv_obj_create(ev);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, 10, 22);
    lv_obj_set_size(box, 329, 80);
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
        lv_obj_t *b = ui_btn(scr, 361, i ? 160 : 104, 109, 48, UI_SECONDARY,
                             i ? LV_SYMBOL_DOWN : LV_SYMBOL_UP, NULL, NULL);
        lv_obj_add_event_cb(b, log_page, LV_EVENT_PRESSED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(b, log_page, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)i);
        fp.page[i] = b;
    }
    fp.log = ui_label(box, 0, 0, 317, &ui_font_12, UI_TEXT, LV_TEXT_ALIGN_LEFT, "");
    lv_label_set_long_mode(fp.log, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(fp.log, 2, 0);
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
        ui_label(fp.list, 10, 14, 300, &ui_font_16, UI_IDLE, LV_TEXT_ALIGN_LEFT, "No faults");
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
        ui_label(r, 10, 5, 300, &ui_font_16, UI_FAULT, LV_TEXT_ALIGN_LEFT, t);
        lv_snprintf(t, sizeof t, "%s  %s", ui_when(w, sizeof w, f.age_ms), mode_word(f.mode));
        ui_label(r, 10, 27, 300, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT, t);
        ui_label(r, 318, 14, 0, &lv_font_montserrat_14, UI_IDLE, LV_TEXT_ALIGN_LEFT, LV_SYMBOL_RIGHT);
    }
}

static void faults_log(void)
{
    uint32_t total = valve_event_total();
    if (total == fp.log_total)
        return;
    fp.log_total = total;
    size_t len = 0;
    log_text[0] = '\0';
    for (int i = 0; i < LOG_ROWS; i++) {
        uint32_t age;
        const char *tag, *text;
        char w[16];
        if (!valve_event_get(i, &age, &tag, &text))
            break;
        lv_snprintf(log_text + len, sizeof log_text - len, "%s%s  %s  %s", i ? "\n" : "",
                    ui_when(w, sizeof w, age), tag, text);
        len += strlen(log_text + len);
    }
    lv_label_set_text_static(fp.log, log_text);
    log_buttons();
}

static void faults_refresh(void)
{
    faults_rows();
    faults_log();
    ui_btn_enable(fp.reset, valve_fault_count() > 0);
    if (fp.detail >= 0 && ui_overlay_open())
        detail_fill();
    else
        fp.detail = -1;
    char t[48];
    int n = valve_fault_count();
    lv_snprintf(t, sizeof t, n ? "%d latched. Tap one for detail." : "No faults latched", n);
    ui_strip_set(UI_STRIP_INFO, UI_MUTED, t);
}

const ui_screen_t ui_scr_faults = { "Faults and log", faults_build, faults_refresh, NULL };

/* --------------------------------------------------------------------------
 * Diagnostics
 * ------------------------------------------------------------------------ */
static struct {
    lv_obj_t *v[9];
} dp;

// Valve motor row: position, limit switch and steps past it, at 50 Hz so
// the switch shows as the hardware reads it (limit.c samples every 5 ms).
static void diag_motor(lv_timer_t *t)
{
    (void)t;
    char buf[16];
    lv_snprintf(buf, sizeof buf, "%ld", (long)stepper3_position());
    ui_set_text(dp.v[3], buf);
    bool pressed = limit_open_now();
    ui_set_text(dp.v[4], pressed ? "Pressed" : "Released");
    int32_t beyond;
    if (pressed && valve_beyond_switch(&beyond))
        lv_snprintf(buf, sizeof buf, "%ld", (long)beyond);
    else
        lv_strlcpy(buf, "--", sizeof buf);
    ui_set_text(dp.v[5], buf);
}


static void diag_build(lv_obj_t *scr)
{
    static const char *const rows[3] = { "Pressure sensor", "Valve motor", "Last move" };
    static const char *const caps[9] = { "Signal, mV", "Supply, V", "Before cal, mB",
                                         "Position, steps", "Limit switch", "Beyond limit",
                                         "Result", "Moved, steps", "Took, s" };
    static const int32_t x[3] = { 98, 225, 351 };
    for (int r = 0; r < 3; r++) {
        // 16 px as the Menu captions, centred on the row (Ryan, 22 Oct).
        lv_obj_t *l = ui_label(scr, 10, 0, 84, &ui_font_16, UI_MUTED, LV_TEXT_ALIGN_LEFT, rows[r]);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_update_layout(l);
        lv_obj_set_y(l, 48 + r * 56 + (48 - lv_obj_get_height(l)) / 2);
        for (int c = 0; c < 3; c++)
            dp.v[r * 3 + c] = ui_info(scr, x[c], 48 + r * 56, 119, 48, caps[r * 3 + c]);
    }
    lv_timer_t *t = lv_timer_create(diag_motor, 20, NULL);
    lv_obj_add_event_cb(scr, timer_deleted, LV_EVENT_DELETE, t);
    diag_motor(t);
}

static void diag_refresh(void)
{
    const clicks_state_t *s = clicks_state();
    char buf[48];
    bool sig = s->pm_found && !s->pm_diag;
    int32_t uv = s->shunt_nv / 1000;    // Druck output, 0.001 mV
    int32_t au = uv < 0 ? -uv : uv;
    lv_snprintf(buf, sizeof buf, "%s%ld.%03ld", uv < 0 ? "-" : "", (long)(au / 1000), (long)(au % 1000));
    ui_set_text(dp.v[0], sig ? buf : "----");
    lv_snprintf(buf, sizeof buf, "%ld.%02ld", (long)(s->bus_mv / 1000), (long)(s->bus_mv % 1000 / 10));
    ui_set_text(dp.v[1], sig ? buf : "----");
    ui_set_text(dp.v[2], sig ? ui_fmt1(buf, sizeof buf, (int64_t)s->r_ppb * 1034214 / 10000000) : "----");

    int32_t from;
    uint32_t ms;
    bool any = stepper3_last_move(&from, &ms);
    const char *res = "None yet";
    if (stepper3_busy() && any) res = "Moving";
    else switch (stepper3_last_stop()) {
        case STEPPER3_STOP_DONE:    res = "At target"; break;
        case STEPPER3_STOP_LIMIT:   res = "At limit"; break;
        case STEPPER3_STOP_CMD:     res = "Stopped"; break;
        case STEPPER3_STOP_TIMEOUT: res = "Timed out"; break;
        default: break;
    }
    ui_set_text(dp.v[6], res);
    lv_snprintf(buf, sizeof buf, "%ld", (long)(stepper3_position() - from));
    ui_set_text(dp.v[7], any ? buf : "--");
    ui_set_text(dp.v[8], any ? ui_fmt1(buf, sizeof buf, ms) : "--");

    char t[16];
    lv_snprintf(buf, sizeof buf, "Firmware %s. Board %s \xC2\xB0" "C. Link %s.", PMC_FW_VERSION,
                ui_fmt1(t, sizeof t, s->die_mc), PMC_LINK_ETHERNET ? "Ethernet" : "USB");
    ui_strip_set(UI_STRIP_INFO, UI_MUTED, buf);
}

const ui_screen_t ui_scr_diag = { "Diagnostics", diag_build, diag_refresh, NULL };
