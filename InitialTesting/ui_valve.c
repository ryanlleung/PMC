#include <string.h>

#include "lvgl.h"
#include "pmc_config.h"
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
 * Operator screens (stage 5, simulator v63): Events, Diagnostics. The
 * closed-switch question is in ui.c, on every screen. Valve position went
 * to Valve > Position in 0.17.0 (ui_settings.c).
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
    // Three faults from 0.16.33 (valve.c). F-1 takes in the excitation
    // (F-2 to 0.16.32); the event log names which cause it was.
    { "F-1", "No Druck reading: missing, outside 0.005-11 mV/V, or the excitation under 7 V or tripped.",
      "Pressure shows ----. The valve still moves.",
      "The Druck cable and plug, the Power Monitor and the Boost 10. A trip needs a power cycle.",
      "The reading is back." },
    { "F-2", "Homing, the valve closed 1.1 x full travel without the limit switch.",
      "Stopped. The valve is not homed.",
      "The switch and its wire, and that the valve turns.",
      "Any time. Then home again." },
    { "F-3", "The limit switch stayed pressed after the valve moved off it, or pressed during an opening move.",
      "Stopped. The valve is not homed.",
      "The switch is not stuck (ice), its wire is not broken. On an opening move: Direction and coil order.",
      "Any time. Then home again." },
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
    lv_obj_t *v[9];
} dp;
enum { D_SUP, D_OUT, D_P, D_STEPS, D_TURNS, D_LIMIT, D_BARO, D_TEMP, D_NET };

// A row card across the width (0.17.0): its name on the left, three
// readings at x 100, 220 and 340, each a caption over a value.
static void diag_row(lv_obj_t *scr, int32_t y, const char *name, const char *const cap[3], int first)
{
    lv_obj_t *c = ui_card(scr, 10, y, 460, 48);
    ui_label(c, 12, 14, 84, &ui_font_16, UI_TEXT, LV_TEXT_ALIGN_LEFT, name);
    for (int i = 0; i < 3; i++) {
        ui_label(c, 100 + i * 120, 7, 116, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT, cap[i]);
        dp.v[first + i] = ui_label(c, 100 + i * 120, 24, 116, &ui_num_16, UI_TEXT, LV_TEXT_ALIGN_LEFT, "");
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
    // 0.17.0 (canvas "PMC Screens 0.17"): three row cards, Druck, Motor
    // and Board, no Help. Last move went (Ryan 14 Nov: "useless"); Network
    // shows whether a client is connected on the text port or Modbus TCP.
    static const char *const druck[3] = { "Supply, V", "Output, mV", "Pressure, mB" };
    static const char *const motor[3] = { "Position, steps", "Position, turns", "Limit switch" };
    static const char *const board[3] = { "Barometer, mB", "Temperature, \xC2\xB0" "C", "Network" };
    diag_row(scr, 48, "Druck", druck, D_SUP);
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
    // The pressure before calibration (the datasheet line), as Raw to 0.16.33.
    ui_set_text(dp.v[D_P], sig ? ui_fmt1(buf, sizeof buf, (int64_t)s->r_ppb * valve_range_mmbar() / 10000000) : "----");
    ui_set_text(dp.v[D_BARO], s->baro_ok ? ui_fmt1(buf, sizeof buf, s->baro_mmbar) : "----");   // Pressure 21
    ui_set_text(dp.v[D_TEMP], ui_fmt1(buf, sizeof buf, s->die_mc));
#if PMC_LINK_ETHERNET
    bool net = ethlink_client_connected() || ethlink_modbus_clients() > 0;
#else
    bool net = false;
#endif
    ui_set_text(dp.v[D_NET], net ? "Connected" : "No client");
    ui_status_line();
}

const ui_screen_t ui_scr_diag = { "Diagnostics", diag_build, diag_refresh, NULL };
