#include <math.h>
#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "motor_screen.h"
#include "stepper3.h"
#include "link.h"
#include "limit.h"
#include "valve.h"
#include "cfg.h"
#include "ui.h"
#include "lv_port_indev.h"

/*
 * Motor > Jog.
 *
 * Close, Limit override and Open in a row, 109 x 104 each (x 10, 127, 244,
 * y 104). Close and Open move while pressed (hold to run), at the Jog
 * speed, and ramp down on release. Limit override is a two-finger deadman:
 * hold it and press Close or Open with another finger (Close then goes
 * past the switch). The step count is on the strip; Speed, Start, Accel,
 * Hold torque and Direction are on Motor > Settings; STOP or a restart
 * turns the coils off.
 *
 * The Motor page (ui_settings.c) makes the row in its Jog tab, calls
 * motor_jog_fast at 10 Hz and motor_jog_strip for the strip. The row and
 * its timer go when the tab or the page does.
 */
static struct {
    lv_obj_t *close, *open, *over;
    lv_obj_t *held;   // the Close or Open button driving a hold move
    int32_t held_to;  // that move's target, so a release stops only it
    int32_t held2_to; // the same for held2
    lv_obj_t *held2;  // Close or Open under the second finger, Limit override held
    lv_timer_t *two;  // reads the second finger while the row is up
} m;
// Limit override is held (accepted on press); a second finger may move.
static bool over_on;
// The second finger has been off the glass since Limit override was
// pressed, so its next touch is a fresh press.
static bool second_up;
// A hold move asks for this far and is ramped down on release.
#define HOLD_MOVE 200000

// A move from rest at the Jog speed (Motor > Settings > Jog), not
// the run speed AUTO and homing use.
static bool jog_move(int32_t steps)
{
    return stepper3_goto_rate(stepper3_position() + steps, (uint32_t)cfg_jog_rate());
}

static intptr_t event_arg(lv_event_t *e)
{
    return (intptr_t)lv_event_get_user_data(e);
}

/* --------------------------------------------------------------------------
 * Events (on press; a greyed button does nothing)
 * ------------------------------------------------------------------------ */
// Open ends at 100 % once the valve is homed or set; further needs Limit
// override.
#define TOP_WHY "At 100 %: hold Limit override to open further"

static bool at_top(void)
{
    return valve_calibrated() && stepper3_position() >= valve_pos_max();
}

static void move_event(lv_event_t *e)
{
    const bool open = event_arg(e) > 0;
    if (ui_btn_disabled(e)) {
        // A greyed button says why.
        const char *lw = stepper3_locked();
        ui_msg(lw ? lw : open ? "Wait for the valve to stop" : "Limit switch pressed: hold Limit override to close");
        return;
    }
    if (stepper3_busy())
        return;   // a move starts from rest only; the buttons stay lit meanwhile
    const char *why = stepper3_locked();
    if (!why && open && at_top()) {
        ui_msg(TOP_WHY);
        return;
    }
    int32_t steps = HOLD_MOVE * (int32_t)event_arg(e);
    if (open && valve_calibrated() && stepper3_position() + steps > valve_pos_max())
        steps = valve_pos_max() - stepper3_position();
    if (!why && jog_move(steps)) {
        m.held = lv_event_get_target_obj(e);
        m.held_to = stepper3_target();
        valve_hold(true);
    } else {
        if (!why)
            why = stepper3_refusal();
        ui_msg(why);
    }
    motor_jog_fast();
}

// Hold move released (or the finger slid off): ramp down where it is.
static void hold_end(void)
{
    if (!m.held)
        return;
    m.held = NULL;
    valve_hold(false);
    if (stepper3_busy() && stepper3_target() == m.held_to) {   // its own move, not a later one
        stepper3_stop_ramped();
    }
}

static void release_event(lv_event_t *e)
{
    if (m.held == lv_event_get_target_obj(e))
        hold_end();
}

// Limit override is a two-finger deadman. Held alone it does nothing. While it is held, a second finger
// pressing Close closes past the switch, and Open opens. The move stops
// when either finger lifts or the second slides off its button: a close
// past the switch at once, an open ramped down. LVGL follows one finger
// (the one on Limit override); the second comes from touch_second() and
// is read by two_poll.
static uint32_t over_at;   // when Limit override was pressed

static void override_event(lv_event_t *e)
{
    const char *why = stepper3_locked();
    if (ui_btn_disabled(e)) {
        ui_msg(why ? why : "Wait for the valve to stop");   // greyed: why
        return;
    }
    over_at = lv_tick_get();
    if (why) {
        ui_msg(why);
        return;
    }
    over_on = true;
    second_up = !touch_second(NULL, NULL);   // a finger already down waits to lift
    valve_event("LIM", "Limit override held (screen): press Close or Open to move");
    ui_refresh_soon();
}

static bool inside(lv_obj_t *b, int32_t x, int32_t y)
{
    lv_area_t a;
    lv_obj_get_coords(b, &a);
    return x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2;
}

// The second finger's move ends: a close past the switch stops at once and
// the override ends; an open ramps down.
static void two_end(void)
{
    lv_obj_t *b = m.held2;
    if (!b)
        return;
    m.held2 = NULL;
    valve_hold(false);
    lv_obj_remove_state(b, LV_STATE_PRESSED);
    if (stepper3_override()) {
        bool stopped = stepper3_override_cancel();
        valve_event("LIM", "Limit override move ended (screen)%s", stopped ? ", valve stopped" : "");
    } else if (stepper3_busy() && stepper3_target() == m.held2_to) {   // its own move, not a later one
        stepper3_stop_ramped();
    }
}

// Second finger pressed b (Close or Open) while Limit override is held.
// False while a move is still running, so it is tried again.
static bool two_start(lv_obj_t *b)
{
    if (stepper3_busy())
        return false;
    bool close = b == m.close;
    int32_t steps = HOLD_MOVE * (close ? -1 : 1);
    const char *why = stepper3_locked();
    if (!why) {
        if (close)
            stepper3_override_arm();
        if (jog_move(steps)) {
            m.held2 = b;
            m.held2_to = stepper3_target();
            valve_hold(true);
            lv_obj_add_state(b, LV_STATE_PRESSED);
            valve_event("LIM", close ? "Limit override (screen): closing past the switch"
                                     : "Limit override (screen): opening");
            return true;
        }
        if (close)
            stepper3_override_cancel();
        why = stepper3_refusal();
    }
    ui_msg(why);
    return true;
}

static void over_release(lv_event_t *e)
{
    (void)e;
    if (!over_on)
        return;
    over_on = false;
    bool moving = m.held2 != NULL;
    two_end();
    if (!moving)
        valve_event("LIM", "Limit override released (screen)");
    // A short tap did nothing: what the button is for.
    if (!moving && lv_tick_elaps(over_at) < 600)
        ui_msg("Hold Limit override, then press Close");
    ui_refresh_soon();
}

static void two_poll(lv_timer_t *t)
{
    (void)t;
    int32_t x = 0, y = 0;
    bool down = touch_second(&x, &y);
    // Released, even if the release event was missed.
    if (over_on && !lv_obj_has_state(m.over, LV_STATE_PRESSED))
        over_release(NULL);
    if (!over_on)
        return;
    if (m.held2) {
        if (!down || !inside(m.held2, x, y)) {
            two_end();
            motor_jog_fast();
        }
        return;
    }
    if (!down) {
        second_up = true;
        return;
    }
    if (!second_up)
        return;
    lv_obj_t *b = inside(m.close, x, y) ? m.close : inside(m.open, x, y) ? m.open : NULL;
    if (!b) {
        second_up = false;   // landed elsewhere: lift it and press again
        return;
    }
    if (two_start(b)) {
        second_up = false;
        motor_jog_fast();
    }
}

// The row goes (a tab change, leaving the page): its timer and any move
// it drives go with it.
static void jog_delete(lv_event_t *e)
{
    (void)e;
    if (m.two)
        lv_timer_delete(m.two);
    m.two = NULL;
    two_end();
    hold_end();
    over_on = false;
    memset(&m, 0, sizeof m);
}

/* --------------------------------------------------------------------------
 * Layout
 * ------------------------------------------------------------------------ */
// Close's and Open's arrow, 40 x 26: a head and a shaft, drawn in the
// word's colour so it greys with the button.
#define ARROW_W 40
#define ARROW_H 26
static void arrow_draw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t *o = lv_event_get_target_obj(e);
    bool left = lv_obj_get_user_data(o) != NULL;
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    lv_color_t c = lv_obj_get_style_text_color(lv_obj_get_child(lv_obj_get_parent(o), 0), LV_PART_MAIN);
    int32_t x = a.x1, y = a.y1, tip = left ? x : x + ARROW_W - 1, base = left ? x + 18 : x + ARROW_W - 19;
    lv_draw_triangle_dsc_t t;
    lv_draw_triangle_dsc_init(&t);
    t.color = c;
    t.p[0].x = tip;  t.p[0].y = y + ARROW_H / 2;
    t.p[1].x = base; t.p[1].y = y;
    t.p[2].x = base; t.p[2].y = y + ARROW_H;
    lv_draw_triangle(layer, &t);
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = c;
    lv_area_t sh = left ? (lv_area_t){ base - 1, y + 9, x + ARROW_W - 1, y + 17 }
                        : (lv_area_t){ x, y + 9, base + 1, y + 17 };
    lv_draw_rect(layer, &r, &sh);
}

// A 109 x 104 button: an arrow over the word, the two centred together, or
// two lines (Limit override).
static lv_obj_t *big(lv_obj_t *p, int32_t x, const char *text, int arrow, lv_event_cb_t cb, intptr_t arg)
{
    lv_obj_t *b = ui_btn(p, x, 104, 109, 104, UI_SECONDARY, text, NULL, NULL);
    lv_obj_add_event_cb(b, cb, LV_EVENT_PRESSED, (void *)arg);
    if (arrow) {
        lv_obj_t *i = lv_obj_create(b);
        lv_obj_remove_style_all(i);
        lv_obj_remove_flag(i, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(i, ARROW_W, ARROW_H + 1);
        lv_obj_set_user_data(i, arrow < 0 ? (void *)1 : NULL);
        lv_obj_add_event_cb(i, arrow_draw, LV_EVENT_DRAW_MAIN, NULL);
        lv_obj_align(i, LV_ALIGN_TOP_MID, 0, 26);
        lv_obj_align(lv_obj_get_child(b, 0), LV_ALIGN_TOP_MID, 0, 62);
    }
    return b;
}

void motor_jog_make(lv_obj_t *p)
{
    memset(&m, 0, sizeof m);
    m.close = big(p, 10, "Close", -1, move_event, -1);
    m.over = big(p, 127, "Limit\noverride", 0, override_event, 0);
    m.open = big(p, 244, "Open", 1, move_event, 1);
    // A plain button at rest (a dark outline would mean "selected"), filled
    // dark while it is held.
    lv_obj_t *const held_btns[2] = { m.close, m.open };
    for (int i = 0; i < 2; i++) {
        lv_obj_add_event_cb(held_btns[i], release_event, LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(held_btns[i], release_event, LV_EVENT_PRESS_LOST, NULL);
    }
    lv_obj_add_event_cb(m.over, over_release, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(m.over, over_release, LV_EVENT_PRESS_LOST, NULL);
    // The second finger, every 20 ms; the timer goes with the row.
    over_on = false;
    m.two = lv_timer_create(two_poll, 20, NULL);
    lv_obj_add_event_cb(m.over, jog_delete, LV_EVENT_DELETE, NULL);
    motor_jog_fast();
}

/* --------------------------------------------------------------------------
 * Refresh: fast() at 10 Hz; unchanged values redraw nothing.
 * ------------------------------------------------------------------------ */
void motor_jog_fast(void)
{
    if (!m.over)
        return;
    bool busy = stepper3_busy();

    // A hold move ends when its button is no longer pressed, even if the
    // release event was missed; one stopped by the limit is simply over.
    if (m.held && (!busy || !lv_obj_has_state(m.held, LV_STATE_PRESSED))) {
        if (m.held == m.open && !busy && at_top())
            ui_msg(TOP_WHY);   // Open ran to 100 % and ended there
        hold_end();
    }

    // The strip follows the limit override and the motor within 0.1 s, not
    // at the next 0.5 s refresh, so it keeps up with the button.
    static int shown = -1;
    int now = (over_on ? 1 : 0) | (m.held2 ? 2 : 0) | (stepper3_override() ? 4 : 0) |
              ((stepper3_direction() + 1) << 3) | (stepper3_limit_open() ? 32 : 0);
    if (now != shown) {
        if (shown >= 0)
            ui_refresh_soon();
        shown = now;
    }

    // Close is greyed while the limit stop reads open, unless the override
    // is on; Limit override while the motor is locked. A move does not grey
    // the row; a press during a move does nothing. All three greyed in
    // AUTO; a tap still says why.
    bool ov = stepper3_override() || over_on;
    // And with the motor locked (a fault, homing): all three alike, label
    // included, so Close and Open do not look live under F-2.
    bool au = stepper3_locked() != NULL && !m.held && !m.held2;
    ui_btn_enable(m.close, !au && (!stepper3_limit_open() || ov || m.held == m.close));
    ui_btn_enable(m.open, !au);
    ui_btn_enable(m.over, !stepper3_locked() || ov);
    lv_color_t ob = ov ? UI_WARN : UI_CARD;
    if (!lv_color_eq(lv_obj_get_style_bg_color(m.over, 0), ob)) {
        // Pressed too: it is held while it drives, and the pressed shade
        // would hide that it is on.
        lv_obj_set_style_bg_color(m.over, ob, 0);
        if (ov)
            lv_obj_set_style_bg_color(m.over, ob, LV_STATE_PRESSED);
        else
            lv_obj_remove_local_style_prop(m.over, LV_STYLE_BG_COLOR, LV_STATE_PRESSED);
        // At rest the label takes the button's colour, so it greys with it
        // (a local dark one would keep it dark on the greyed button).
        for (uint32_t i = 0; i < lv_obj_get_child_count(m.over); i++) {
            lv_obj_t *l = lv_obj_get_child(m.over, (int32_t)i);
            if (ov) lv_obj_set_style_text_color(l, lv_color_white(), 0);
            else    lv_obj_remove_local_style_prop(l, LV_STYLE_TEXT_COLOR, 0);
        }
    }
}

// The strip: the steps and what the motor does ("0 steps. Idle",
// "-120 steps. Closing past the switch"); Limit override held, or the
// switch blocking a close, as a dark warning. A latched fault or a
// message shows over it, as on every screen.
void motor_jog_strip(void)
{
    char t[96];
    int8_t d = stepper3_direction();
    bool warn = false;
    // "Past the switch" only with the switch pressed; at rest "Valve idle"
    // as Home, "AUTO" in AUTO.
    const char *st = d > 0 ? "Opening"
                   : d < 0 ? (!stepper3_override() ? "Closing" : stepper3_limit_open() ? "Closing past the switch"
                                                                                       : "Closing, limit switch overridden")
                   : valve_auto() ? "AUTO" : "Valve idle";
    if (d < 0 && stepper3_override())
        warn = true;
    char n[24];
    ui_steps(n, sizeof n, stepper3_position());
    lv_snprintf(t, sizeof t, "%s. %s", n, st);
    if (d == 0 && stepper3_limit_fitted() && limit_state() == LIMIT_OPEN && !stepper3_limit_ignored()) {
        lv_snprintf(t, sizeof t, "%s. Limit switch pressed: hold Limit override to close", n);
    }
    // Limit override held, nothing moving yet: what the second finger does.
    if (over_on && !m.held2) {
        lv_strlcpy(t, at_top() ? "Limit override held: press Close or Open" : "Limit override held: press Close", sizeof t);
        warn = true;
    }
    // The override lines white with the triangle, as the frame's.
    ui_strip_set(UI_STRIP_INFO, warn ? LV_SYMBOL_WARNING : UI_ICON_VALVE, t);
}

// Full travel (POS_MAX steps) at the start, run and accel rates in use: up
// to speed, at speed, and down again, as stepper3 ramps.
const char *motor_travel_text(char *buf, size_t n)
{
    float steps = (float)valve_pos_max(), r = (float)stepper3_rate(), s0 = (float)stepper3_start_rate();
    float a = (float)stepper3_accel(), t;
    if (r < s0)
        r = s0;
    float d = (r * r - s0 * s0) / (2.0f * a);   // steps to reach the run rate
    if (2.0f * d >= steps) {
        float v = sqrtf(s0 * s0 + a * steps);   // peak rate halfway
        t = 2.0f * (v - s0) / a;
    } else {
        t = 2.0f * (r - s0) / a + (steps - 2.0f * d) / r;
    }
    uint32_t sec = (uint32_t)(t + 0.5f);
    // "17 s", "5 min", "2 min 45 s".
    unsigned long m = sec / 60, s = sec % 60;
    if (!m)
        lv_snprintf(buf, n, "%lu s", s);
    else if (!s)
        lv_snprintf(buf, n, "%lu min", m);
    else
        lv_snprintf(buf, n, "%lu min %lu s", m, s);
    return buf;
}
