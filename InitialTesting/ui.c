#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "pmc_config.h"
#include "rtclock.h"
#include "stepper3.h"
#include "valve.h"
#include "valvepos.h"
#include "motor_screen.h"
#include "fixed.h"
#include "cfg.h"
#include "trend.h"
#include "ui.h"

/* --------------------------------------------------------------------------
 * Fonts: tabular digits. Every digit takes the width of the widest, so a
 * right-aligned reading does not shift as it changes.
 * ------------------------------------------------------------------------ */
lv_font_t ui_num_16, ui_num_26, ui_num_40;

static bool num_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *dsc, uint32_t letter, uint32_t next)
{
    const lv_font_t *base = font->user_data;
    if (!base->get_glyph_dsc(base, dsc, letter, next))
        return false;
    if (letter >= '0' && letter <= '9') {
        int32_t widest = 0;
        lv_font_glyph_dsc_t d;
        for (uint32_t c = '0'; c <= '9'; c++)
            if (base->get_glyph_dsc(base, &d, c, 0) && d.adv_w > widest)
                widest = d.adv_w;
        dsc->ofs_x += (widest - dsc->adv_w) / 2;
        dsc->adv_w = widest;
    }
    return true;
}

static void num_font(lv_font_t *f, const lv_font_t *base)
{
    *f = *base;
    f->get_glyph_dsc = num_glyph_dsc;
    f->kerning = LV_FONT_KERNING_NONE;
    f->user_data = (void *)base;
}

/* --------------------------------------------------------------------------
 * Styles (STYLE-GUIDE.md section 6)
 * ------------------------------------------------------------------------ */
#define UI_EDGE    lv_color_hex(0x202020)
#define UI_PRESS   lv_color_hex(0xC0C0C0)
#define UI_DIS_BG  lv_color_hex(0xE0E0E0)
#define UI_DIS_TX  lv_color_hex(0x808080)
#define UI_CHEV    lv_color_hex(0x808080)

static lv_style_t st_btn, st_btn_pr, st_btn_dis, st_act, st_act_pr, st_act_small,
                  st_red, st_red_pr, st_dark, st_dark_pr, st_card, st_stop_op, st_stop_op_pr;
#define ST_DIS LV_STATE_USER_1   // greyed, still clickable so it can say why

static void styles_init(void)
{
    lv_style_init(&st_btn);
    lv_style_set_bg_opa(&st_btn, LV_OPA_COVER);
    lv_style_set_bg_color(&st_btn, UI_CARD);
    // Pokemon menu box: rounded pixel corners and a double black frame.
    lv_style_set_radius(&st_btn, 4);
    lv_style_set_border_side(&st_btn, LV_BORDER_SIDE_FULL);
    lv_style_set_border_width(&st_btn, 2);
    lv_style_set_border_color(&st_btn, UI_TEXT);
    lv_style_set_outline_width(&st_btn, 1);
    lv_style_set_outline_pad(&st_btn, 2);
    lv_style_set_outline_color(&st_btn, UI_TEXT);
    lv_style_set_border_color(&st_btn, UI_EDGE);
    lv_style_set_text_color(&st_btn, UI_TEXT);
    lv_style_set_text_font(&st_btn, &ui_font_16);

    lv_style_init(&st_btn_pr);
    lv_style_set_bg_color(&st_btn_pr, UI_PRESS);

    lv_style_init(&st_btn_dis);
    lv_style_set_bg_color(&st_btn_dis, UI_DIS_BG);
    lv_style_set_text_color(&st_btn_dis, UI_DIS_TX);
    lv_style_set_outline_color(&st_btn_dis, UI_DIS_TX);
    lv_style_set_border_color(&st_btn_dis, UI_DIS_TX);

    lv_style_init(&st_act);
    lv_style_set_bg_color(&st_act, UI_BLUE);
    lv_style_set_text_color(&st_act, lv_color_white());
    lv_style_init(&st_act_pr);
    lv_style_set_bg_color(&st_act_pr, lv_color_hex(0x204890));
    lv_style_init(&st_act_small);
    lv_style_set_text_color(&st_act_small, lv_color_hex(0xD0E0F8));

    lv_style_init(&st_red);
    lv_style_set_bg_color(&st_red, UI_FAULT);
    lv_style_set_text_color(&st_red, lv_color_white());
    lv_style_init(&st_red_pr);
    lv_style_set_bg_color(&st_red_pr, lv_color_hex(0xA02818));

    // STOP outside AUTO (no latch): white with a red edge and red word.
    lv_style_init(&st_stop_op);
    lv_style_set_bg_color(&st_stop_op, UI_CARD);
    lv_style_set_text_color(&st_stop_op, UI_FAULT);
    lv_style_set_border_side(&st_stop_op, LV_BORDER_SIDE_FULL);
    lv_style_set_border_width(&st_stop_op, 3);
    lv_style_set_border_color(&st_stop_op, UI_FAULT);
    lv_style_set_pad_all(&st_stop_op, -3);      // the word keeps its place
    lv_style_init(&st_stop_op_pr);
    lv_style_set_bg_color(&st_stop_op_pr, lv_color_hex(0xF8D0C8));

    lv_style_init(&st_dark);
    lv_style_set_bg_opa(&st_dark, LV_OPA_TRANSP);   // the header's rule shows under Back and Home
    lv_style_set_bg_color(&st_dark, UI_BG);
    lv_style_set_text_color(&st_dark, UI_TEXT);
    lv_style_set_radius(&st_dark, 0);
    lv_style_set_border_width(&st_dark, 0);
    lv_style_set_outline_width(&st_dark, 0);
    lv_style_init(&st_dark_pr);
    lv_style_set_bg_opa(&st_dark_pr, LV_OPA_COVER);
    lv_style_set_bg_color(&st_dark_pr, lv_color_hex(0xC0C0C0));

    lv_style_init(&st_card);
    lv_style_set_bg_opa(&st_card, LV_OPA_COVER);
    lv_style_set_bg_color(&st_card, UI_CARD);
    lv_style_set_radius(&st_card, 0);
    // Pokemon battle HUD bracket: black rule on the left and bottom. An
    // alarm turns it red and full (card_edge); pad keeps children in place.
    lv_style_set_border_side(&st_card, LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_BOTTOM);
    lv_style_set_border_color(&st_card, UI_TEXT);
    lv_style_set_border_width(&st_card, 3);
    lv_style_set_pad_left(&st_card, -3);
    lv_style_set_pad_bottom(&st_card, -3);
}

void ui_card_alarm(lv_obj_t *o, bool on)
{
    lv_border_side_t side = on ? LV_BORDER_SIDE_FULL : LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_BOTTOM;
    if (lv_obj_get_style_border_side(o, 0) == side && lv_obj_get_style_border_width(o, 0) == 3)
        return;
    lv_obj_set_style_border_width(o, 3, 0);
    lv_obj_set_style_border_side(o, side, 0);
    lv_obj_set_style_border_color(o, on ? UI_FAULT : UI_TEXT, 0);
    lv_obj_set_style_pad_all(o, -3, 0);         // children keep their place
    lv_obj_set_style_pad_top(o, on ? -3 : 0, 0);
    lv_obj_set_style_pad_right(o, on ? -3 : 0, 0);
}

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

lv_obj_t *ui_card(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *o = plain(parent);
    lv_obj_add_style(o, &st_card, 0);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    return o;
}

lv_obj_t *ui_label(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, const lv_font_t *font,
                   lv_color_t color, lv_text_align_t align, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_pos(l, x, y);
    if (w > 0) {
        lv_obj_set_width(l, w);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    }
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_align(l, align, 0);
    lv_label_set_text(l, text);
    return l;
}

void ui_set_text(lv_obj_t *l, const char *text)
{
    if (strcmp(lv_label_get_text(l), text) != 0)
        lv_label_set_text(l, text);
}

void ui_set_color(lv_obj_t *o, lv_color_t c)
{
    if (!lv_color_eq(lv_obj_get_style_text_color(o, LV_PART_MAIN), c))
        lv_obj_set_style_text_color(o, c, 0);
}

lv_obj_t *ui_info(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, const char *caption)
{
    lv_obj_t *c = ui_card(parent, x, y, w, h);
    ui_label(c, 10, 5, w - 20, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT, caption);
    return ui_label(c, 10, 22, w - 20, &ui_num_16, UI_TEXT, LV_TEXT_ALIGN_LEFT, "");
}

const char *ui_date(char *buf, size_t n, const char *iso)
{
    // "YYYY-MM-DD HH:MM:SS" to "16/10/26 14:32" (DD/MM/YY).
    lv_snprintf(buf, n, "%.2s/%.2s/%.2s %.5s", iso + 8, iso + 5, iso + 2, iso + 11);
    return buf;
}

const char *ui_when(char *buf, size_t n, uint32_t age_ms)
{
    const char *iso = rtclock_iso();
    if (iso[0]) {
        int32_t tod = ((iso[11] - '0') * 10 + iso[12] - '0') * 3600 + ((iso[14] - '0') * 10 + iso[15] - '0') * 60
                      + (iso[17] - '0') * 10 + iso[18] - '0';
        // Before today's midnight: the days back first, "-1d 23:15:02"
        // (review of 0.10.11: yesterday's event read as today's).
        int32_t age = (int32_t)(age_ms / 1000);
        int32_t days = age > tod ? (age - tod + 86399) / 86400 : 0;
        tod = (int32_t)((tod - age % 86400 + 86400) % 86400);
        if (days)
            lv_snprintf(buf, n, "-%ldd %02ld:%02ld:%02ld", (long)days, (long)(tod / 3600), (long)(tod / 60 % 60),
                        (long)(tod % 60));
        else
            lv_snprintf(buf, n, "%02ld:%02ld:%02ld", (long)(tod / 3600), (long)(tod / 60 % 60), (long)(tod % 60));
    } else {
        uint32_t t = (lv_tick_get() - age_ms) / 1000;
        lv_snprintf(buf, n, "up %lu:%02lu:%02lu", (unsigned long)(t / 3600), (unsigned long)(t / 60 % 60),
                    (unsigned long)(t % 60));
    }
    return buf;
}

const char *ui_fmt2(char *buf, size_t n, int64_t milli)
{
    bool neg = milli < 0;
    int64_t t = ((neg ? -milli : milli) + 5) / 10;
    lv_snprintf(buf, n, "%s%ld.%02ld", neg && t ? "-" : "", (long)(t / 100), (long)(t % 100));
    return buf;
}

const char *ui_fmt1(char *buf, size_t n, int64_t milli)
{
    bool neg = milli < 0;
    int64_t t = ((neg ? -milli : milli) + 50) / 100;
    lv_snprintf(buf, n, "%s%ld.%ld", neg && t ? "-" : "", (long)(t / 10), (long)(t % 10));
    return buf;
}

int32_t ui_pct_shown(int32_t milli)
{
    int32_t c = milli < 0 ? 0 : (milli + 5) / 10;
    return c > 10000 ? 10000 : c;
}

/* --------------------------------------------------------------------------
 * Buttons. Child 0 is the main label, then the chevron (nav only), then the
 * 12 px state line, made on first use (most buttons never have one).
 * ------------------------------------------------------------------------ */
#define BTN_NAV   LV_OBJ_FLAG_USER_1    // on the button
#define BTN_SMALL LV_OBJ_FLAG_USER_1    // on its state line

static lv_obj_t *btn_small(lv_obj_t *b)
{
    uint32_t n = lv_obj_get_child_count(b);
    lv_obj_t *last = lv_obj_get_child(b, (int32_t)n - 1);
    return n > 1 && lv_obj_has_flag(last, BTN_SMALL) ? last : NULL;
}

static void btn_layout(lv_obj_t *b)
{
    lv_obj_t *main = lv_obj_get_child(b, 0), *small = btn_small(b);
    bool two = small && !lv_obj_has_flag(small, LV_OBJ_FLAG_HIDDEN);
    bool nav = lv_obj_has_flag(b, BTN_NAV);
    lv_align_t al = nav ? LV_ALIGN_LEFT_MID : LV_ALIGN_CENTER;
    int32_t x = nav ? 12 : 0;
    lv_obj_align(main, al, x, two ? -8 : 0);
    if (small)
        lv_obj_align(small, al, x, 10);
}

lv_obj_t *ui_btn(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h,
                 ui_btn_kind_t kind, const char *text, lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_add_style(b, &st_btn, 0);
    lv_obj_add_style(b, &st_btn_pr, LV_STATE_PRESSED);
    if (kind == UI_PRIMARY) {
        lv_obj_add_style(b, &st_act, 0);
        lv_obj_add_style(b, &st_act_pr, LV_STATE_PRESSED);
    } else if (kind == UI_RED) {
        lv_obj_add_style(b, &st_red, 0);
        lv_obj_add_style(b, &st_red_pr, LV_STATE_PRESSED);
    } else if (kind == UI_DARK) {
        lv_obj_add_style(b, &st_dark, 0);
        lv_obj_add_style(b, &st_dark_pr, LV_STATE_PRESSED);
    }
    lv_obj_add_style(b, &st_btn_dis, ST_DIS);
    if (kind == UI_NAV)
        lv_obj_add_flag(b, BTN_NAV);
    if (kind == UI_PRIMARY)
        lv_obj_add_flag(b, LV_OBJ_FLAG_USER_2);   // its state line is light

    lv_obj_t *main = lv_label_create(b);
    lv_obj_set_width(main, w - (kind == UI_NAV ? 36 : 8));
    lv_obj_set_style_text_align(main, kind == UI_NAV || kind == UI_DARK ? LV_TEXT_ALIGN_LEFT
                                                                         : LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(main, text);
    if (kind == UI_NAV) {
        lv_obj_t *chev = lv_label_create(b);
        lv_obj_set_style_text_font(chev, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(chev, UI_TEXT, 0);
        lv_label_set_text(chev, LV_SYMBOL_PLAY);   // the Pokemon menu cursor
        lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -8, 0);
    }
    btn_layout(b);
    if (cb)
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    return b;
}

void ui_btn_text(lv_obj_t *b, const char *text, const char *small)
{
    lv_obj_t *m = lv_obj_get_child(b, 0), *s = btn_small(b);
    bool has = small && *small;
    ui_set_text(m, text);
    if (!s && !has)
        return;
    if (!s) {
        s = lv_label_create(b);
        lv_obj_add_flag(s, BTN_SMALL | LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(s, lv_obj_get_style_width(m, 0));
        lv_obj_set_style_text_align(s, lv_obj_get_style_text_align(m, 0), 0);
        lv_obj_set_style_text_font(s, &ui_font_12, 0);
        lv_obj_set_style_text_color(s, UI_MUTED, 0);
        if (lv_obj_has_flag(b, LV_OBJ_FLAG_USER_2))
            lv_obj_add_style(s, &st_act_small, 0);
    }
    bool had = !lv_obj_has_flag(s, LV_OBJ_FLAG_HIDDEN);
    ui_set_text(s, has ? small : "");
    if (had != has) {
        if (has) lv_obj_remove_flag(s, LV_OBJ_FLAG_HIDDEN);
        else     lv_obj_add_flag(s, LV_OBJ_FLAG_HIDDEN);
        btn_layout(b);
    }
}

void ui_btn_enable(lv_obj_t *b, bool on)
{
    if (on == !lv_obj_has_state(b, ST_DIS))
        return;
    if (on) lv_obj_remove_state(b, ST_DIS);
    else    lv_obj_add_state(b, ST_DIS);
}

bool ui_btn_disabled(lv_event_t *e)
{
    return lv_obj_has_state(lv_event_get_current_target(e), ST_DIS);
}

/* --------------------------------------------------------------------------
 * Frame on lv_layer_top(): header, status strip, STOP
 * ------------------------------------------------------------------------ */
#define HDR_BTN_W 48  // Back and Home, icons only
static lv_obj_t *frame, *hdr, *back_btn, *home_btn, *pmc_lbl, *title_lbl, *mode_lbl, *mode_dot, *fault_rule;
static lv_obj_t *strip, *strip_dot, *strip_icon, *strip_text, *strip_ok, *strip_act, *strip_spark;
static lv_obj_t *stop_btn;

static lv_obj_t *home_scr, *cur_scr, *overlay;
// Screens to go Back to (Home is under them all), e.g. Menu under Faults and log.
static const ui_screen_t *back_stack[4];
static int back_n;
static bool overlay_tv;         // the overlay is a text page (Help): strip shown greyed
static int overlay_q;           // the overlay is a question: 1 closed switch, 2 saved position
static bool kp_open;            // the overlay is the keypad
static void kp_side(void);
static void question_check(void);
static lv_obj_t *overlay_new(void);
static const ui_screen_t *cur = &ui_scr_home;
static bool service;

static char msg_text[96];
static uint32_t msg_until;

static ui_strip_kind_t line_kind;
static lv_color_t line_dot;
static char line_text[96];
static lv_event_cb_t act_cb;
static int32_t strip_w = 343;

static void open_screen(const ui_screen_t *s);

static void frame_refresh(void);

static void back_event(lv_event_t *e)
{
    (void)e;
    ui_back();
}

void ui_back(void)
{
    if (overlay)
        ui_overlay_close();
    else if (back_n)
        open_screen(back_stack[--back_n]);
    else if (cur != &ui_scr_home)
        ui_home();
}

static void home_event(lv_event_t *e)
{
    (void)e;
    ui_home();
}

static void stop_event(lv_event_t *e)
{
    (void)e;
    ui_overlay_close();
    valve_stop();
    frame_refresh();
}

static void ok_event(lv_event_t *e)
{
    (void)e;
    msg_until = 0;
    frame_refresh();   // clear now, not at the next 0.5 s refresh
}

static void act_event(lv_event_t *e)
{
    if (act_cb)
        act_cb(e);
}

static void build_frame(void)
{
    frame = plain(lv_layer_top());
    lv_obj_set_size(frame, 480, 272);
    lv_obj_add_flag(frame, LV_OBJ_FLAG_EVENT_BUBBLE);

    hdr = plain(frame);
    lv_obj_set_size(hdr, 480, 40);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(hdr, UI_BG, 0);
    lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(hdr, 2, 0);
    lv_obj_set_style_border_color(hdr, UI_TEXT, 0);
    lv_obj_add_flag(hdr, LV_OBJ_FLAG_CLICKABLE);    // touches on the header stop here
    fault_rule = plain(hdr);
    lv_obj_set_pos(fault_rule, 0, 36);
    lv_obj_set_size(fault_rule, 480, 4);
    lv_obj_set_style_bg_opa(fault_rule, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(fault_rule, UI_FAULT, 0);
    lv_obj_add_flag(fault_rule, LV_OBJ_FLAG_HIDDEN);
    // Back and Home, icons only, 48 px each; the title is centred (Ryan,
    // 16 Oct). Home is hidden on Home and under popups, where Back closes
    // the popup.
    back_btn = ui_btn(hdr, 0, 0, HDR_BTN_W, 40, UI_DARK, LV_SYMBOL_LEFT, back_event, NULL);
    home_btn = ui_btn(hdr, HDR_BTN_W, 0, HDR_BTN_W, 40, UI_DARK, LV_SYMBOL_HOME, home_event, NULL);
    // A touch in the 8 px gap under the header is Back's, not the page's.
    lv_obj_set_ext_click_area(hdr, 8);
    lv_obj_set_ext_click_area(back_btn, 8);
    lv_obj_set_ext_click_area(home_btn, 8);
    // The fault rule runs the full width, over Back and Home (not clickable,
    // so their touches still reach them).
    lv_obj_move_foreground(fault_rule);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *l = lv_obj_get_child(i ? home_btn : back_btn, 0);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(l);
    }
    pmc_lbl = ui_label(hdr, 10, 11, 280, &ui_font_16, UI_TEXT, LV_TEXT_ALIGN_CENTER,
                       "Pressure controller");
    lv_label_set_long_mode(pmc_lbl, LV_LABEL_LONG_DOT);
    title_lbl = ui_label(hdr, 100, 11, 280, &ui_font_16, UI_TEXT, LV_TEXT_ALIGN_CENTER, "");
    lv_label_set_long_mode(title_lbl, LV_LABEL_LONG_DOT);
    mode_lbl = ui_label(hdr, 296, 11, 158, &ui_font_16, UI_TEXT, LV_TEXT_ALIGN_RIGHT, "");
    mode_dot = plain(hdr);
    lv_obj_set_pos(mode_dot, 460, 15);
    lv_obj_set_size(mode_dot, 10, 10);
    lv_obj_set_style_radius(mode_dot, 0, 0);
    lv_obj_set_style_border_color(mode_dot, lv_color_hex(0xC0C0C0), 0);
    lv_obj_set_style_bg_color(mode_dot, UI_FAULT, 0);

    strip = plain(frame);
    lv_obj_set_pos(strip, 10, 216);
    lv_obj_set_size(strip, 343, 48);
    lv_obj_add_style(strip, &st_card, 0);
    lv_obj_add_flag(strip, LV_OBJ_FLAG_CLICKABLE);
    strip_dot = plain(strip);
    lv_obj_set_pos(strip_dot, 12, 20);
    lv_obj_set_size(strip_dot, 8, 8);
    lv_obj_set_style_radius(strip_dot, 0, 0);
    lv_obj_set_style_bg_opa(strip_dot, LV_OPA_COVER, 0);
    strip_icon = ui_label(strip, 6, 15, 0, &lv_font_montserrat_14, lv_color_white(), LV_TEXT_ALIGN_LEFT,
                          LV_SYMBOL_WARNING);
    strip_text = ui_label(strip, 28, 0, 303, &ui_font_12, UI_TEXT, LV_TEXT_ALIGN_LEFT, "");
    lv_label_set_long_mode(strip_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(strip_text, 4, 0);
    // The pressure sparkline on the right whenever the strip shows the
    // status line (Home and the operator screens; Ryan, 21 Oct).
    strip_spark = ui_spark(strip, 192, 6, 139, 36, false);
    lv_obj_add_flag(strip_spark, LV_OBJ_FLAG_HIDDEN);
    strip_ok = ui_btn(strip, 279, 4, 60, 40, UI_SECONDARY, "OK", ok_event, NULL);
    strip_act = ui_btn(strip, 239, 4, 100, 40, UI_PRIMARY, "", act_event, NULL);

    stop_btn = ui_btn(frame, 361, 216, 109, 48, UI_RED, "STOP", NULL, NULL);
    lv_obj_set_style_text_font(lv_obj_get_child(stop_btn, 0), &ui_font_26, 0);
    lv_obj_add_event_cb(stop_btn, stop_event, LV_EVENT_PRESSED, NULL);   // on press, not release
    lv_obj_add_style(stop_btn, &st_stop_op, LV_STATE_USER_2);
    lv_obj_add_style(stop_btn, &st_stop_op_pr, LV_STATE_USER_2 | LV_STATE_PRESSED);
}

static void show(lv_obj_t *o, bool on)
{
    if (on == !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN))
        return;
    if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

void ui_msg(const char *text)
{
    lv_strlcpy(msg_text, text, sizeof msg_text);
    // With a fault latched the message is brief: the fault text is the one to read.
    msg_until = lv_tick_get() + (valve_fault_count() ? 2000 : 5000);
    if (!msg_until)
        msg_until = 1;
}

void ui_strip_set(ui_strip_kind_t kind, lv_color_t dot, const char *text)
{
    line_kind = kind;
    line_dot = dot;
    lv_strlcpy(line_text, text, sizeof line_text);
}

void ui_strip_width(int32_t w) { strip_w = w; }

void ui_strip_action(const char *text, const char *small, lv_event_cb_t cb)
{
    act_cb = text ? cb : NULL;
    if (text)
        ui_btn_text(strip_act, text, small);
}

static void strip_draw(ui_strip_kind_t kind, lv_color_t dot, const char *text, bool ok, bool act)
{
    static const uint32_t bg[] = { 0xFFFFFF, 0xC0C0C0, 0x505050, 0x202020 };
    // Under a popup: one fixed grey whatever the kind or what is behind it
    // (opacity let the dim of a pick list or confirm tint it blue).
    bool dim = overlay != NULL;
    lv_color_t dis_bg = UI_DIS_BG, dis_tx = UI_DIS_TX;
    if (dim && lv_obj_get_style_bg_opa(overlay, 0) < LV_OPA_COVER) {
        // A pick list or confirm dims the page; the strip is above that
        // layer, so it takes the same dim to match the greyed buttons under it.
        lv_color_t sh = lv_obj_get_style_bg_color(overlay, 0);
        lv_opa_t o = lv_obj_get_style_bg_opa(overlay, 0);
        dis_bg = lv_color_mix(sh, dis_bg, o);
        dis_tx = lv_color_mix(sh, dis_tx, o);
    }
    lv_color_t b = dim ? dis_bg : lv_color_hex(bg[kind]);   // as a greyed button
    if (!lv_color_eq(lv_obj_get_style_bg_color(strip, 0), b))
        lv_obj_set_style_bg_color(strip, b, 0);
    ui_card_alarm(strip, !dim && kind == UI_STRIP_FAULT);   // HUD bracket, red frame on a fault
    show(strip_dot, kind == UI_STRIP_INFO);
    bool ring = false, is_mode = lv_color_eq(dot, UI_DOT_MODE);
    if (is_mode) {
        ring = !valve_auto();
        dot = lv_color_hex(0xC0C0C0);
    }
    if (dim)
        dot = dis_tx;
    if (kind == UI_STRIP_INFO && (!lv_color_eq(lv_obj_get_style_bg_color(strip_dot, 0), dot) ||
                                  (lv_obj_get_style_border_width(strip_dot, 0) != 0) != ring)) {
        lv_obj_set_style_bg_color(strip_dot, dot, 0);
        lv_obj_set_style_border_color(strip_dot, dot, 0);
        lv_obj_set_style_border_width(strip_dot, ring ? 2 : 0, 0);
        lv_obj_set_style_bg_opa(strip_dot, ring ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    }
    show(strip_icon, kind >= UI_STRIP_WARN);
    ui_set_color(strip_icon, dim ? dis_tx : kind == UI_STRIP_FAULT ? UI_FAULT : lv_color_white());
    ui_set_color(strip_text, dim ? dis_tx : kind >= UI_STRIP_WARN ? lv_color_white()
                             : kind == UI_STRIP_PROMPT ? lv_color_black() : UI_TEXT);
    bool spark = kind == UI_STRIP_INFO && is_mode && !dim && !ok && !act && strip_w >= 300;
    int32_t x = kind == UI_STRIP_PROMPT ? 12 : 28;
    int32_t w = (spark ? strip_w - 155 : act ? strip_w - 104 : ok ? strip_w - 64 : strip_w - 4) - x - 4;
    if (lv_obj_get_width(strip) != strip_w) {
        lv_obj_set_width(strip, strip_w);
        lv_obj_set_x(strip_ok, strip_w - 64);
        lv_obj_set_x(strip_act, strip_w - 104);
        lv_obj_set_x(strip_spark, strip_w - 151);
    }
    show(strip_spark, spark);
    static uint32_t seen = UINT32_MAX;
    if (spark && trend_seconds() != seen) {   // a new sample: redraw it
        seen = trend_seconds();
        lv_obj_invalidate(strip_spark);
    }
    if (lv_obj_get_x(strip_text) != x || lv_obj_get_width(strip_text) != w) {
        lv_obj_set_x(strip_text, x);
        lv_obj_set_width(strip_text, w);
    }
    ui_set_text(strip_text, text);
    lv_obj_align(strip_text, LV_ALIGN_LEFT_MID, x, 0);
    // Under a popup the strip stays, updating, greyed and without its buttons.
    show(strip_ok, ok && !overlay);
    show(strip_act, act && !overlay);

}

static int32_t text_w(const char *t)
{
    lv_point_t sz;
    lv_text_get_size(&sz, t, &ui_font_16, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x;
}

// Centre the title on the screen; when the mode word leaves no room for
// that, centre it in the gap between Back and Home (or the edge, on Home)
// and the mode word, and cut it with dots only if it is wider than the gap.
static void title_fit(lv_obj_t *l, const char *text, const char *mode_txt, int32_t left)
{
    int32_t right = 454 - text_w(mode_txt) - 8;
    int32_t w = text_w(text);
    int32_t x = 240 - w / 2;
    if (w > right - left)
        w = right - left;
    if (x < left || x + w > right)
        x = left + (right - left - w) / 2;
    lv_obj_set_pos(l, x, 11);
    lv_obj_set_width(l, w + 1);
}

static void override_cancel_event(lv_event_t *e);

static void frame_refresh(void)
{
    const char *tag, *text;
    int nf = valve_fault_count();
    bool f12 = valve_fault_is("F-12");
    valve_mode_t m = valve_mode();

    // Header.
    bool home = cur == &ui_scr_home;
    show(back_btn, !home || overlay);
    show(home_btn, !home && !overlay);
    show(pmc_lbl, home && !overlay);
    show(title_lbl, !home);
    const char *mode_txt = f12 ? "STOPPED" : nf ? "FAULT" : valve_mode_name();
    static const char *fit_mode, *fit_title;
    // Home's title: "<cryostat> pressure controller" (Settings > Cryostat name).
    static char home_title[CFG_NAME_LEN + 24], fit_name[CFG_NAME_LEN + 1] = "\x01";
    bool renamed = strcmp(cfg_name(), fit_name) != 0;
    if (renamed) {
        lv_strlcpy(fit_name, cfg_name(), sizeof fit_name);
        if (fit_name[0])
            lv_snprintf(home_title, sizeof home_title, "%s pressure controller", fit_name);
        else
            lv_strlcpy(home_title, "Pressure controller", sizeof home_title);
        ui_set_text(pmc_lbl, home_title);
    }
    ui_set_text(mode_lbl, mode_txt);
    if (renamed || mode_txt != fit_mode || (!home && cur->title != fit_title)) {
        fit_mode = mode_txt;
        if (!home) {
            fit_title = cur->title;
            ui_set_text(title_lbl, cur->title);
            title_fit(title_lbl, cur->title, mode_txt, 2 * HDR_BTN_W + 4);
        }
        // Home's title sits on the left (Ryan, 16 Oct: centred left the
        // top left empty); it is centred in the gap only if it is too long.
        title_fit(pmc_lbl, home_title, mode_txt, 10);
        if (text_w(home_title) < 454 - text_w(mode_txt) - 8 - 12)
            lv_obj_set_x(pmc_lbl, 12);
    }
    show(fault_rule, nf > 0);
    bool op = !valve_stop_latches();
    if (op != lv_obj_has_state(stop_btn, LV_STATE_USER_2)) {
        if (op) lv_obj_add_state(stop_btn, LV_STATE_USER_2);
        else    lv_obj_remove_state(stop_btn, LV_STATE_USER_2);
    }
    // Mode dot (simulator WP023 S4): red and filled with a latched fault,
    // filled grey in AUTO, a grey ring in every other mode; shape, not
    // colour, tells AUTO apart. (Before 0.10.5 the ring never drew: its
    // border was only set on a change of fill, and the first was none.)
    static int dot_kind = -1;
    bool in_auto = valve_auto();
    int kind = nf ? 2 : in_auto ? 1 : 0;
    if (kind != dot_kind) {
        dot_kind = kind;
        lv_obj_set_style_bg_opa(mode_dot, kind ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(mode_dot, kind == 2 ? UI_FAULT : lv_color_hex(0xC0C0C0), 0);
        lv_obj_set_style_border_width(mode_dot, kind ? 0 : 2, 0);
    }

    question_check();

    // The screen's own line and Home's action, then the frame's priorities.
    act_cb = NULL;
    if (cur->refresh)
        cur->refresh();
    if (msg_until && (int32_t)(lv_tick_get() - msg_until) >= 0)
        msg_until = 0;
    // A new fault takes the strip at once: it ends a message, and closes the
    // keypad (which hides the strip); the entry is lost, the fault is seen.
    static int nf_seen;
    if (nf > nf_seen) {
        msg_until = 0;
        if (kp_open)
            ui_overlay_close();
    }
    nf_seen = nf;
    // The keypad uses the strip's row; an empty line (Home, idle) hides it.
    bool quiet = !msg_until && !nf && !stepper3_override() && !line_text[0];
    show(strip, (!overlay || !kp_open) && !quiet);
    char buf[96];
    if (msg_until) {
        strip_draw(UI_STRIP_WARN, UI_IDLE, msg_text, true, false);
    } else if (valve_fault_first(&tag, &text)) {
        if (f12)
            lv_strlcpy(buf, "Stopped where it was, not closed.", sizeof buf);
        else if (nf > 1)
            lv_snprintf(buf, sizeof buf, "%s %s (+%d)", tag, text, nf - 1);
        else
            lv_snprintf(buf, sizeof buf, "%s %s", tag, text);
        strip_draw(UI_STRIP_FAULT, UI_IDLE, buf, false, act_cb != NULL);
    } else if (stepper3_override()) {
        // Cancel on every screen while it is armed (Ryan, 16 Oct).
        ui_strip_action("Cancel", NULL, override_cancel_event);
        strip_draw(UI_STRIP_WARN, UI_IDLE, "Limit override on", false, true);
    } else {
        strip_draw(line_kind, line_dot, line_text, false, act_cb != NULL);
    }
}

static void override_cancel_event(lv_event_t *e)
{
    (void)e;
    bool stopped = stepper3_override_cancel();
    valve_event("LIM", "Limit override off (screen)%s", stopped ? ", valve stopped" : "");
    ui_msg(stopped ? "Limit override off, valve stopped." : "Limit override off.");
}

static void fast_timer(lv_timer_t *t)
{
    (void)t;
    if (service)
        return;
    if (cur->fast)
        cur->fast();
    if (kp_open)
        kp_side();             // the keypad's present value follows the valve
}

static void refresh_timer(lv_timer_t *t)
{
    (void)t;
    if (service) {
        // Back on Home once the motor screen is left (its Back calls ui_home()).
        return;
    }
    frame_refresh();
}

/* --------------------------------------------------------------------------
 * Screens
 * ------------------------------------------------------------------------ */
static lv_obj_t *new_screen(void)
{
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_remove_style_all(s);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s, UI_BG, 0);
    lv_obj_set_style_text_font(s, &ui_font_16, 0);
    lv_obj_set_style_text_color(s, UI_TEXT, 0);
    return s;
}

void ui_open(const ui_screen_t *s)
{
    if (cur == s)   // e.g. the strip's sparkline tapped on Trend
        return;
    if (cur != &ui_scr_home && cur != s && !service && back_n < (int)(sizeof back_stack / sizeof back_stack[0]))
        back_stack[back_n++] = cur;
    open_screen(s);
}

static void open_screen(const ui_screen_t *s)
{
    ui_overlay_close();
    lv_obj_t *old = cur_scr;
    lv_obj_t *scr = new_screen();
    cur = s;
    cur_scr = scr;
    strip_w = 343;
    s->build(scr);
    lv_screen_load(scr);
    if (old != home_scr)
        lv_obj_delete(old);
    msg_until = 0;
    frame_refresh();
}

void ui_home(void)
{
    back_n = 0;
    ui_overlay_close();
    lv_obj_t *old = cur_scr;
    cur = &ui_scr_home;
    cur_scr = home_scr;
    strip_w = 343;
    lv_screen_load(home_scr);
    if (service) {
        service = false;
        show(frame, true);
        motor_screen_close();   // its LVGL heap back
    } else if (old != home_scr) {
        lv_obj_delete(old);
    }
    frame_refresh();
}

// Back from the motor screen: Settings, with Menu and Home behind it. Run
// after the Back event, so the motor screen can be freed before Menu and
// Settings are built (both at once ran a host test's heap out).
static void service_back_now(void *p)
{
    (void)p;
    if (!service)
        return;
    ui_home();
    ui_open(&ui_scr_menu);
    ui_open(&ui_scr_settings);
}

static void service_back(void)
{
    lv_async_call(service_back_now, NULL);
}

void ui_service(void)
{
    ui_overlay_close();
    show(frame, false);
    service = true;
    // The screen left (Settings) goes first, so the two are never built
    // at once; the motor screen is built on open and freed on leave.
    if (cur_scr != home_scr) {
        lv_screen_load(home_scr);
        lv_obj_delete(cur_scr);
    }
    cur = &ui_scr_home;
    cur_scr = home_scr;
    motor_screen_init(service_back);
    motor_screen_show();
}

bool ui_on_service(void) { return service; }

bool ui_showing(const ui_screen_t *s) { return cur == s && !service; }

/* --------------------------------------------------------------------------
 * Overlays: keypad and confirm, over 0,40 480x232 (STOP stays on top)
 * ------------------------------------------------------------------------ */
bool ui_overlay_open(void) { return overlay != NULL; }

void ui_overlay_close(void)
{
    if (overlay) {
        lv_obj_delete(overlay);
        overlay = NULL;
    }
    overlay_q = 0;
    overlay_tv = false;
    kp_open = false;
}

lv_obj_t *ui_overlay(void) { return overlay_new(); }

static lv_obj_t *overlay_new(void)
{
    ui_overlay_close();
    overlay = plain(cur_scr);
    lv_obj_set_pos(overlay, 0, 40);
    lv_obj_set_size(overlay, 480, 232);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);   // nothing under it is touched
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(overlay, UI_BG, 0);
    return overlay;
}

// Keypad.
static struct {
    char entry[10];
    ui_kp_t k;
    lv_obj_t *value, *title, *info, *err, *live;
} kp;

static const char *fmt_dec(char *buf, size_t n, int64_t v, int dec)
{
    return fixed_fmt(buf, n, v, dec);
}

static void kp_side(void)
{
    char v[16];
    if (kp.live && kp.k.live_milli)
        ui_set_text(kp.live, kp.k.live_dec == 1 ? ui_fmt1(v, sizeof v, kp.k.live_milli())
                                                 : ui_fmt2(v, sizeof v, kp.k.live_milli()));
}

static void kp_show_err(const char *why)
{
    show(kp.title, why == NULL);
    show(kp.info, why == NULL);
    show(kp.err, why != NULL);
    if (why)
        ui_set_text(kp.err, why);
}

// The entry in 10^-dec units; false if empty or too large.
static bool kp_parse(int64_t *v)
{
    const char *s = kp.entry;
    int64_t whole = 0, frac = 0;
    bool neg = *s == '-';
    if (neg) s++;
    if (!*s)
        return false;
    while (*s >= '0' && *s <= '9')
        whole = whole * 10 + (*s++ - '0');
    if (*s == '.') {
        s++;
        for (int i = 0; i < kp.k.dec; i++) {
            frac *= 10;
            if (*s >= '0' && *s <= '9')
                frac += *s++ - '0';
        }
    }
    int64_t scale = 1;
    for (int i = 0; i < kp.k.dec; i++)
        scale *= 10;
    *v = (neg ? -1 : 1) * (whole * scale + frac);
    return true;
}

static void kp_key(lv_event_t *e)
{
    const char *k = lv_event_get_user_data(e);
    size_t n = strlen(kp.entry);
    char *dot = strchr(kp.entry, '.');
    kp_show_err(NULL);
    if (strcmp(k, "<") == 0) {
        if (n) kp.entry[n - 1] = '\0';
    } else if (strcmp(k, "set") == 0) {
        int64_t v;
        const char *why = NULL;
        if (!kp_parse(&v)) {
            ui_overlay_close();
            return;
        }
        if (v < kp.k.min || v > kp.k.max) {
            char a[16], b[16], t[48];
            lv_snprintf(t, sizeof t, "Outside %s to %s", fmt_dec(a, sizeof a, kp.k.min, kp.k.dec),
                        fmt_dec(b, sizeof b, kp.k.max, kp.k.dec));
            kp_show_err(t);
            return;
        }
        if (!kp.k.on_set((int32_t)v, &why)) {
            kp_show_err(why ? why : "Refused");
            return;
        }
        // on_set may have opened the next overlay (a pick list): leave it.
        if (kp_open)
            ui_overlay_close();
        frame_refresh();
        return;
    } else if (n < sizeof kp.entry - 1) {
        if (*k == '.') {
            if (dot || kp.k.dec == 0) return;
            if (!n) { strcpy(kp.entry, "0"); n = 1; }
        } else if (dot && (int)strlen(dot) > kp.k.dec) {
            return;                           // no more decimals
        } else if (n == 1 && kp.entry[0] == '0') {
            n = 0;                            // no leading zero
        }
        kp.entry[n] = *k;
        kp.entry[n + 1] = '\0';
    }
    ui_set_text(kp.value, kp.entry);
}

void ui_keypad_ex(const ui_kp_t *k)
{
    lv_obj_t *o = overlay_new();
    memset(&kp, 0, sizeof kp);
    kp.k = *k;
    kp_open = true;

    lv_obj_t *card = ui_card(o, 10, 8, 343, 48);
    kp.title = ui_label(card, 10, 6, 180, &ui_font_12, UI_TEXT, LV_TEXT_ALIGN_LEFT, k->title);
    kp.info = ui_label(card, 10, 24, 180, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT, k->info);
    kp.err = ui_label(card, 10, 6, 176, &ui_font_12, UI_FAULT, LV_TEXT_ALIGN_LEFT, "");
    lv_label_set_long_mode(kp.err, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(kp.err, LV_OBJ_FLAG_HIDDEN);
    kp.value = ui_label(card, 190, 10, 143, &ui_num_26, UI_TEXT, LV_TEXT_ALIGN_RIGHT, "");

    static const char *const keys[3][4] = { { "7", "8", "9", "<" }, { "4", "5", "6", "0" }, { "1", "2", "3", "." } };
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++) {
            const char *key = keys[r][c];
            bool bs = *key == '<';
            lv_obj_t *b = ui_btn(o, 10 + c * 70, 64 + r * 56, bs ? 132 : 62, 48, UI_SECONDARY,
                                 bs ? LV_SYMBOL_BACKSPACE : key, kp_key, (void *)key);
            if (bs)
                lv_obj_set_style_text_font(lv_obj_get_child(b, 0), &lv_font_montserrat_14, 0);
            if (*key == '.' && k->dec == 0)
                ui_btn_enable(b, false);
        }
    ui_btn(o, 290, 120, 62, 104, UI_PRIMARY, "Set", kp_key, (void *)"set");

    // Side card: the live value (one row), or In use and Saved (two rows).
    lv_obj_t *side = ui_card(o, 361, 8, 109, 160);
    if (k->live_label) {
        ui_label(side, 10, 52, 89, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT, k->live_label);
        kp.live = ui_label(side, 4, 72, 97, &ui_num_26, UI_TEXT, LV_TEXT_ALIGN_RIGHT, "");
        kp_side();
    } else {
        int rows = k->side_text[1] ? 2 : k->side_text[0] ? 1 : 0;
        for (int i = 0; i < rows; i++) {
            int32_t y = rows == 1 ? 52 : 18 + i * 72;
            ui_label(side, 10, y, 89, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT, k->side_label[i]);
            lv_obj_t *l = ui_label(side, 4, y + 20, 97, strlen(k->side_text[i]) > 7 ? &ui_font_16 : &ui_num_26,
                                   UI_TEXT, LV_TEXT_ALIGN_RIGHT, k->side_text[i]);
            (void)l;
        }
        if (!rows)
            lv_obj_add_flag(side, LV_OBJ_FLAG_HIDDEN);
    }
    frame_refresh();
}

static ui_keypad_set_t kp_v100_set;
static bool kp_v100(int32_t v, const char **why) { return kp_v100_set(v, why); }

void ui_keypad(const char *title, const char *info, int32_t min100, int32_t max100,
               const char *side_label, int32_t (*side_milli)(void), ui_keypad_set_t on_set)
{
    kp_v100_set = on_set;
    ui_kp_t k = { .title = title, .info = info, .dec = 2, .min = min100, .max = max100,
                  .live_label = side_label, .live_milli = side_milli, .live_dec = 2, .on_set = kp_v100 };
    ui_keypad_ex(&k);
}

// Pick list: a caption and up to six rows, the selected one dark. A tap
// outside the list closes it.
static void (*pick_cb)(int);

static void pick_event(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    void (*f)(int) = pick_cb;
    ui_overlay_close();
    if (i >= 0 && f)
        f(i);
    frame_refresh();
}

void ui_pick(const char *title, const char *const *items, int n, int sel, void (*on_pick)(int i))
{
    lv_obj_t *o = overlay_new();
    lv_obj_set_style_bg_color(o, lv_color_make(40, 50, 60), 0);
    lv_obj_set_style_bg_opa(o, 140, 0);
    lv_obj_add_event_cb(o, pick_event, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    pick_cb = on_pick;
    if (n > 6)
        n = 6;
    // Rows 40 px, as other touch targets (review of 0.10.11; were 32). Above
    // the strip that leaves three rows: four items (sensor names are long)
    // stay in one column at 37 px, five or six go in two columns.
    int cols = n > 4 ? 2 : 1, rows = (n + cols - 1) / cols;
    const int32_t PICK_ROW = rows > 3 ? 37 : 40;
    int32_t iw = 343 / cols;
    lv_obj_t *box = ui_card(o, 10, 4, 343, 24 + PICK_ROW * rows);
    lv_obj_set_style_clip_corner(box, true, 0);
    ui_label(box, 12, 4, 320, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT, title);
    for (int i = 0; i < n; i++) {
        lv_obj_t *it = plain(box);
        lv_obj_set_pos(it, (i / rows) * iw, 24 + PICK_ROW * (i % rows));
        lv_obj_set_size(it, i / rows ? 343 - iw : iw, PICK_ROW);
        lv_obj_add_flag(it, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(it, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(it, i == sel ? lv_color_hex(0x202020) : UI_CARD, 0);
        lv_obj_set_style_bg_color(it, UI_TRACK, LV_STATE_PRESSED);
        lv_obj_set_style_border_side(it, i / rows ? LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT : LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_border_width(it, 1, 0);
        lv_obj_set_style_border_color(it, UI_BG, 0);
        lv_obj_t *l = ui_label(it, 12, (PICK_ROW - 18) / 2, iw - 23, &ui_font_16, i == sel ? UI_CARD : UI_TEXT, LV_TEXT_ALIGN_LEFT, items[i]);
        (void)l;
        lv_obj_add_event_cb(it, pick_event, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    frame_refresh();
}

// Confirm and question dialogs.
static void (*confirm_ok)(void), (*confirm_cancel)(void), (*confirm_mid)(void);

static void confirm_event(lv_event_t *e)
{
    intptr_t k = (intptr_t)lv_event_get_user_data(e);   // 0 cancel, 1 OK, 2 middle
    void (*f)(void) = k == 1 ? confirm_ok : k == 2 ? confirm_mid : confirm_cancel;
    ui_overlay_close();
    if (f)
        f();
    frame_refresh();
}

void ui_ask(const char *text, const char *cancel, void (*on_cancel)(void), const char *ok, void (*on_ok)(void))
{
    ui_ask3(text, cancel, on_cancel, NULL, NULL, ok, on_ok);
}

void ui_ask3(const char *text, const char *cancel, void (*on_cancel)(void), const char *mid,
             void (*on_mid)(void), const char *ok, void (*on_ok)(void))
{
    lv_obj_t *o = overlay_new();
    lv_obj_set_style_bg_color(o, lv_color_make(40, 50, 60), 0);
    lv_obj_set_style_bg_opa(o, 140, 0);
    confirm_ok = on_ok;
    confirm_cancel = on_cancel;
    confirm_mid = on_mid;
    lv_obj_t *box = ui_card(o, 70, 0, 340, 100);
    lv_obj_set_style_radius(box, 0, 0);
    lv_obj_t *t = ui_label(box, 16, 14, 308, &ui_font_16, UI_TEXT, LV_TEXT_ALIGN_LEFT, text);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_update_layout(t);
    int32_t th = lv_obj_get_height(t);
    if (th > 84) th = 84;
    int32_t by = 14 + th + 10, h = by + 60;
    lv_obj_set_height(t, th);
    lv_obj_set_size(box, 340, h);
    lv_obj_set_y(box, (168 - h) / 2);
    if (mid) {
        ui_btn(box, 16, by, 96, 48, UI_SECONDARY, cancel, confirm_event, NULL);
        ui_btn(box, 122, by, 96, 48, UI_SECONDARY, mid, confirm_event, (void *)2);
        ui_btn(box, 228, by, 96, 48, UI_PRIMARY, ok, confirm_event, (void *)1);
    } else {
        ui_btn(box, 16, by, 150, 48, UI_SECONDARY, cancel, confirm_event, NULL);
        ui_btn(box, 174, by, 150, 48, UI_PRIMARY, ok, confirm_event, (void *)1);
    }
    frame_refresh();
}

void ui_confirm(const char *text, const char *ok, void (*on_ok)(void))
{
    ui_ask(text, "Cancel", NULL, ok, on_ok);
}

// Text page over the screen (Help). Header Back closes it. No dragging (a
// drag redraws the card every frame and crowded Back): small Up and Down
// buttons on the right jump a page less one line, without animation.
// Layout A: wide text card, 52 px buttons at the right edge.
// Layout B: taller text card down to the bottom row, buttons over STOP.
#ifndef TV_LAYOUT_B
#define TV_W 400
#define TV_H 152
#define TV_BX 418
#define TV_BW 52
#else
#define TV_W 341
#define TV_H 208
#define TV_BX 361
#define TV_BW 109
#endif
#define TV_STEP (TV_H - 38)   // a page less one line
static lv_obj_t *tv_card, *tv_btn[2];

static void tv_buttons(void)
{
    for (int i = 0; i < 2; i++) {
        bool room = i ? lv_obj_get_scroll_bottom(tv_card) > 0 : lv_obj_get_scroll_top(tv_card) > 0;
        ui_btn_enable(tv_btn[i], room);
    }
}

static void tv_page(lv_event_t *e)
{
    if (ui_btn_disabled(e))
        return;
    int32_t dir = lv_event_get_user_data(e) ? 1 : -1;
    int32_t room = dir > 0 ? lv_obj_get_scroll_bottom(tv_card) : lv_obj_get_scroll_top(tv_card);
    int32_t d = room < TV_STEP ? room : TV_STEP;
    if (d > 0)
        lv_obj_scroll_by(tv_card, 0, -dir * d, LV_ANIM_OFF);
    tv_buttons();
}

void ui_text_view(const char *title, const char *text)
{
    lv_obj_t *o = overlay_new();
    // Not clickable, so a touch on the text does nothing; it still scrolls
    // from the buttons and shows where it is with the scroll bar.
    lv_obj_t *c = ui_card(o, 10, 16, TV_W, TV_H);
    tv_card = c;
    lv_obj_add_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_MOMENTUM |
                          LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_set_scroll_dir(c, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_ON);
    lv_obj_set_style_width(c, 6, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(c, 0, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(c, UI_IDLE, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(c, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_ver(c, 6, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_all(c, 10, 0);
    lv_obj_set_style_pad_right(c, 20, 0);
    lv_obj_t *t = ui_label(c, 0, 0, 0, &ui_font_16, UI_TEXT, LV_TEXT_ALIGN_LEFT, title);
    t = ui_label(c, 0, 26, TV_W - 30, &ui_font_12, UI_TEXT, LV_TEXT_ALIGN_LEFT, text);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(t, 3, 0);
    // On press (not release); held, it repeats every 0.1 s after 0.4 s.
    // Greyed at the top and the bottom.
    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = ui_btn(o, TV_BX, i ? 96 : 16, TV_BW, 72, UI_SECONDARY,
                             i ? LV_SYMBOL_DOWN : LV_SYMBOL_UP, NULL, NULL);
        lv_obj_add_event_cb(b, tv_page, LV_EVENT_PRESSED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(b, tv_page, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)i);
        tv_btn[i] = b;
    }
    lv_obj_update_layout(c);
    tv_buttons();
    overlay_tv = true;
    frame_refresh();
}

// The limit switch pressed during a move: asks on every screen until answered.
static void q_answer(bool zero)
{
    const char *why;
    if (!valve_answer(zero, &why))
        ui_msg(why);
    else if (zero)
        ui_msg("Set as 0 %.");
}
static void q_zero(void) { q_answer(true); }
static void q_skip(void) { q_answer(false); }

// A saved valve position at start-up: offered, never taken without asking.
static void sp_use(void)
{
    const char *why;
    if (!valvepos_use(&why))
        ui_msg(why);
}
static void sp_calibrate(void)
{
    valvepos_decline();
    ui_calibrate_ask();
}

static void question_check(void)
{
    int32_t q;
    bool lim = valve_question(&q);
    bool saved = !lim && valvepos_offer(&q);
    int want = lim ? 1 : saved ? 2 : 0;
    if (overlay_q && overlay_q != want) {
        ui_overlay_close();
    }
    static bool asking;
    // A keypad or confirm the operator opened stays; the question waits.
    if (!want || overlay_q || asking || (want == 2 && overlay))
        return;
    static char t[128], ok[32];
    char v[16];
    asking = true;
    if (want == 1) {
        lv_snprintf(t, sizeof t, "Limit switch pressed at %s %%. Set this point as 0 %%?",
                    ui_fmt1(v, sizeof v, (int64_t)q * 100));
        ui_ask(t, "Skip", q_skip, "Set as 0 %", q_zero);
    } else {
        int32_t pm = valve_pos_max();
        ui_fmt2(v, sizeof v, ((int64_t)q * 100000 + (q >= 0 ? pm / 2 : -pm / 2)) / pm);
        lv_snprintf(t, sizeof t, "Valve position saved at %s %% before the restart. Not checked: "
                    "if it was moved while off, home it. Use it, or home the valve?", v);
        lv_snprintf(ok, sizeof ok, "Use %s %%", v);
        ui_ask(t, "Home", sp_calibrate, ok, sp_use);
    }
    asking = false;
    overlay_q = want;
}

/* --------------------------------------------------------------------------
 * Start-up
 * ------------------------------------------------------------------------ */
void ui_init(void)
{
    num_font(&ui_num_16, &ui_font_16);
    num_font(&ui_num_26, &ui_font_26);
    num_font(&ui_num_40, &ui_font_40);
    styles_init();
    // 8-bit: no antialiasing, so lines, arcs and edges stay on whole pixels.
    lv_display_set_antialiasing(lv_display_get_default(), false);
    home_scr = new_screen();
    cur_scr = home_scr;
    ui_scr_home.build(home_scr);
    build_frame();
    lv_screen_load(home_scr);
    frame_refresh();
    lv_timer_create(refresh_timer, 500, NULL);
    lv_timer_create(fast_timer, 100, NULL);
    // Drags start after 3 px, not LVGL's 10, so Help follows the finger sooner.
    for (lv_indev_t *in = lv_indev_get_next(NULL); in; in = lv_indev_get_next(in))
        lv_indev_set_scroll_limit(in, 3);
}
