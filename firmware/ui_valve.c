#include <string.h>

#include "lvgl.h"
#include "ipc_config.h"
#include "clicks.h"
#include "limit.h"
#include "stepper3.h"
#include "valve.h"
#include "valvepos.h"
#include "link.h"
#include "ethlink.h"
#include "cfg.h"
#include "ui.h"
#include "fixed.h"

/* --------------------------------------------------------------------------
 * Operator screens: Events, Diagnostics. The closed-switch question is in
 * ui.c, on every screen; the valve position is on Valve > Position
 * (ui_settings.c).
 * ------------------------------------------------------------------------ */

static void reset_cb(lv_event_t *e)
{
    (void)e;
    char t[96];
    if (ui_reset_faults(t, sizeof t))
        ui_msg(t);
}

static void timer_deleted(lv_event_t *e)
{
    lv_timer_delete(lv_event_get_user_data(e));
}

static void right(lv_obj_t *l)
{
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
}

/* --------------------------------------------------------------------------
 * Events
 * ------------------------------------------------------------------------ */
#define LOG_ROWS 40
#define EV_ROWS (LOG_ROWS + 4)   // the latched faults (up to 3) over the events
static struct {
    lv_obj_t *reset, *box, *rows, *page[2];
    int nf;                   // faults shown
    char sig[64];             // their tags, to rebuild on change
    uint32_t log_total;
    int detail;               // fault shown in the detail, -1 none
    lv_obj_t *d_title, *d_when, *d_cap[4], *d_text[4], *d_next, *d_reset;
    char d_msg[96];           // what the detail's Reset did, in its Reset when row
    int pressed;              // the fault row under the finger, -1 none
} fp;

/* The list: one card, a row each, newest first, the
 * latched faults on top in red with a chevron (a tap opens the detail).
 * A row is the time and tag in 12 px over the text in 16 px, wrapped, with
 * a rule under it. Drawn on one object from these copies, not a label per
 * row, to keep the LVGL heap small. */
typedef struct {
    uint64_t at;             // valve_ms() when it happened
    int16_t y, h;
    int8_t fault;            // the fault's index, -1 an event
    char tag[8];
    char text[104];
} ev_row_t;
static ev_row_t ev_rows[EV_ROWS];
static int ev_n;
#define EV_TEXT_W 319
#define EV_FAULT_W 300       // room for the chevron
#define EV_TOP 3             // the time line from the row's top
#define EV_TEXT_Y 18         // the text from the row's top
#define EV_GAP 6             // under the text, then the rule

typedef struct {
    const char *tag, *happened, *controller, *check, *reset;
} fault_help_t;

static const fault_help_t fault_help[] = {
    // Three faults (valve.c). F-1 takes in the sensor supply; its row shows
    // the cause that latched it, this text when there is none.
    { "F-1", "No pressure sensor reading: missing, outside 0.005 to 11 mV/V, or the sensor supply under 7 V or tripped.",
      "AUTO stops. The valve remains in position and can still be moved.",
      "The pressure sensor cable and connector. A supply trip clears only after the power is switched off and on.",
      "After the reading returns." },
    { "F-2", "During homing the valve closed 1.1 times full travel without reaching the limit switch.",
      "Stopped. The valve is not homed.",
      "Before homing again: the limit switch and its wiring, and that the valve turns.",
      "Any time. Homing again closes up to 1.1 times full travel further." },
    { "F-3", "The limit switch stayed pressed after the valve moved off it, or was pressed during an opening move.",
      "Stopped. The valve is not homed.",
      "That the switch moves freely and its wiring is intact. On an opening move, check Direction (Motor > Settings). "
      "After homing: Back-off and Return on Valve > Homing.",
      "Any time. Then home the valve." },
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
    lv_snprintf(buf, sizeof buf, "%s, %s", ui_when(w, sizeof w, f.age_ms), mode_word(f.mode));
    ui_set_text(fp.d_when, buf);
    // The title in the room left of the time: at 16 px, or 12 px on the
    // time's line when it does not fit ("F-3  Limit pressed when it should
    // be free" is 72 px too long for its box).
    lv_point_t sw, st;
    lv_text_get_size(&sw, buf, &ui_font_12, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int32_t room = 270 + 178 - sw.x - 8 - 12;   // from 12 to 8 px before the time (right edge 270 + 178)
    lv_snprintf(buf, sizeof buf, "%s  %s", f.tag, f.text);
    lv_text_get_size(&st, buf, &ui_font_16, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const lv_font_t *tf = st.x <= room ? &ui_font_16 : &ui_font_12;
    if (lv_obj_get_style_width(fp.d_title, 0) != room)
        lv_obj_set_width(fp.d_title, room);
    if (lv_obj_get_style_text_font(fp.d_title, 0) != tf) {
        lv_obj_set_style_text_font(fp.d_title, tf, 0);
        lv_obj_set_y(fp.d_title, tf == &ui_font_16 ? 6 : 8);
    }
    ui_set_text(fp.d_title, buf);
    ui_set_text(fp.d_text[0], f.cause && f.cause[0] ? f.cause : h->happened);
    ui_set_text(fp.d_text[1], h->controller);
    ui_set_text(fp.d_text[2], h->check);
    // After a Reset that left a fault, what it did.
    if (fp.d_msg[0])
        lv_strlcpy(buf, fp.d_msg, sizeof buf);
    else if (strcmp(f.tag, "F-1") == 0)
        lv_snprintf(buf, sizeof buf, "%s%s", h->reset, f.can_reset ? " Possible now." : " Not possible yet.");
    else
        lv_strlcpy(buf, h->reset, sizeof buf);
    ui_set_text(fp.d_text[3], buf);
    // The rows under one another, each as high as its text.
    int32_t y = 30;
    for (int k = 0; k < 4; k++) {
        lv_obj_update_layout(fp.d_text[k]);
        if (lv_obj_get_y(fp.d_text[k]) != y) {
            lv_obj_set_y(fp.d_text[k], y);
            lv_obj_set_y(fp.d_cap[k], y);
        }
        y += lv_obj_get_height(fp.d_text[k]) + 4;
    }
    lv_snprintf(buf, sizeof buf, "%d of %d", fp.detail + 1, n);
    ui_btn_text(fp.d_next, "Next fault", buf);
    ui_btn_enable(fp.d_next, n > 1);
    ui_btn_text(fp.d_reset, n > 1 ? "Reset faults" : "Reset fault", NULL);
}

static void d_next_cb(lv_event_t *e)
{
    (void)e;
    int n = valve_fault_count();
    if (n > 1) {
        fp.detail = (fp.detail + 1) % n;
        fp.d_msg[0] = '\0';
        detail_fill();
    }
}

static void d_reset_cb(lv_event_t *e)
{
    (void)e;
    // What stays says so in the detail: "Not reset: the
    // cause is still present.", or with another fault reset "F-3 reset. F-1
    // not reset: cause still present".
    char t[96];
    if (ui_reset_faults(t, sizeof t)) {
        if (strstr(t, " reset. "))
            lv_snprintf(fp.d_msg, sizeof fp.d_msg, "%s.", t);
        else
            lv_strlcpy(fp.d_msg, "Not reset: the cause is still present.", sizeof fp.d_msg);
    }
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
    fp.d_msg[0] = '\0';
    lv_obj_add_event_cb(o, detail_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_t *c = ui_card(o, 10, 8, 460, 160);
    fp.d_title = ui_label(c, 12, 6, 258, &ui_font_16, UI_FAULT, LV_TEXT_ALIGN_LEFT, "");
    fp.d_when = ui_label(c, 270, 8, 178, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_RIGHT, "");
    static const char *const caps[4] = { "Cause", "Controller", "Check", "Reset when" };
    for (int k = 0; k < 4; k++) {
        fp.d_cap[k] = ui_label(c, 12, 30 + k * 32, 84, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT, caps[k]);
        fp.d_text[k] = ui_label(c, 98, 30 + k * 32, 350, &ui_font_12, UI_TEXT, LV_TEXT_ALIGN_LEFT, "");
        lv_label_set_long_mode(fp.d_text[k], LV_LABEL_LONG_WRAP);
    }
    fp.d_next = ui_btn(o, 10, 176, 109, 48, UI_SECONDARY, "Next fault", d_next_cb, NULL);
    fp.d_reset = ui_btn(o, 127, 176, 109, 48, UI_PRIMARY, "Reset fault", d_reset_cb, NULL);
    ui_btn(o, 244, 176, 109, 48, UI_SECONDARY, "Close", d_close_cb, NULL);
    detail_fill();
}

// From the strip on any screen: a tap on the fault text.
void ui_fault_detail(void)
{
    if (valve_fault_count())
        detail_open(0);
}

// A tap on a fault row opens its detail.
static void rows_click(lv_event_t *e)
{
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    lv_area_t a;
    lv_obj_get_coords(fp.rows, &a);
    int32_t y = p.y - a.y1;
    for (int i = 0; i < ev_n; i++)
        if (y >= ev_rows[i].y && y < ev_rows[i].y + ev_rows[i].h) {
            if (ev_rows[i].fault >= 0)
                detail_open(ev_rows[i].fault);
            return;
        }
    (void)e;
}

// A fault row darkens under the finger.
static void rows_press(lv_event_t *e)
{
    int was = fp.pressed;
    fp.pressed = -1;
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        lv_point_t p;
        lv_indev_get_point(lv_indev_active(), &p);
        lv_area_t a;
        lv_obj_get_coords(fp.rows, &a);
        int32_t y = p.y - a.y1;
        for (int i = 0; i < ev_n; i++)
            if (ev_rows[i].fault >= 0 && y >= ev_rows[i].y && y < ev_rows[i].y + ev_rows[i].h)
                fp.pressed = i;
    }
    if (was != fp.pressed)
        lv_obj_invalidate(fp.rows);
}

static void rows_draw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(fp.rows, &a);
    uint64_t now = valve_ms();
    for (int i = 0; i < ev_n; i++) {
        const ev_row_t *r = &ev_rows[i];
        int32_t y = a.y1 + r->y;
        char head[40], w[16];
        lv_snprintf(head, sizeof head, "%s   %s", ui_when(w, sizeof w, now - r->at), r->tag);
        if (i == fp.pressed) {
            lv_draw_rect_dsc_t pr;
            lv_draw_rect_dsc_init(&pr);
            pr.bg_color = UI_PRESS;
            lv_area_t pa = { a.x1 - 12, y, a.x1 + EV_TEXT_W + 11, y + r->h - 2 };
            lv_draw_rect(layer, &pr, &pa);
        }
        lv_draw_label_dsc_t d;
        lv_draw_label_dsc_init(&d);
        d.font = &ui_num_12;   // tabular digits: the tags line up
        d.color = UI_MUTED;
        d.text = head;
        d.text_local = 1;
        lv_area_t t = { a.x1, y + EV_TOP, a.x1 + EV_TEXT_W, y + EV_TOP + 15 };
        if (r->tag[0])
            lv_draw_label(layer, &d, &t);
        lv_draw_label_dsc_init(&d);
        d.font = &ui_font_16;
        d.color = r->fault >= 0 ? UI_FAULT : UI_TEXT;
        d.text = r->text;
        d.text_local = 1;
        int32_t tw = r->fault >= 0 ? EV_FAULT_W : EV_TEXT_W;
        lv_area_t x = { a.x1, y + EV_TEXT_Y, a.x1 + tw - 1, y + r->h - EV_GAP };
        lv_draw_label(layer, &d, &x);
        if (r->fault >= 0) {
            lv_draw_label_dsc_init(&d);
            d.font = &lv_font_montserrat_14;
            d.color = UI_CHEV;   // 12 px from the card's edge, as every chevron
            d.text = LV_SYMBOL_RIGHT;
            lv_area_t c = { a.x1 + EV_TEXT_W - 8, y + r->h / 2 - 8, a.x1 + EV_TEXT_W + 4, y + r->h / 2 + 8 };
            lv_draw_label(layer, &d, &c);
        }
        if (i + 1 < ev_n) {
            lv_draw_rect_dsc_t rd;
            lv_draw_rect_dsc_init(&rd);
            rd.bg_color = lv_color_hex(0xE3E9EF);
            lv_area_t l = { a.x1, y + r->h - 1, a.x1 + EV_TEXT_W - 1, y + r->h - 1 };
            lv_draw_rect(layer, &rd, &l);
        }
    }
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
    fp.pressed = -1;
    fp.nf = -1;
    ev_n = 0;
    lv_obj_t *card = ui_card(scr, 10, 48, 343, 160);
    lv_obj_t *box = lv_obj_create(card);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, 12, 2);
    lv_obj_set_size(box, 327, 156);
    lv_obj_set_scroll_dir(box, LV_DIR_VER);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_MOMENTUM |
                            LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(box, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(box, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(box, UI_IDLE, LV_PART_SCROLLBAR);
    fp.box = box;
    fp.rows = lv_obj_create(box);
    lv_obj_remove_style_all(fp.rows);
    lv_obj_set_pos(fp.rows, 0, 0);
    lv_obj_set_size(fp.rows, EV_TEXT_W, 10);
    lv_obj_add_flag(fp.rows, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(fp.rows, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_event_cb(fp.rows, rows_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(fp.rows, rows_click, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(fp.rows, rows_press, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(fp.rows, rows_press, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(fp.rows, rows_press, LV_EVENT_PRESS_LOST, NULL);
    fp.reset = ui_btn(scr, 361, 48, 109, 48, UI_PRIMARY, "Reset fault", faults_reset_cb, NULL);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = ui_btn(scr, 361, i ? 160 : 104, 109, 48, UI_SECONDARY, "", NULL, NULL);
        ui_btn_arrow(b, !i);
        lv_obj_add_event_cb(b, log_page, LV_EVENT_PRESSED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(b, log_page, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)i);
        fp.page[i] = b;
    }
    fp.log_total = (uint32_t)-1;
}

// One row's place and height: the text wrapped to its width.
static void row_add(int fault, uint64_t age, const char *tag, const char *text, int32_t *y)
{
    if (ev_n >= EV_ROWS)
        return;
    ev_row_t *r = &ev_rows[ev_n++];
    r->fault = (int8_t)fault;
    r->at = valve_ms() - age;
    lv_strlcpy(r->tag, tag, sizeof r->tag);
    lv_strlcpy(r->text, text, sizeof r->text);
    // Positions in % on the screen; the counts and the save slot stay in
    // the text port's EVENT? lines. Lines break between words, a number
    // with its unit.
    ui_plain(r->text);
    ui_mb(r->text);
    ui_dates(r->text);
    ui_wrap(r->text, &ui_font_16, fault >= 0 ? EV_FAULT_W : EV_TEXT_W);
    lv_point_t sz;
    lv_text_get_size(&sz, r->text, &ui_font_16, 0, 0, fault >= 0 ? EV_FAULT_W : EV_TEXT_W, LV_TEXT_FLAG_NONE);
    r->y = (int16_t)*y;
    r->h = (int16_t)(EV_TEXT_Y + sz.y + EV_GAP);
    *y += r->h;
}

// Rebuilt when a fault latches or clears or an event is logged.
static void faults_list(void)
{
    char sig[64] = "";
    valve_fault_t f;
    int n = valve_fault_count();
    for (int i = 0; i < n && valve_fault_get(i, &f); i++) {
        size_t len = strlen(sig);
        lv_snprintf(sig + len, sizeof sig - len, "%s ", f.tag);
    }
    uint32_t total = valve_event_total();
    if (n == fp.nf && strcmp(sig, fp.sig) == 0 && total == fp.log_total)
        return;
    fp.nf = n;
    fp.log_total = total;
    lv_strlcpy(fp.sig, sig, sizeof fp.sig);
    ev_n = 0;
    int32_t y = 0;
    for (int i = 0; i < n && valve_fault_get(i, &f); i++)
        row_add(i, f.age_ms, f.tag, f.text, &y);
    for (int i = 0; i < LOG_ROWS; i++) {
        uint64_t age;
        const char *tag, *text;
        if (!valve_event_get(i, &age, &tag, &text))
            break;
        // A setpoint from the network is two events, NET over SP: one row,
        // "Setpoint 3.0 mB (text port)".
        const char *sp = strcmp(tag, "NET") == 0 ? strstr(text, ": setpoint ") : NULL;
        uint64_t age2;
        const char *tag2, *text2;
        if (sp && valve_event_get(i + 1, &age2, &tag2, &text2) && strcmp(tag2, "SP") == 0
            && strncmp(text2, "Setpoint ", 9) == 0 && strcmp(text2 + 9, sp + 11) == 0) {
            char m[104];
            if (strncmp(text, "Text port", 9) == 0)
                lv_snprintf(m, sizeof m, "%s (text port)", text2);
            else
                lv_snprintf(m, sizeof m, "%s (%.*s)", text2, (int)(sp - text), text);
            row_add(-1, age, "SP", m, &y);
            i++;
            continue;
        }
        row_add(-1, age, tag, text, &y);
    }
    if (!ev_n) {
        row_add(-1, 0, "", "No events yet", &y);
    } else if (valve_event_total() > (uint32_t)valve_event_count() || valve_event_count() > LOG_ROWS) {
        row_add(-1, 0, "", "Older events are not kept", &y);
    }
    lv_obj_set_height(fp.rows, y > 10 ? y : 10);
    lv_obj_invalidate(fp.rows);
    log_buttons();
}

static void faults_refresh(void)
{
    faults_list();
    lv_obj_invalidate(fp.rows);   // the times move on
    ui_btn_enable(fp.reset, valve_fault_count() > 0);
    if (fp.detail >= 0)   // the detail overlay is open (detail_deleted)
        detail_fill();
    char t[48];
    int n = valve_fault_count();
    if (n)   // "1 fault latched. Tap it", "2 faults latched. Tap one"
        lv_snprintf(t, sizeof t, n == 1 ? "%d fault latched. Tap it for detail." : "%d faults latched. Tap one for detail.", n);
    else if (valve_warning() && valve_warning_tag()[0])
        // A warning is not a fault, and is up: W-2's checks are here, under
        // "No faults latched" and a tick.
        lv_snprintf(t, sizeof t, "No faults latched. %s shown on Home", valve_warning_tag());
    else
        lv_strlcpy(t, "No faults latched", sizeof t);
    ui_btn_text(fp.reset, n > 1 ? "Reset faults" : "Reset fault", NULL);
    ui_strip_set(UI_STRIP_INFO, n || valve_warning() ? LV_SYMBOL_WARNING : LV_SYMBOL_OK, t);
}

const ui_screen_t ui_scr_faults = { "Events", faults_build, faults_refresh, NULL };

/* --------------------------------------------------------------------------
 * Diagnostics
 * ------------------------------------------------------------------------ */
static struct {
    lv_obj_t *v[9];
} dp;
enum { D_SUP, D_OUT, D_P, D_STEPS, D_TURNS, D_LIMIT, D_BARO, D_TEMP, D_NET };

// A row card across the width: its name on the left, three readings on the
// page's columns 2 to 4, each a caption over a value.
static void diag_row(lv_obj_t *scr, int32_t y, const char *name, const char *const cap[3], int first)
{
    lv_obj_t *c = ui_card(scr, 10, y, 460, 48);
    // Three 116 px columns from x 100 (narrower is too narrow for
    // Network's "Connected").
    ui_label(c, 12, 14, 84, &ui_font_16, UI_TEXT, LV_TEXT_ALIGN_LEFT, name);
    for (int i = 0; i < 3; i++) {
        const int32_t x = 100 + i * 120, w = 116;
        ui_label(c, x, 7, w, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT, cap[i]);
        dp.v[first + i] = ui_label(c, x, 24, w, &ui_num_16, UI_TEXT, LV_TEXT_ALIGN_LEFT, "");
    }
}

// Motor row: position and limit switch, at 50 Hz so the switch shows as
// the hardware reads it (limit.c samples every 5 ms).
static void diag_motor(lv_timer_t *t)
{
    (void)t;
    char buf[16];
    int32_t p = stepper3_position();
    lv_snprintf(buf, sizeof buf, "%ld", (long)p);
    ui_set_text(dp.v[D_STEPS], buf);
    ui_set_text(dp.v[D_TURNS], fixed_fmt(buf, sizeof buf, fixed_round((int64_t)p * 1000 / cfg_steps_per_turn(), 1), 2));
    ui_set_text(dp.v[D_LIMIT], limit_open_now() ? "Pressed" : "Released");
}

static void diag_build(lv_obj_t *scr)
{
    // Three row cards, Sensor, Motor and Board. Network shows whether a
    // client is connected on the text port or Modbus TCP.
    static const char *const druck[3] = { "Supply, V", "Output, mV", "Pressure, mB" };
    static const char *const motor[3] = { "Position, steps", "Position, turns", "Limit switch" };
    static const char *const board[3] = { "Barometer, mB", "Temperature, \xC2\xB0" "C", "Network" };
    diag_row(scr, 48, "Sensor", druck, D_SUP);
    diag_row(scr, 104, "Motor", motor, D_STEPS);
    diag_row(scr, 160, "Board", board, D_BARO);
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
    ui_set_text(dp.v[D_OUT], sig ? buf : "----");
    lv_snprintf(buf, sizeof buf, "%ld.%02ld", (long)(s->bus_mv / 1000), (long)(s->bus_mv % 1000 / 10));
    ui_set_text(dp.v[D_SUP], sig ? buf : "----");
    // The pressure before calibration (the datasheet line). "----" without
    // a valid reading, as Home (0.0 with the sensor unplugged would read as
    // a good vacuum); Supply and Output stay live.
    bool rd = sig && s->reading_ok && !s->boost_tripped;
    ui_set_text(dp.v[D_P], rd ? ui_fmt1(buf, sizeof buf, (int64_t)s->r_ppb * valve_range_mmbar() / 10000000) : "----");
    ui_set_text(dp.v[D_BARO], s->baro_ok ? ui_fmt1(buf, sizeof buf, s->baro_mmbar) : "----");   // Pressure 21
    // The power monitor's own sensor: "----" while it does not answer, as
    // Supply and Output and Modbus (NaN), not the last value read.
    ui_set_text(dp.v[D_TEMP], sig ? ui_fmt1(buf, sizeof buf, s->die_mc) : "----");
    // No cable while the Ethernet link is down (ethlink.c drops the clients
    // too).
#if IPC_LINK_ETHERNET
    bool cable = ethlink_cable();
    bool net = ethlink_client_connected() || ethlink_modbus_clients() > 0;
#else
    bool cable = true, net = false;
#endif
    ui_set_text(dp.v[D_NET], !cable ? "No cable" : net ? "Connected" : "No client");
    ui_status_line();
}

const ui_screen_t ui_scr_diag = { "Diagnostics", diag_build, diag_refresh, NULL };
