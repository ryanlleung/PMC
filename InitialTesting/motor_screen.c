#include <string.h>

#include "lvgl.h"
#include "motor_screen.h"
#include "stepper3.h"
#include "link.h"

/*
 * Motor screen, 480 x 272. Every MOT setting and move from the touchscreen;
 * the MOT commands on the link still work and the screen follows them.
 *   y   0-30   header: Back, title, state (opening / closing / idle, coils)
 *   left       position card, step size (1 to 10000), Close / Open, STOP
 *   right      settings: mode, run rate, start rate, accel, order, direction,
 *              hold, zero, coils off
 *   y 256-270  limit switch warning while the input is not fitted
 * Moves are relative, by the selected step size, in steps of the current
 * mode (half steps in HALF). Settings are refused while moving, as on the link.
 * Buttons act on press, as the main screen tabs do. Each action is echoed on
 * the link as "MOT screen: ..." so a terminal log shows what was done.
 */
#define COL_HEADER   lv_color_make(31, 41, 51)
#define COL_BG       lv_color_make(232, 238, 244)
#define COL_CARD     lv_color_white()
#define COL_TEXT     lv_color_make(31, 41, 51)
#define COL_MUTED    lv_color_make(84, 99, 116)
#define COL_BTN      lv_color_make(235, 240, 245)
#define COL_BTN_DOWN lv_color_make(218, 225, 232)
#define COL_OK       lv_color_make(46, 160, 67)
#define COL_FAULT    lv_color_make(214, 48, 49)
#define COL_WARN     lv_color_make(214, 130, 0)
#define COL_IDLE     lv_color_make(150, 158, 166)

static lv_obj_t *screen, *back_to;
static lv_obj_t *state_label, *pos_label, *last_label;
static lv_obj_t *size_btn[5], *mode_btn[3];
static lv_obj_t *rate_val, *start_val, *accel_val;
static lv_obj_t *order_btn, *dir_btn, *hold_btn;
static lv_obj_t *move_btns[2];
static int size_sel = 2;

static const int32_t step_sizes[5] = { 1, 10, 100, 1000, 10000 };
static const char *const step_names[5] = { "1", "10", "100", "1k", "10k" };

// +/- walk these lists; a value set over the link snaps to the nearest entry.
static const uint32_t rates[] = { 16, 25, 50, 75, 100, 125, 150, 200, 250, 300, 400, 500, 600, 800, 1000 };
static const uint32_t accels[] = { 25, 50, 100, 200, 400, 800, 1600, 3200, 6400 };

// The three distinct coil orders for a four-phase unipolar motor; every other
// order is one of these turned or reversed.
static const uint8_t orders[3][4] = { { 0, 1, 2, 3 }, { 0, 2, 1, 3 }, { 0, 1, 3, 2 } };

/* Only touch an object when its value changes (see main_screen.c). */
static void set_text(lv_obj_t *l, const char *text)
{
    if (strcmp(lv_label_get_text(l), text) != 0)
        lv_label_set_text(l, text);
}

static void set_color(lv_obj_t *o, lv_color_t c)
{
    if (!lv_color_eq(lv_obj_get_style_text_color(o, LV_PART_MAIN), c))
        lv_obj_set_style_text_color(o, c, 0);
}

static void set_checked(lv_obj_t *o, bool on)
{
    if (on != lv_obj_has_state(o, LV_STATE_CHECKED)) {
        if (on) lv_obj_add_state(o, LV_STATE_CHECKED);
        else lv_obj_remove_state(o, LV_STATE_CHECKED);
    }
}

static void set_disabled(lv_obj_t *o, bool off)
{
    if (off != lv_obj_has_state(o, LV_STATE_DISABLED)) {
        if (off) lv_obj_add_state(o, LV_STATE_DISABLED);
        else lv_obj_remove_state(o, LV_STATE_DISABLED);
    }
}

static lv_obj_t *button_label(lv_obj_t *b)
{
    return lv_obj_get_child(b, 0);
}

static lv_obj_t *make_card(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, COL_CARD, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 6, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    return c;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, lv_color_t col)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, col, 0);
    return l;
}

static lv_obj_t *make_caption(lv_obj_t *parent, int32_t x, int32_t y, const char *text)
{
    lv_obj_t *l = make_label(parent, text, COL_MUTED);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

// Same look as the main screen tabs: grey fill, dark outline when checked.
static lv_obj_t *make_button(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h,
                             const char *text, lv_event_cb_t cb, intptr_t arg)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 4, 0);
    lv_obj_set_style_bg_color(b, COL_BTN, 0);
    lv_obj_set_style_text_color(b, COL_TEXT, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, COL_BTN, 0);
    lv_obj_set_style_border_color(b, COL_HEADER, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(b, COL_BTN_DOWN, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(b, COL_IDLE, LV_STATE_DISABLED);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_PRESSED, (void *)arg);
    return b;
}

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

static void refresh(void);

/* --------------------------------------------------------------------------
 * Events
 * ------------------------------------------------------------------------ */
static void back_event(lv_event_t *e)
{
    (void)e;
    lv_screen_load(back_to);
}

static void size_event(lv_event_t *e)
{
    size_sel = (int)event_arg(e);
    refresh();
}

static void move_event(lv_event_t *e)
{
    int32_t steps = step_sizes[size_sel] * (int32_t)event_arg(e);
    if (stepper3_move(steps))
        link_printf("MOT screen: move %ld\r\n", (long)steps);
    refresh();
}

static void stop_event(lv_event_t *e)
{
    (void)e;
    stepper3_stop();
    link_printf("MOT screen: stop at %ld\r\n", (long)stepper3_position());
    refresh();
}

static void mode_event(lv_event_t *e)
{
    stepper3_mode_t m = (stepper3_mode_t)event_arg(e);
    if (stepper3_set_mode(m))
        link_printf("MOT screen: mode %s\r\n", stepper3_mode_name(m));
    refresh();
}

enum { SET_RATE, SET_START, SET_ACCEL };

static void adjust_event(lv_event_t *e)
{
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
    refresh();
}

static void order_event(lv_event_t *e)
{
    (void)e;
    int i = (order_index() + 1) % 3;
    if (stepper3_set_order(orders[i]))
        link_printf("MOT screen: order %d%d%d%d\r\n",
                    orders[i][0], orders[i][1], orders[i][2], orders[i][3]);
    refresh();
}

static void dir_event(lv_event_t *e)
{
    (void)e;
    if (stepper3_set_reverse(!stepper3_reverse()))
        link_printf("MOT screen: dir %d\r\n", stepper3_reverse() ? 1 : 0);
    refresh();
}

static void hold_event(lv_event_t *e)
{
    (void)e;
    if (stepper3_set_hold(!stepper3_hold()))
        link_printf("MOT screen: hold %d\r\n", stepper3_hold() ? 1 : 0);
    refresh();
}

static void zero_event(lv_event_t *e)
{
    (void)e;
    if (!stepper3_busy()) {
        stepper3_zero();
        link_printf("MOT screen: zero\r\n");
    }
    refresh();
}

static void off_event(lv_event_t *e)
{
    (void)e;
    stepper3_off();
    link_printf("MOT screen: coils off at %ld\r\n", (long)stepper3_position());
    refresh();
}

/* --------------------------------------------------------------------------
 * Refresh: 10 times a second while the screen is shown, so the position
 * follows a move; unchanged values redraw nothing.
 * ------------------------------------------------------------------------ */
static void refresh(void)
{
    char t[48];
    bool busy = stepper3_busy();
    int8_t d = stepper3_direction();

    if (d > 0) {
        set_text(state_label, "Opening");
        set_color(state_label, COL_OK);
    } else if (d < 0) {
        set_text(state_label, "Closing");
        set_color(state_label, COL_WARN);
    } else {
        set_text(state_label, stepper3_energised() ? "Idle, coils on" : "Idle, coils off");
        set_color(state_label, COL_IDLE);
    }

    lv_snprintf(t, sizeof t, "%ld", (long)stepper3_position());
    set_text(pos_label, t);
    stepper3_stop_t s = stepper3_last_stop();
    lv_snprintf(t, sizeof t, "%s steps, last move: %s",
                stepper3_mode() == STEPPER3_HALF ? "half" : "full", stepper3_stop_name(s));
    set_text(last_label, t);
    set_color(last_label, s == STEPPER3_STOP_LIMIT ? COL_FAULT : COL_MUTED);

    for (int i = 0; i < 5; i++)
        set_checked(size_btn[i], i == size_sel);
    for (int i = 0; i < 2; i++)
        set_disabled(move_btns[i], busy);
    for (int i = 0; i < 3; i++) {
        set_checked(mode_btn[i], (int)stepper3_mode() == i);
        set_disabled(mode_btn[i], busy);
    }

    lv_snprintf(t, sizeof t, "%lu sps", (unsigned long)stepper3_rate());
    set_text(rate_val, t);
    lv_snprintf(t, sizeof t, "%lu sps", (unsigned long)stepper3_start_rate());
    set_text(start_val, t);
    lv_snprintf(t, sizeof t, "%lu sps/s", (unsigned long)stepper3_accel());
    set_text(accel_val, t);

    uint8_t o[4];
    stepper3_order(o);
    lv_snprintf(t, sizeof t, "Order %d%d%d%d", o[0], o[1], o[2], o[3]);
    set_text(button_label(order_btn), t);
    set_text(button_label(dir_btn), stepper3_reverse() ? "Dir reversed" : "Dir normal");
    set_text(button_label(hold_btn), stepper3_hold() ? "Hold on" : "Hold off");
    set_checked(hold_btn, stepper3_hold());
    set_checked(dir_btn, stepper3_reverse());
    set_disabled(order_btn, busy);
    set_disabled(dir_btn, busy);
    set_disabled(hold_btn, busy);
}

static void refresh_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (lv_screen_active() == screen)
        refresh();
}

/* --------------------------------------------------------------------------
 * Layout
 * ------------------------------------------------------------------------ */
// Caption, [-] value [+] on one settings row.
static lv_obj_t *make_adjust_row(lv_obj_t *parent, int32_t y, const char *caption, int which)
{
    make_caption(parent, 8, y + 9, caption);
    make_button(parent, 56, y, 36, 30, LV_SYMBOL_MINUS, adjust_event, which << 1);
    lv_obj_t *v = make_label(parent, "", COL_TEXT);
    lv_obj_set_width(v, 90);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(v, 93, y + 7);
    make_button(parent, 184, y, 36, 30, LV_SYMBOL_PLUS, adjust_event, (which << 1) | 1);
    return v;
}

void motor_screen_init(lv_obj_t *return_screen)
{
    back_to = return_screen;
    screen = lv_obj_create(NULL);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, COL_BG, 0);

    // Header.
    lv_obj_t *hdr = lv_obj_create(screen);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_size(hdr, 480, 30);
    lv_obj_set_style_bg_color(hdr, COL_HEADER, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_t *back = make_button(hdr, 0, 0, 80, 30, LV_SYMBOL_LEFT " Back", back_event, 0);
    lv_obj_set_style_radius(back, 0, 0);
    lv_obj_set_style_bg_color(back, COL_HEADER, 0);
    lv_obj_set_style_border_color(back, COL_HEADER, 0);
    lv_obj_set_style_bg_color(back, lv_color_make(60, 72, 86), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(back, lv_color_white(), 0);
    lv_obj_t *title = make_label(hdr, "Motor", lv_color_white());
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 92, 0);
    state_label = make_label(hdr, "", COL_IDLE);
    lv_obj_align(state_label, LV_ALIGN_RIGHT_MID, -10, 0);

    // Left: position and motion.
    lv_obj_t *pc = make_card(screen, 10, 38, 222, 80);
    make_caption(pc, 8, 4, "Position, steps (+ open)");
    pos_label = make_label(pc, "0", COL_TEXT);
    lv_obj_set_style_text_font(pos_label, &lv_font_montserrat_48, 0);
    lv_obj_set_width(pos_label, 206);
    lv_obj_set_style_text_align(pos_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(pos_label, 8, 12);
    last_label = make_caption(pc, 8, 62, "");
    // The 48 px label box overlaps the caption rows; keep them on top.
    lv_obj_move_foreground(last_label);

    lv_obj_t *mc = make_card(screen, 10, 124, 222, 128);
    make_caption(mc, 8, 2, "Step size");
    for (int i = 0; i < 5; i++)
        size_btn[i] = make_button(mc, 8 + i * 42, 18, 38, 30, step_names[i], size_event, i);
    move_btns[0] = make_button(mc, 8, 54, 101, 34, LV_SYMBOL_LEFT " Close", move_event, -1);
    move_btns[1] = make_button(mc, 113, 54, 101, 34, "Open " LV_SYMBOL_RIGHT, move_event, 1);
    lv_obj_t *stop = make_button(mc, 8, 92, 206, 32, "STOP", stop_event, 0);
    lv_obj_set_style_bg_color(stop, COL_FAULT, 0);
    lv_obj_set_style_border_color(stop, COL_FAULT, 0);
    lv_obj_set_style_bg_color(stop, lv_color_make(170, 30, 30), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(stop, lv_color_white(), 0);

    // Right: settings.
    lv_obj_t *sc = make_card(screen, 240, 38, 230, 214);
    make_caption(sc, 8, 9, "Mode");
    for (int i = 0; i < 3; i++)
        mode_btn[i] = make_button(sc, 56 + i * 56, 2, 52, 30,
                                  stepper3_mode_name((stepper3_mode_t)i), mode_event, i);
    rate_val = make_adjust_row(sc, 38, "Run", SET_RATE);
    start_val = make_adjust_row(sc, 74, "Start", SET_START);
    accel_val = make_adjust_row(sc, 110, "Accel", SET_ACCEL);
    order_btn = make_button(sc, 8, 146, 104, 30, "", order_event, 0);
    dir_btn = make_button(sc, 116, 146, 104, 30, "", dir_event, 0);
    hold_btn = make_button(sc, 8, 180, 68, 30, "", hold_event, 0);
    make_button(sc, 80, 180, 60, 30, "Zero", zero_event, 0);
    make_button(sc, 144, 180, 76, 30, "Coils off", off_event, 0);

    if (!stepper3_limit_fitted()) {
        lv_obj_t *w = make_label(screen, "Limit switch not fitted: closing moves are not stopped",
                                 COL_WARN);
        lv_obj_set_style_text_font(w, &lv_font_montserrat_12, 0);
        lv_obj_set_pos(w, 12, 256);
    }

    refresh();
    lv_timer_create(refresh_timer_cb, 100, NULL);
}

void motor_screen_show(void)
{
    refresh();
    lv_screen_load(screen);
}
