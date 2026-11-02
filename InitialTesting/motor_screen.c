#include <math.h>
#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "motor_screen.h"
#include "stepper3.h"
#include "link.h"
#include "limit.h"
#include "valve.h"
#include "ui.h"
#include "lv_port_indev.h"

/*
 * Settings > Motor override (0.11.32: option B, Ryan 22 Oct; three tabs
 * since 0.11.33, Ryan 23 Oct). A framework screen: the standard header,
 * status strip and STOP; every MOT setting and move from the touchscreen,
 * and the screen follows MOT commands on the link.
 * Body y 48-208, three 48 px rows (y 48, 104, 160). The top row is on every
 * tab: the tabs Move / Speed / Coils (x10 w226) and the position card (x244
 * w226). Below it:
 *   Move   Hold to run (move while pressed, on by default) or a step size 1 to 10k, one
 *          choice; then Limit override, Close, Open and Set zero. Limit
 *          override is a two-finger deadman: hold it and press Close or
 *          Open with another finger (Close then goes past the switch)
 *   Speed  Run, Start and Accel [-] value [+], and the time for a full open or close
 *   Coils  Mode WAVE/FULL/HALF, Hold torque, Coils off, Coil order, Direction
 * Coil order, Direction and Set zero ask first while the valve is homed.
 * The strip reads the motor state and the limit line (a latched fault, the
 * limit override or a message over it, as on every screen).
 * Moves are relative, by the selected step size, in steps of the current
 * mode (half steps in HALF). With Hold, Close and Open move while pressed
 * and ramp down on release. Settings are refused while moving, as on the
 * link. Buttons act on press. Each action is echoed on the link as
 * "MOT screen: ..." so a terminal log shows what was done.
 */
#define TRACK lv_color_hex(0xC3CDD7)

static const int32_t R[3] = { 48, 104, 160 };
#define H 48

enum { TAB_MOVE, TAB_SPEED, TAB_COILS, TAB_N };

static struct {
    lv_obj_t *tabs[TAB_N], *page[TAB_N];
    lv_obj_t *pos;
    lv_obj_t *size[6], *mode[3], *turns, *travel;
    lv_obj_t *rate, *start, *accel, *adj[6];
    lv_obj_t *close, *open, *zero, *over, *hold, *off, *order, *dir;
    lv_obj_t *held;   // the Close or Open button driving a hold move
    int32_t held_to;  // that move's target, so a release stops only it
    int32_t held2_to; // the same for held2
    lv_obj_t *held2;  // Close or Open under the second finger, Limit override held
    lv_timer_t *two;  // reads the second finger while the screen is open
} m;
// Limit override is held (accepted on press); a second finger may move.
static bool over_on;
// The second finger has been off the glass since Limit override was
// pressed, so its next touch is a fresh press.
static bool second_up;
// Hold to run (move while pressed) is the first choice in the step size row
// (0.13.2, Ryan 28 Oct: it looked apart as its own button). On by default
// each time the page opens.
static bool hold_on = true;
static int size_sel = 2, tab_sel;
static const int32_t step_sizes[5] = { 1, 10, 100, 1000, 10000 };
static const char *const step_names[6] = { "Hold to run", "1", "10", "100", "1k", "10k" };
// A hold move asks for this far and is ramped down on release.
#define HOLD_MOVE 200000

// +/- walk these lists; a value set over the link snaps to the nearest entry.
static const uint32_t rates[] = { 16, 25, 50, 75, 100, 125, 150, 200, 250, 300, 400, 500, 600, 800, 1000 };
static const uint32_t accels[] = { 25, 50, 100, 200, 400, 800, 1600, 3200, 6400 };

// The three distinct coil orders for a four-phase unipolar motor; every other
// order is one of these turned or reversed.
static const uint8_t orders[3][4] = { { 0, 1, 2, 3 }, { 0, 2, 1, 3 }, { 0, 1, 3, 2 } };

static intptr_t event_arg(lv_event_t *e)
{
    return (intptr_t)lv_event_get_user_data(e);
}

static size_t nearest(const uint32_t *list, size_t n, uint32_t v)
{
    size_t best = 0;
    for (size_t i = 1; i < n; i++) {
        uint32_t d = list[i] > v ? list[i] - v : v - list[i];
        uint32_t db = list[best] > v ? list[best] - v : v - list[best];
        if (d < db)
            best = i;
    }
    return best;
}

// Next list entry above (dir > 0) or below the current value.
static uint32_t step_list(const uint32_t *list, size_t n, uint32_t v, int dir)
{
    size_t i = nearest(list, n, v);
    if (dir > 0 && list[i] <= v && i + 1 < n)
        i++;
    else if (dir < 0 && list[i] >= v && i > 0)
        i--;
    return list[i];
}

static int order_index(void)
{
    uint8_t o[4];
    stepper3_order(o);
    for (int i = 0; i < 3; i++)
        if (memcmp(o, orders[i], 4) == 0)
            return i;
    return -1;
}

static void motor_fast(void);
static void motor_refresh(void);

/* --------------------------------------------------------------------------
 * Events (on press; a greyed button does nothing)
 * ------------------------------------------------------------------------ */
// The row's choice: Hold to run (0) or a step size (1 to 5). Not during a
// move.
static void size_event(lv_event_t *e)
{
    int i = (int)event_arg(e);
    if (!stepper3_busy()) {
        hold_on = i == 0;
        if (i)
            size_sel = i - 1;
    }
    motor_fast();
}

static void move_event(lv_event_t *e)
{
    if (ui_btn_disabled(e))
        return;
    bool hold = hold_on;
    if (stepper3_busy())
        return;   // a move starts from rest only; the buttons stay lit meanwhile
    int32_t steps = (hold ? HOLD_MOVE : step_sizes[size_sel]) * (int32_t)event_arg(e);
    const char *why = stepper3_locked();
    if (!why && stepper3_move(steps)) {
        if (hold) {
            m.held = lv_event_get_target_obj(e);
            m.held_to = stepper3_target();
            valve_hold(true);
            link_printf("MOT screen: hold move %s\r\n", steps > 0 ? "open" : "close");
        } else {
            link_printf("MOT screen: move %ld\r\n", (long)steps);
        }
    } else {
        if (!why)
            why = stepper3_refusal();
        link_printf("MOT screen: move refused, %s\r\n", why);
        ui_msg(why);
    }
    motor_fast();
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
        link_printf("MOT screen: hold move released\r\n");
    }
}

static void release_event(lv_event_t *e)
{
    if (m.held == lv_event_get_target_obj(e))
        hold_end();
}

/* Coil order, direction and Zero clear the homing. While the valve is
 * homed they ask first (Ryan, 23 Oct). Not homed, they apply at once as
 * before. A mode change keeps the homing (the count is rescaled). */
static char ask_text[64];

static void guarded(const char *what, const char *ok, void (*act)(void))
{
    if (!valve_calibrated()) {
        act();
        return;
    }
    snprintf(ask_text, sizeof ask_text, "%s? This clears the homing.", what);
    ui_confirm(ask_text, ok, act);
}

static void mode_event(lv_event_t *e)
{
    if (stepper3_busy() || stepper3_locked())
        return;
    stepper3_mode_t md = (stepper3_mode_t)event_arg(e);
    if (stepper3_set_mode(md))
        link_printf("MOT screen: mode %s\r\n", stepper3_mode_name(md));
    motor_fast();
}

enum { SET_RATE, SET_START, SET_ACCEL };

static void adjust_event(lv_event_t *e)
{
    if (ui_btn_disabled(e))
        return;
    intptr_t a = event_arg(e);
    int dir = a & 1 ? 1 : -1;
    switch (a >> 1) {
    case SET_RATE: {
        uint32_t v = step_list(rates, (sizeof rates / sizeof rates[0]), stepper3_rate(), dir);
        if (stepper3_set_rate(v))
            link_printf("MOT screen: rate %lu\r\n", (unsigned long)v);
        break;
    }
    case SET_START: {
        uint32_t v = step_list(rates, (sizeof rates / sizeof rates[0]), stepper3_start_rate(), dir);
        if (stepper3_set_start(v))
            link_printf("MOT screen: start %lu\r\n", (unsigned long)v);
        break;
    }
    default: {
        uint32_t v = step_list(accels, (sizeof accels / sizeof accels[0]), stepper3_accel(), dir);
        if (stepper3_set_accel(v))
            link_printf("MOT screen: accel %lu\r\n", (unsigned long)v);
        break;
    }
    }
    motor_fast();
}

// Coil order and Direction clear the homing: refused while the valve layer
// locks the motor (AUTO, homing, a latched fault), as MOT ORDER and MOT DIR
// are on the link. Checked again here: the question may have waited.
static bool ref_locked(void)
{
    const char *why = stepper3_locked();
    if (why)
        ui_msg(why);
    return why != NULL;
}

static void do_order(void)
{
    int i = (order_index() + 1) % 3;
    if (ref_locked())
        return;
    if (!stepper3_busy() && stepper3_set_order(orders[i]))
        link_printf("MOT screen: order %d%d%d%d\r\n",
                    orders[i][0], orders[i][1], orders[i][2], orders[i][3]);
    motor_fast();
}

static void order_event(lv_event_t *e)
{
    if (ui_btn_disabled(e))
        return;
    guarded("Change the coil order", "Change", do_order);
}

static void do_dir(void)
{
    if (ref_locked())
        return;
    if (!stepper3_busy() && stepper3_set_reverse(!stepper3_reverse()))
        link_printf("MOT screen: dir %d\r\n", stepper3_reverse() ? 1 : 0);
    motor_fast();
}

static void dir_event(lv_event_t *e)
{
    if (ui_btn_disabled(e))
        return;
    guarded("Reverse the direction", "Change", do_dir);
}

static void hold_event(lv_event_t *e)
{
    if (ui_btn_disabled(e))
        return;
    if (stepper3_set_hold(!stepper3_hold()))
        link_printf("MOT screen: hold %d\r\n", stepper3_hold() ? 1 : 0);
    motor_fast();
}

static void do_zero(void)
{
    if (!stepper3_busy() && !stepper3_locked()) {
        stepper3_zero();
        link_printf("MOT screen: zero\r\n");
    }
    motor_fast();
}

static void zero_event(lv_event_t *e)
{
    if (ui_btn_disabled(e) || stepper3_busy())
        return;
    guarded("Set this position as 0 steps", "Set zero", do_zero);
}

static void off_event(lv_event_t *e)
{
    (void)e;
    // In AUTO the loop would move (and energise) the valve again at its
    // next step: end AUTO first, as MOT OFF does on the link.
    if (valve_auto())
        valve_stop();
    stepper3_off();
    link_printf("MOT screen: coils off at %ld\r\n", (long)stepper3_position());
    motor_fast();
}

// Limit override is a two-finger deadman (0.13.7, Ryan 28 Oct; one finger
// closed past the switch from 0.13.2, and it armed the next close from
// 0.12.19). Held alone it does nothing. While it is held, a second finger
// pressing Close closes past the switch, and Open opens, by the move
// choice (Hold to run or a step). The move stops when either finger lifts
// or the second slides off its button: a close past the switch at once,
// an open ramped down. LVGL follows one finger (the one on Limit
// override); the second comes from touch_second() and is read by two_poll.
static void override_event(lv_event_t *e)
{
    if (ui_btn_disabled(e))
        return;
    const char *why = stepper3_locked();
    if (why) {
        link_printf("MOT screen: override refused, %s\r\n", why);
        ui_msg(why);
        return;
    }
    over_on = true;
    second_up = !touch_second(NULL, NULL);   // a finger already down waits to lift
    valve_event("LIM", "Limit override held (screen): press Close or Open to move");
    motor_refresh();
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
        link_printf("MOT screen: override move released\r\n");
    }
}

// Second finger pressed b (Close or Open) while Limit override is held.
// False while a move is still running, so it is tried again.
static bool two_start(lv_obj_t *b)
{
    if (stepper3_busy())
        return false;
    bool close = b == m.close;
    int32_t steps = (hold_on ? HOLD_MOVE : step_sizes[size_sel]) * (close ? -1 : 1);
    const char *why = stepper3_locked();
    if (!why) {
        if (close)
            stepper3_override_arm();
        if (stepper3_move(steps)) {
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
    link_printf("MOT screen: override move refused, %s\r\n", why);
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
    motor_refresh();
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
            motor_fast();
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
        motor_fast();
    }
}

static void two_delete(lv_event_t *e)
{
    (void)e;
    if (m.two)
        lv_timer_delete(m.two);
    m.two = NULL;
    two_end();
    over_on = false;
}

static void tab_show(void);

static void tab_event(lv_event_t *e)
{
    tab_sel = (int)event_arg(e);
    tab_show();
}

/* --------------------------------------------------------------------------
 * Layout
 * ------------------------------------------------------------------------ */
static lv_obj_t *btn(lv_obj_t *p, int32_t x, int32_t y, int32_t w, const char *text, lv_event_cb_t cb,
                     intptr_t arg)
{
    lv_obj_t *b = ui_btn(p, x, y, w, H, UI_SECONDARY, text, NULL, NULL);
    lv_obj_add_event_cb(b, cb, LV_EVENT_PRESSED, (void *)arg);
    return b;
}

// Segmented choice as Valve setup's tabs: grey track, the chosen segment
// raised white (seg_show).
// first > 0 makes the first segment that wide, the rest share what is left.
static void seg(lv_obj_t *p, int32_t x, int32_t y, int32_t w, int n, const char *const *labels,
                lv_obj_t **out, lv_event_cb_t cb, int32_t first)
{
    lv_obj_t *t = ui_card(p, x, y, w, H);
    lv_obj_set_style_bg_color(t, TRACK, 0);
    lv_obj_set_style_border_width(t, 0, 0);
    lv_obj_set_style_radius(t, 6, 0);
    const int32_t pad = 4, w0 = first > 0 ? first : (w - 2 * pad) / n;
    const int32_t iw = first > 0 ? (w - 2 * pad - first) / (n - 1) : w0;
    for (int i = 0; i < n; i++) {
        int32_t bx = pad + (i ? w0 + (i - 1) * iw : 0), bw = i ? iw : w0;
        lv_obj_t *b = ui_card(t, bx, pad, bw, H - 2 * pad);
        lv_obj_set_style_radius(b, 4, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, cb, LV_EVENT_PRESSED, (void *)(intptr_t)i);
        lv_obj_t *l = ui_label(b, 0, 0, bw, &ui_font_16, UI_MUTED, LV_TEXT_ALIGN_CENTER, labels[i]);
        lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
        out[i] = b;
    }
}

static void seg_show(lv_obj_t *b, bool sel, bool on)
{
    lv_opa_t o = sel ? LV_OPA_COVER : LV_OPA_TRANSP;
    if (lv_obj_get_style_bg_opa(b, 0) != o)
        lv_obj_set_style_bg_opa(b, o, 0);
    ui_set_color(lv_obj_get_child(b, 0), !on ? UI_IDLE : sel ? UI_TEXT : UI_MUTED);
}

// [-] card [+], the card with a 12 px caption over the value; returns the
// value label.
static lv_obj_t *adjust_row(lv_obj_t *p, int32_t x, int32_t y, int32_t w, int which, const char *cap)
{
    const int32_t bw = 48, cw = w - 2 * bw - 8;
    m.adj[which * 2] = btn(p, x, y, bw, LV_SYMBOL_MINUS, adjust_event, which << 1);
    lv_obj_t *c = ui_card(p, x + bw + 4, y, cw, H);
    ui_label(c, 0, 5, cw, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_CENTER, cap);
    lv_obj_t *v = ui_label(c, 0, 22, cw, &ui_font_16, UI_TEXT, LV_TEXT_ALIGN_CENTER, "");
    m.adj[which * 2 + 1] = btn(p, x + w - bw, y, bw, LV_SYMBOL_PLUS, adjust_event, (which << 1) | 1);
    return v;
}

// A tab's page: rows 2 and 3 only, so it never covers the header or the
// top row.
static lv_obj_t *page(lv_obj_t *scr)
{
    lv_obj_t *pg = lv_obj_create(scr);
    lv_obj_remove_style_all(pg);
    lv_obj_remove_flag(pg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(pg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(pg, 0, R[1]);
    lv_obj_set_size(pg, 480, R[2] + H - R[1]);
    return pg;
}

static void tab_show(void)
{
    for (int i = 0; i < TAB_N; i++) {
        seg_show(m.tabs[i], i == tab_sel, true);
        if (i == tab_sel)
            lv_obj_remove_flag(m.page[i], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(m.page[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void motor_build(lv_obj_t *scr)
{
    memset(&m, 0, sizeof m);
    tab_sel = TAB_MOVE;
    hold_on = true;   // opens on Hold (0.12.19, Ryan 27 Oct)
    const int32_t y2 = 0, y3 = R[2] - R[1];

    // Top row: the tabs and the position (room for -2147483648, Ryan 22 Oct).
    static const char *const tabs[TAB_N] = { "Move", "Speed", "Coils" };
    seg(scr, 10, R[0], 226, TAB_N, tabs, m.tabs, tab_event, 0);
    lv_obj_t *pc = ui_card(scr, 244, R[0], 226, H);
    ui_label(pc, 8, 3, 210, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT, "Steps (+ open)");
    m.pos = ui_label(pc, 0, 0, 210, &ui_num_26, UI_TEXT, LV_TEXT_ALIGN_RIGHT, "0");
    lv_obj_align(m.pos, LV_ALIGN_BOTTOM_RIGHT, -8, -1);
    // Output turns, bottom left (0.12.20, Ryan 27 Oct).
    m.turns = ui_label(pc, 8, 0, 110, &ui_font_16, UI_MUTED, LV_TEXT_ALIGN_LEFT, "");
    lv_obj_align(m.turns, LV_ALIGN_BOTTOM_LEFT, 8, -5);

    // Move: Hold to run or a step size, one choice; then Limit override
    // leftmost, Close, Open, Set zero rightmost (Ryan, 28 Oct).
    lv_obj_t *p = m.page[TAB_MOVE] = page(scr);
    seg(p, 10, y2, 460, 6, step_names, m.size, size_event, 122);
    // Apart from the moves: a warning outline, filled while it drives.
    m.over = btn(p, 10, y3, 109, "Limit\noverride", override_event, 0);
    lv_obj_set_style_border_side(m.over, LV_BORDER_SIDE_FULL, 0);
    lv_obj_set_style_border_width(m.over, 2, 0);
    lv_obj_set_style_border_color(m.over, UI_WARN, 0);
    m.close = btn(p, 127, y3, 109, LV_SYMBOL_LEFT " Close", move_event, -1);
    m.open = btn(p, 244, y3, 109, "Open " LV_SYMBOL_RIGHT, move_event, 1);
    lv_obj_t *const held_btns[2] = { m.close, m.open };
    for (int i = 0; i < 2; i++) {
        lv_obj_add_event_cb(held_btns[i], release_event, LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(held_btns[i], release_event, LV_EVENT_PRESS_LOST, NULL);
    }
    lv_obj_add_event_cb(m.over, over_release, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(m.over, over_release, LV_EVENT_PRESS_LOST, NULL);
    // The second finger, every 20 ms; the timer goes with the screen.
    over_on = false;
    m.two = lv_timer_create(two_poll, 20, NULL);
    lv_obj_add_event_cb(m.over, two_delete, LV_EVENT_DELETE, NULL);
    m.zero = btn(p, 361, y3, 109, "Set zero", zero_event, 0);

    // Speed: the three rates.
    p = m.page[TAB_SPEED] = page(scr);
    m.rate = adjust_row(p, 10, y2, 226, SET_RATE, "Run");
    m.start = adjust_row(p, 244, y2, 226, SET_START, "Start");
    m.accel = adjust_row(p, 10, y3, 226, SET_ACCEL, "Accel");
    // How long a full open or close takes at these rates (Ryan, 28 Oct).
    m.travel = ui_info(p, 244, y3, 226, H, "Full open or close");

    // Coils: mode, hold torque, coils off, order, direction.
    p = m.page[TAB_COILS] = page(scr);
    static const char *modes[3];
    for (int i = 0; i < 3; i++)
        modes[i] = stepper3_mode_name((stepper3_mode_t)i);
    seg(p, 10, y2, 226, 3, modes, m.mode, mode_event, 0);
    m.hold = btn(p, 244, y2, 109, "", hold_event, 0);
    m.off = btn(p, 361, y2, 109, "Coils off", off_event, 0);
    m.order = btn(p, 10, y3, 226, "", order_event, 0);
    m.dir = btn(p, 244, y3, 226, "", dir_event, 0);

    tab_show();
    motor_fast();
}

/* --------------------------------------------------------------------------
 * Refresh: fast() at 10 Hz so the position follows a move; unchanged
 * values redraw nothing.
 * ------------------------------------------------------------------------ */
// Full travel (POS_MAX, in steps of the current mode) at the start, run and
// accel rates: up to speed, at speed, and down again, as stepper3 ramps.
static void travel_show(void)
{
    float n = (float)valve_pos_max(), r = (float)stepper3_rate(), s0 = (float)stepper3_start_rate();
    float a = (float)stepper3_accel(), t;
    if (r < s0)
        r = s0;
    float d = (r * r - s0 * s0) / (2.0f * a);   // steps to reach the run rate
    if (2.0f * d >= n) {
        float v = sqrtf(s0 * s0 + a * n);           // peak rate halfway
        t = 2.0f * (v - s0) / a;
    } else {
        t = 2.0f * (r - s0) / a + (n - 2.0f * d) / r;
    }
    uint32_t sec = (uint32_t)(t + 0.5f);
    char buf[32];
    lv_snprintf(buf, sizeof buf, "%lu min %02lu s", (unsigned long)(sec / 60), (unsigned long)(sec % 60));
    ui_set_text(m.travel, buf);
}

static void motor_fast(void)
{
    char t[48];
    bool busy = stepper3_busy();

    // A hold move ends when its button is no longer pressed, even if the
    // release event was missed; one stopped by the limit is simply over.
    if (m.held && (!busy || !lv_obj_has_state(m.held, LV_STATE_PRESSED)))
        hold_end();

    // The strip follows the limit override and the motor within 0.1 s, not
    // at the next 0.5 s refresh (0.13.9, Ryan 28 Oct: it lagged the button).
    static int shown = -1;
    int now = (over_on ? 1 : 0) | (m.held2 ? 2 : 0) | (stepper3_override() ? 4 : 0) |
              ((stepper3_direction() + 1) << 3) | (stepper3_limit_open() ? 32 : 0);
    if (now != shown) {
        if (shown >= 0)
            ui_refresh_soon();
        shown = now;
    }

    lv_snprintf(t, sizeof t, "%ld", (long)stepper3_position());
    ui_set_text(m.pos, t);

    for (int i = 0; i < 6; i++)
        seg_show(m.size[i], i ? !hold_on && i - 1 == size_sel : hold_on, true);
    // Output turns from count 0, in the steps of the current mode.
    char tn[24];
    int64_t c100 = (int64_t)stepper3_position() * 100 / ((int64_t)STEPPER3_STEPS_PER_TURN * stepper3_scale());
    int64_t a100 = c100 < 0 ? -c100 : c100;
    lv_snprintf(tn, sizeof tn, "%s%ld.%02ld turns", stepper3_position() < 0 ? "-" : "", (long)(a100 / 100),
                (long)(a100 % 100));
    ui_set_text(m.turns, tn);
    for (int i = 0; i < 3; i++)
        seg_show(m.mode[i], (int)stepper3_mode() == i, !busy && !stepper3_locked());
    // Close is greyed while the limit stop reads open, unless the override
    // is on; Set zero and Limit override while the motor is locked. A
    // move does not grey the row (0.12.19, Ryan 27 Oct: the buttons seemed
    // to hide themselves); a press during a move does nothing.
    bool ov = stepper3_override() || over_on;
    ui_btn_enable(m.close, !stepper3_limit_open() || ov || m.held == m.close);
    ui_btn_enable(m.open, true);
    ui_btn_enable(m.zero, !stepper3_locked());
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
        for (uint32_t i = 0; i < lv_obj_get_child_count(m.over); i++)
            lv_obj_set_style_text_color(lv_obj_get_child(m.over, (int32_t)i),
                                        ov ? lv_color_white() : i ? UI_MUTED : UI_TEXT, 0);
    }

    lv_snprintf(t, sizeof t, "%lu sps", (unsigned long)stepper3_rate());
    ui_set_text(m.rate, t);
    lv_snprintf(t, sizeof t, "%lu sps", (unsigned long)stepper3_start_rate());
    ui_set_text(m.start, t);
    lv_snprintf(t, sizeof t, "%lu sps/s", (unsigned long)stepper3_accel());
    ui_set_text(m.accel, t);
    for (int i = 0; i < 6; i++)
        ui_btn_enable(m.adj[i], !busy);
    travel_show();

    uint8_t o[4];
    stepper3_order(o);
    lv_snprintf(t, sizeof t, "%d%d%d%d", o[0], o[1], o[2], o[3]);
    ui_btn_text(m.order, "Coil order", t);
    ui_btn_text(m.dir, "Direction", stepper3_reverse() ? "Reversed" : "Normal");
    ui_btn_text(m.hold, "Hold torque", stepper3_hold() ? "On" : "Off");
    ui_btn_enable(m.order, !busy && !stepper3_locked());
    ui_btn_enable(m.dir, !busy && !stepper3_locked());
    ui_btn_enable(m.hold, !busy);
}

// The strip: the motor state, then the limit switch (released or pressed,
// the words of Diagnostics; HMI review H7) and whether closing is allowed.
// A latched fault, an armed override or a message shows over it.
static void motor_refresh(void)
{
    char t[96];
    int8_t d = stepper3_direction();
    const char *st = d > 0 ? "Opening" : d < 0 ? "Closing"
                   : stepper3_energised() ? "Idle, coils on" : "Idle, coils off";
    limit_state_t lim = limit_state();
    const char *ls;
    bool warn = false;
    if (!stepper3_limit_fitted())
        ls = lim == LIMIT_CLOSED ? "Switch released, stop off" : lim == LIMIT_OPEN ? "Switch pressed, stop off"
                                                                                   : "Switch not read yet";
    else if (lim == LIMIT_CLOSED)
        ls = "Switch released, closing allowed";
    else {
        ls = lim == LIMIT_OPEN ? "Switch pressed, closing blocked" : "Switch not read yet, closing blocked";
        warn = true;
    }
    lv_snprintf(t, sizeof t, "%s. %s", st, ls);
    // Limit override held, nothing moving yet: what the second finger does.
    if (over_on && !m.held2) {
        lv_snprintf(t, sizeof t, "Limit override held: press Close or Open");
        warn = true;
    }
    // The status line's icons (arrows, valve symbol), without its sparkline.
    ui_strip_set(warn ? UI_STRIP_WARN : UI_STRIP_INFO, d > 0 ? LV_SYMBOL_UP : d < 0 ? LV_SYMBOL_DOWN : UI_ICON_VALVE, t);
    motor_fast();
}

const ui_screen_t ui_scr_motor = { "Motor override", motor_build, motor_refresh, motor_fast };
