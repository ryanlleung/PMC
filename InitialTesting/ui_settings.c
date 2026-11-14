#include <stdint.h>
#include <string.h>

#include "lvgl.h"
#include "clicks.h"
#include "cal.h"
#include "cfg.h"
#include "pmc_config.h"
#include "link.h"
#include "ethlink.h"
#include "sysinfo.h"
#include "rtclock.h"
#include "fixed.h"
#include "limit.h"
#include "stepper3.h"
#include "valve.h"
#include "valvepos.h"
#include "motor_screen.h"
#include "ui.h"

/* --------------------------------------------------------------------------
 * The settings pages of the 0.17 screens (Ryan 14 Nov; the design canvas
 * "PMC Screens 0.17"), each on the Menu: PID, Valve, Motor, Pressure
 * sensor and System.
 *
 * PID, Valve and Motor are tabbed pages: a bar of equal tabs on the top
 * row, the tab's cards under it, Help, Save and Undo on the right.
 *
 * An edit is in use at once (0.17.0; a draft until Save from 0.15.x to
 * 0.16.33): the keypad's Set puts the value in use (cfg_apply_with, with
 * the checks of a save). Save writes everything in use to flash; Undo puts
 * the last save back in use. The header's unsaved icon and a dark edge on
 * a card show what is in use and not saved. Factory reset is on the Menu.
 * ------------------------------------------------------------------------ */

#define COL_EDGE   UI_TEXT   // a card in use but not saved: a dark left edge
#define STEPS_PER_TURN cfg_steps_per_turn()
#define TRACK_GREY lv_color_hex(0xC3CDD7)

static const int32_t RY[4] = { 48, 104, 160, 216 };
#define RH 48
// Two columns inside 10-353, as cols(2) in the simulator.
static const int32_t C2X[2] = { 10, 186 }, C2W[2] = { 168, 167 };

/* --------------------------------------------------------------------------
 * Settings in use
 * ------------------------------------------------------------------------ */
static int32_t live(cfg_key_t k)
{
    int32_t v = 0;
    cfg_value(CFG_FROM_LIVE, k, &v);
    return v;
}

static bool unsaved_key(cfg_key_t k)
{
    int32_t s;
    return cfg_value(CFG_FROM_SAVED, k, &s) && s != live(k);
}

static const char *plural(char *buf, size_t n, int count, const char *word)
{
    lv_snprintf(buf, n, "%d %s%s", count, word, count == 1 ? "" : "s");
    return buf;
}

// A save is named by when it was made: "16/10/26 14:32", or "date unknown"
// when the clock was not set at the time.
static const char *save_when(char *buf, size_t n, const char *at)
{
    return at[0] ? ui_date(buf, n, at) : "date unknown";
}

/* --------------------------------------------------------------------------
 * Fields
 * ------------------------------------------------------------------------ */
typedef enum { K_NUM, K_TURNS, K_TOGGLE } kind_t;

typedef struct {
    const char *cap, *unit;
    uint8_t dec;
    uint8_t kind;
} field_t;

// In cfg_key_t order. The captions as on the cards ("Speed, st/s").
static const field_t F[CFG_N] = {
    [CFG_POSMAX]  = { "100 %", "turns", 2, K_TURNS },
    [CFG_REVERSE] = { "Direction", NULL, 0, K_TOGGLE },
    [CFG_BACKOFF] = { "Back-off", "steps", 0, K_NUM },
    [CFG_RETURN]  = { "Return", "steps", 0, K_NUM },
    [CFG_FHOME]   = { "Fast close", "steps/s", 0, K_NUM },
    [CFG_FCREEP]  = { "Slow close", "steps/s", 0, K_NUM },
    [CFG_RATE]    = { "Speed", "st/s", 0, K_NUM },
    [CFG_START]   = { "Start", "st/s", 0, K_NUM },
    [CFG_ACCEL]   = { "Accel", "st/s/s", 0, K_NUM },
    [CFG_KP]      = { "KP", "%/mB", 2, K_NUM },
    [CFG_TI]      = { "TI", "min", 2, K_NUM },
    [CFG_OUTMIN]  = { "AUTO min", "%", 1, K_NUM },
    [CFG_DB]      = { "Deadband", "steps", 0, K_NUM },
    [CFG_BAND]    = { "SP band", "mB", 2, K_NUM },
    [CFG_KD]      = { "KD", "%s/mB", 2, K_NUM },
    [CFG_KP2]     = { "KP", "%/mB", 2, K_NUM },
    [CFG_TI2]     = { "TI", "min", 2, K_NUM },
    [CFG_KD2]     = { "KD", "%s/mB", 2, K_NUM },
    [CFG_KP3]     = { "KP", "%/mB", 2, K_NUM },
    [CFG_TI3]     = { "TI", "min", 2, K_NUM },
    [CFG_KD3]     = { "KD", "%s/mB", 2, K_NUM },
    [CFG_B1]      = { "1 to 2", "mB", 1, K_NUM },
    [CFG_B2]      = { "2 to 3", "mB", 1, K_NUM },
    [CFG_HYST]    = { "Hysteresis", "%", 0, K_NUM },
    [CFG_KP4]     = { "KP", "%/mB", 2, K_NUM },
    [CFG_TI4]     = { "TI", "min", 2, K_NUM },
    [CFG_KD4]     = { "KD", "%s/mB", 2, K_NUM },
    [CFG_B3]      = { "3 to 4", "mB", 1, K_NUM },
    [CFG_PASS_LO] = { "From", "%", 0, K_NUM },
    [CFG_PASS_HI] = { "To", "%", 0, K_NUM },
    [CFG_SPT]     = { "Steps/turn", NULL, 0, K_NUM },
};

static const char *value_text(char *buf, size_t n, cfg_key_t k, int32_t v)
{
    switch (F[k].kind) {
    case K_TURNS:
        return fixed_fmt(buf, n, fixed_round((int64_t)v * 1000 / STEPS_PER_TURN, 1), 2);
    case K_TOGGLE:   // CFG_REVERSE, the only one
        return v ? "Reversed" : "Normal";
    default:
        // 0 means none here (0.16.25): no set 4, no band.
        if (v == 0 && (k == CFG_B3 || k == CFG_BAND))
            return "None";
        return fixed_fmt(buf, n, v, F[k].dec);
    }
}

static const char *caption(char *buf, size_t n, cfg_key_t k)
{
    if (F[k].unit)
        lv_snprintf(buf, n, "%s, %s", F[k].cap, F[k].unit);
    else
        lv_snprintf(buf, n, "%s", F[k].cap);
    return buf;
}

// The keypad's title: the caption, with the set for a PID gain ("Set 2
// KP, %/mB").
static const char *kp_title(char *buf, size_t n, cfg_key_t k)
{
    for (int s = 1; s <= 4; s++)
        for (int w = 0; w < 3; w++)
            if (cfg_pid_key(s, w) == k) {
                lv_snprintf(buf, n, "Set %d %s, %s", s, F[k].cap, F[k].unit);
                return buf;
            }
    return caption(buf, n, k);
}

/* Editable card: white, outlined, 12 px caption, 16 px value, chevron; a
 * 3 px dark edge on the left while its value is in use and not saved. */
typedef struct {
    lv_obj_t *card, *edge, *v;
} ecard_t;

static void ecard_make(ecard_t *e, lv_obj_t *p, int32_t x, int32_t y, int32_t w, const char *cap,
                       lv_event_cb_t cb, void *user)
{
    e->card = ui_card(p, x, y, w, RH);
    lv_obj_add_flag(e->card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_width(e->card, 1, 0);
    lv_obj_set_style_border_color(e->card, UI_TICK, 0);
    lv_obj_set_style_bg_color(e->card, UI_TRACK, LV_STATE_PRESSED);
    lv_obj_add_event_cb(e->card, cb, LV_EVENT_CLICKED, user);
    ui_label(e->card, 11, 5, w - 31, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT, cap);
    e->v = ui_label(e->card, 11, 22, w - 31, &ui_font_16, UI_TEXT, LV_TEXT_ALIGN_LEFT, "");
    lv_label_set_long_mode(e->v, LV_LABEL_LONG_DOT);
    ui_label(e->card, w - 20, 15, 0, &lv_font_montserrat_14, UI_IDLE, LV_TEXT_ALIGN_LEFT, LV_SYMBOL_RIGHT);
    e->edge = ui_card(e->card, -1, 5, 3, 36);
    lv_obj_set_style_radius(e->edge, 0, 0);
    lv_obj_set_style_border_width(e->edge, 0, 0);
    lv_obj_set_style_bg_color(e->edge, COL_EDGE, 0);
    lv_obj_set_style_bg_opa(e->edge, LV_OPA_TRANSP, 0);
}

static void ecard_state(ecard_t *e, const char *text, bool edge)
{
    ui_set_text(e->v, text);
    lv_opa_t opa = edge ? LV_OPA_COVER : LV_OPA_TRANSP;
    if (lv_obj_get_style_bg_opa(e->edge, 0) != opa)
        lv_obj_set_style_bg_opa(e->edge, opa, 0);
}

static void field_update(ecard_t *e, cfg_key_t k)
{
    char buf[40];
    ecard_state(e, value_text(buf, sizeof buf, k, live(k)), unsaved_key(k));
}

/* Editing: the value goes in use at once */
static cfg_key_t ed_key;

static bool set_now(cfg_key_t k, int32_t v, const char **why)
{
    int32_t vals[CFG_N];
    bool set[CFG_N];
    char t[40], b[16];
    if (v == live(k))
        return true;
    memset(set, 0, sizeof set);
    memset(vals, 0, sizeof vals);
    vals[k] = v;
    set[k] = true;
    if (!cfg_apply_with(vals, set, why))
        return false;
    valve_event("CFG", "%s set to %s (screen), in use, not saved", kp_title(t, sizeof t, k),
                value_text(b, sizeof b, k, v));
    ui_refresh_soon();
    return true;
}

static bool ed_set(int32_t v, const char **why)
{
    return set_now(ed_key, v, why);
}

static bool ed_turns(int32_t v100, const char **why)
{
    return set_now(CFG_POSMAX, (int32_t)(((int64_t)v100 * STEPS_PER_TURN + 50) / 100), why);
}

// The keypad's side rows: In use, and Saved when it differs.
static void kp_rows(ui_kp_t *k, cfg_key_t key, char *now, char *sv, size_t n)
{
    int32_t s;
    bool have = cfg_value(CFG_FROM_SAVED, key, &s);
    k->side_label[0] = "In use";
    k->side_text[0] = value_text(now, n, key, live(key));
    if (have && s != live(key)) {
        k->side_label[1] = "Saved";
        k->side_text[1] = value_text(sv, n, key, s);
    }
}

static void ed_toggle_ok(void)
{
    const char *why;
    char t[64];
    if (!set_now(ed_key, !live(ed_key), &why)) {
        lv_snprintf(t, sizeof t, "Not changed: %s", why);
        ui_msg(t);
    }
}

static void edit(cfg_key_t k)
{
    static char t[96], cap[40], now[24], sv[24];
    ed_key = k;
    if (F[k].kind == K_TURNS) {
        // The keypad at once (0.17.0, Ryan 14 Nov: "just open a keypad to
        // type the number of turns, no question").
        int32_t lo, hi;
        cfg_range(CFG_POSMAX, &lo, &hi);
        ui_kp_t kp = { .title = "100 %, turns", .dec = 2,
                       .min = (int32_t)(((int64_t)lo * 100 + STEPS_PER_TURN - 1) / STEPS_PER_TURN),
                       .max = (int32_t)((int64_t)hi * 100 / STEPS_PER_TURN), .on_set = ed_turns };
        kp_rows(&kp, CFG_POSMAX, now, sv, sizeof now);
        ui_keypad_ex(&kp);
    } else if (F[k].kind == K_TOGGLE) {
        lv_snprintf(t, sizeof t, "Set motor direction to %s? This clears the zero.",
                    live(k) ? "Normal" : "Reversed");
        ui_confirm(t, "Change", ed_toggle_ok);
    } else {
        int32_t lo, hi;
        cfg_range(k, &lo, &hi);
        ui_kp_t kp = { .title = kp_title(cap, sizeof cap, k), .dec = F[k].dec, .min = lo, .max = hi,
                       .on_set = ed_set };
        kp_rows(&kp, k, now, sv, sizeof now);
        ui_keypad_ex(&kp);
    }
}

static void field_event(lv_event_t *e)
{
    edit((cfg_key_t)(intptr_t)lv_event_get_user_data(e));
}

/* --------------------------------------------------------------------------
 * Save, Undo and the side column (Help, Save, Undo)
 * ------------------------------------------------------------------------ */
static void save_now(void)
{
    static int32_t none_v[CFG_N];
    static bool none[CFG_N];
    const char *why;
    char t[96];
    uint32_t rev;
    const char *at;
    if (!cfg_unsaved()) {
        ui_msg("Nothing to save");
        return;
    }
    // In AUTO the valve moves all the time; tuning saves there.
    if (!valve_auto() && (stepper3_busy() || valve_mode() == VALVE_CALIBRATING)) {
        ui_msg("Wait for the valve to stop");
        return;
    }
    if (!cfg_save_with(none_v, none, &why)) {
        lv_snprintf(t, sizeof t, "Not saved: %s", why);
        ui_msg(t);
        return;
    }
    if (cfg_rev(CFG_FROM_SAVED, &rev, &at)) {
        char w[24];
        lv_snprintf(t, sizeof t, at[0] ? "Saved, %s." : "Saved, %s (clock not set).", save_when(w, sizeof w, at));
        ui_msg(t);
    }
}

static void undo_now(void)
{
    const char *why;
    char t[64];
    if (!cfg_undo(&why)) {
        lv_snprintf(t, sizeof t, "Not undone: %s", why);
        ui_msg(t);
        return;
    }
    ui_msg("Undone: back to the last save.");
}

static void undo_ask(void)
{
    static char t[96];
    char c[24];
    const int n = cfg_unsaved();
    if (!n) {
        ui_msg("Nothing to undo");
        return;
    }
    lv_snprintf(t, sizeof t, "Undo? %s in use %s back to the last save.", plural(c, sizeof c, n, "change"),
                n == 1 ? "goes" : "go");
    ui_confirm(t, "Undo", undo_now);
}

static void save_cb(lv_event_t *e) { (void)e; save_now(); }
static void undo_cb(lv_event_t *e) { (void)e; undo_ask(); }

// The header's unsaved icon (ui.c): Save is one tap from here.
void ui_unsaved_ask(void)
{
    static char t[96];
    char c[24];
    lv_snprintf(t, sizeof t, "%s in use, not saved: lost at a restart. Save?", plural(c, sizeof c, cfg_unsaved(), "change"));
    ui_confirm(t, "Save", save_now);
}

// Factory reset (the Menu's ninth tile).
static void reset_now(void)
{
    const char *why;
    char t[64];
    if (!cfg_reset_factory(&why)) {
        lv_snprintf(t, sizeof t, "Not reset: %s", why);
        ui_msg(t);
        return;
    }
    ui_msg("Factory settings saved.");
}

void ui_factory_reset_ask(void)
{
    if (valve_auto()) {
        ui_msg("Stop AUTO first");
        return;
    }
    if (stepper3_busy() || valve_mode() == VALVE_CALIBRATING) {
        ui_msg("Wait for the valve to stop");
        return;
    }
    ui_confirm("Reset motor, valve and PID settings to factory? Calibration and IP address stay.",
               "Reset", reset_now);
}

static const char *help_title, *help_text;
static void help_cb(lv_event_t *e)
{
    (void)e;
    ui_text_view(help_title, help_text);
}

static struct {
    lv_obj_t *save, *undo;
} side;

// Help in row 0, Save in row 1, Undo in row 2.
static void side_make(lv_obj_t *scr, const char *title, const char *help)
{
    help_title = title;
    help_text = help;
    ui_btn(scr, 361, RY[0], 109, RH, UI_NAV, "Help", help_cb, NULL);
    side.save = ui_btn(scr, 361, RY[1], 109, RH, UI_PRIMARY, "Save", save_cb, NULL);
    side.undo = ui_btn(scr, 361, RY[2], 109, RH, UI_SECONDARY, "Undo", undo_cb, NULL);
}

// The save state on the strip: "1 change in use, not saved", "All
// settings saved 14/11/26 13:02", "Factory settings, nothing saved yet".
static void save_strip(void)
{
    char t[64], c[32], w[24];
    uint32_t rev;
    const char *at;
    int n = cfg_unsaved();
    if (n)
        lv_snprintf(t, sizeof t, "%s in use, not saved", plural(c, sizeof c, n, "change"));
    else if (cfg_rev(CFG_FROM_SAVED, &rev, &at))
        lv_snprintf(t, sizeof t, "All settings saved%s%s", at[0] ? " " : "", at[0] ? save_when(w, sizeof w, at) : "");
    else
        lv_strlcpy(t, "Factory settings, nothing saved yet", sizeof t);
    ui_strip_set(UI_STRIP_INFO, n ? LV_SYMBOL_SAVE : LV_SYMBOL_OK, t);
}

// Save and Undo lit while something is in use and not saved; the strip.
static void side_refresh(void (*strip)(void))
{
    int n = cfg_unsaved();
    ui_btn_enable(side.save, n > 0);
    ui_btn_enable(side.undo, n > 0);
    (strip ? strip : save_strip)();
}

/* --------------------------------------------------------------------------
 * Restart (the Power question only, 0.17.0: no question of its own and no
 * button on System). Stops the valve (AUTO ends, homing is cancelled),
 * then waits 2.5 s so the valve position is saved at standstill
 * (valvepos.h, 2 s) and offered at start-up.
 * ------------------------------------------------------------------------ */
static void restart_now(lv_timer_t *t)
{
    // Something moved the valve meanwhile (the network, a tap): stop it and
    // wait again, so the position is saved at standstill.
    if (stepper3_busy() || valve_auto() || valve_mode() == VALVE_CALIBRATING) {
        valve_stop();
        lv_timer_reset(t);
        lv_timer_set_repeat_count(t, 1);
        return;
    }
    sysinfo_restart();
}

void ui_restart_now(void)
{
    valve_stop();
    valve_event("SYS", "Restart from the screen");
    ui_msg_no_ok("Restarting.");   // nothing to acknowledge (Ryan, 28 Oct)
    lv_timer_t *t = lv_timer_create(restart_now, 2500, NULL);
    lv_timer_set_repeat_count(t, 1);
}

// A key grid in the card style, keys raised as the buttons.
static lv_obj_t *key_grid(lv_obj_t *scr, int32_t w, const char *const *map, lv_event_cb_t cb)
{
    lv_obj_t *kb = lv_buttonmatrix_create(scr);
    lv_obj_set_pos(kb, 10, RY[0]);
    lv_obj_set_size(kb, w, 160);
    lv_buttonmatrix_set_map(kb, map);
    lv_obj_set_style_bg_opa(kb, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    lv_obj_set_style_radius(kb, 0, 0);
    lv_obj_set_style_pad_all(kb, 0, 0);
    lv_obj_set_style_pad_gap(kb, 6, 0);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, UI_CARD, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, lv_color_hex(0xE3E9EF), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_radius(kb, 6, LV_PART_ITEMS);
    lv_obj_set_style_border_side(kb, LV_BORDER_SIDE_BOTTOM, LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 2, LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 0, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_border_color(kb, lv_color_hex(0xAEB9C4), LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_text_font(kb, &ui_font_16, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, UI_TEXT, LV_PART_ITEMS);
    lv_obj_add_event_cb(kb, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return kb;
}

// The address and mask are saved as they are set (0.16.31), as is Network
// control. A save that fails says so; the header's unsaved icon then shows.
static void system_save(void)
{
    const char *why;
    char t[96];
    if (!cfg_save_system(&why)) {
        lv_snprintf(t, sizeof t, "In use, not saved: %s", why);
        ui_msg(t);
    }
}

// A switch on a card that takes the press (Network control, Ignore switch).
static lv_obj_t *card_switch(lv_obj_t *c, int32_t w, int32_t h)
{
    lv_obj_t *sw = lv_switch_create(c);
    lv_obj_set_size(sw, w, h);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_remove_flag(sw, LV_OBJ_FLAG_CLICKABLE);   // the card takes the press
    lv_obj_set_style_bg_color(sw, TRACK_GREY, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, UI_BLUE, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_anim_duration(sw, 0, 0);
    return sw;
}

static void switch_show(lv_obj_t *sw, bool on)
{
    if (lv_obj_has_state(sw, LV_STATE_CHECKED) != on)
        lv_obj_set_state(sw, LV_STATE_CHECKED, on);
}

/* --------------------------------------------------------------------------
 * System (0.17.0 layout; the canvas, Ryan 14 Nov):
 *   IP address & mask (its page)  MAC address
 *   Network control (toggle)      Date and time
 *   Firmware                      Up time        Power
 * The cryostat name went in 0.17.0 (Ryan 14 Nov: "remove the option to name
 * the controller just say Pressure controller"); Restart and Shut down are
 * in the Power question (the Power button here, the header's power icon).
 * Network control was Remote control (0.13.4 to 0.16.33).
 * ------------------------------------------------------------------------ */
static struct { lv_obj_t *net, *mac, *fw, *rsw, *rtext, *clock, *up; } nn;

static void nn_go(lv_event_t *e) { ui_open((const ui_screen_t *)lv_event_get_user_data(e)); }

static void remote_cb(lv_event_t *e)
{
    (void)e;
    cfg_set_remote(!cfg_remote());
    switch_show(nn.rsw, cfg_remote());
    system_save();
}

static void power_cb(lv_event_t *e)
{
    (void)e;
    ui_shutdown_ask();   // the power icon's question: Cancel, Restart, Shut down
}

static void nn_build(lv_obj_t *scr)
{
    memset(&nn, 0, sizeof nn);
    nn.net = ui_btn(scr, 10, RY[0], 226, RH, UI_NAV, "IP address & mask", nn_go, (void *)&ui_scr_netaddr);
    nn.mac = ui_info(scr, 244, RY[0], 226, RH, "MAC address");
    lv_obj_t *c = ui_card(scr, 10, RY[1], 226, RH);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c, remote_cb, LV_EVENT_PRESSED, NULL);
    ui_label(c, 12, 6, 150, &ui_font_16, UI_TEXT, LV_TEXT_ALIGN_LEFT, "Network control");
    nn.rtext = ui_label(c, 12, 26, 150, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT, "");
    nn.rsw = card_switch(c, 52, 28);
    nn.clock = ui_info(scr, 244, RY[1], 226, RH, "Date and time");
    nn.fw = ui_info(scr, 10, RY[2], 226, RH, "Firmware");
    nn.up = ui_info(scr, 244, RY[2], 109, RH, "Up time");
    ui_btn(scr, 361, RY[2], 109, RH, UI_DARK, LV_SYMBOL_POWER "  Power", power_cb, NULL);
}

// "192.168.1.100/24" from the address and mask in use.
static const char *net_short(char *t, size_t n)
{
    uint32_t ip, mask, m;
    char a[48];
    cfg_net(&ip, &mask);
    cfg_net_text(a, sizeof a, ip, mask);   // "a.b.c.d m.m.m.m[ (build default)]"
    char *sp = strchr(a, ' ');
    if (!sp) {
        lv_strlcpy(t, a, n);
        return t;
    }
    *sp = '\0';
    char *end = strchr(sp + 1, ' ');
    if (end)
        *end = '\0';
    int bits = 0;
    if (cfg_net_parse(sp + 1, &m))
        for (; m & 0x80000000u; m <<= 1)
            bits++;
    lv_snprintf(t, n, "%s/%d", a, bits);
    return t;
}

static void nn_refresh(void)
{
    char t[48];
    ui_btn_text(nn.net, "IP address & mask", net_short(t, sizeof t));
#if PMC_LINK_ETHERNET
    ui_set_text(nn.mac, ethlink_mac()[0] ? ethlink_mac() : "Not read");
#else
    ui_set_text(nn.mac, "None (host build)");
#endif
    ui_set_text(nn.fw, ui_fw_text());
    switch_show(nn.rsw, cfg_remote());
    ui_set_text(nn.rtext, cfg_remote() ? "On: network can set" : "Off: network read only");
    // "14/11/26 13:29:34" (DD/MM/YY); the clock is set by TIME SET.
    const char *iso = rtclock_iso();
    if (iso[0] && strlen(iso) >= 19)
        lv_snprintf(t, sizeof t, "%.2s/%.2s/%.2s %.8s", iso + 8, iso + 5, iso + 2, iso + 11);
    else
        lv_strlcpy(t, "Not set", sizeof t);
    ui_set_text(nn.clock, t);
    uint32_t s = lv_tick_get() / 1000;
    if (s >= 86400)
        lv_snprintf(t, sizeof t, "%lu d %lu h", (unsigned long)(s / 86400), (unsigned long)(s % 86400 / 3600));
    else if (s >= 3600)
        lv_snprintf(t, sizeof t, "%lu h %lu min", (unsigned long)(s / 3600), (unsigned long)(s % 3600 / 60));
    else
        lv_snprintf(t, sizeof t, "%lu min", (unsigned long)(s / 60));
    ui_set_text(nn.up, t);
    ui_strip_set(UI_STRIP_INFO, LV_SYMBOL_SETTINGS, cfg_net_restart_due()
                 ? "New IP address saved: restart to use it" : "Static IP address");   // no DHCP wording (Ryan 6 Nov)
}

const ui_screen_t ui_scr_namenet = { "System", nn_build, nn_refresh, NULL };

/* IP address & mask: digits and dots for the address and the mask; Use
 * saves them (0.16.31) for the next restart. */
static struct {
    lv_obj_t *kb, *box[2], *val[2], *use;
    char buf[2][16];
    int sel;
} na;

static const char *const na_map[] = {
    "1", "2", "3", "\n",
    "4", "5", "6", "\n",
    "7", "8", "9", "\n",
    ".", "0", LV_SYMBOL_BACKSPACE, ""
};

static void na_show(void)
{
    for (int i = 0; i < 2; i++) {
        ui_set_text(na.val[i], na.buf[i][0] ? na.buf[i] : "None");
        ui_set_color(na.val[i], na.buf[i][0] ? UI_TEXT : UI_MUTED);
        lv_obj_set_style_border_width(na.box[i], i == na.sel ? 2 : 0, 0);
    }
}

static void na_key(lv_event_t *e)
{
    (void)e;
    const char *k = lv_buttonmatrix_get_button_text(na.kb, lv_buttonmatrix_get_selected_button(na.kb));
    char *b = na.buf[na.sel];
    size_t n = strlen(b);
    if (!k)
        return;
    if (!strcmp(k, LV_SYMBOL_BACKSPACE)) {
        if (n)
            b[n - 1] = '\0';
    } else if (n < 15) {
        b[n] = k[0];
        b[n + 1] = '\0';
    }
    na_show();
}

static void na_pick(lv_event_t *e)
{
    na.sel = (int)(intptr_t)lv_event_get_user_data(e);
    na_show();
}

static void na_use(lv_event_t *e)
{
    (void)e;
    uint32_t ip, mask;
    const char *why;
    char t[48];
    if (!cfg_net_parse(na.buf[0], &ip) || !cfg_net_parse(na.buf[1], &mask)) {
        ui_msg("Type both as four numbers with dots, e.g. 192.168.1.50");
        return;
    }
    if (!cfg_set_net(ip, mask, &why)) {
        ui_msg(why);
        return;
    }
    cfg_net_text(t, sizeof t, ip, mask);
    valve_event("CFG", "IP address set to %s (screen)", t);
    ui_back();
    system_save();
}

static void na_default(lv_event_t *e)
{
    (void)e;
    const char *why;
    cfg_set_net(0, 0, &why);
    valve_event("CFG", "IP address set to the build default (screen)");
    ui_back();
    system_save();
}

static void na_build(lv_obj_t *scr)
{
    uint32_t ip, mask;
    char t[48];
    cfg_net(&ip, &mask);
    cfg_net_text(t, sizeof t, ip, mask);   // "a.b.c.d m.m.m.m[ (build default)]"
    char *sp = strchr(t, ' ');
    *sp = '\0';
    lv_strlcpy(na.buf[0], t, sizeof na.buf[0]);
    char *end = strchr(sp + 1, ' ');
    if (end)
        *end = '\0';
    lv_strlcpy(na.buf[1], sp + 1, sizeof na.buf[1]);
    na.sel = 0;
    na.kb = key_grid(scr, 226, na_map, na_key);
    static const char *const cap[2] = { "Address", "Mask" };
    for (int i = 0; i < 2; i++) {
        na.val[i] = ui_info(scr, 244, RY[i], 226, RH, cap[i]);
        na.box[i] = lv_obj_get_parent(na.val[i]);
        lv_obj_set_style_border_color(na.box[i], UI_TEXT, 0);
        lv_obj_add_flag(na.box[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(na.box[i], na_pick, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    na.use = ui_btn(scr, 244, RY[2], 109, RH, UI_PRIMARY, "Use", na_use, NULL);
    ui_btn(scr, 361, RY[2], 109, RH, UI_SECONDARY, "Default", na_default, NULL);
    na_show();
}

static void na_refresh(void)
{
    ui_strip_set(UI_STRIP_INFO, LV_SYMBOL_SETTINGS, "Use saves it. A restart puts it in use.");
}

const ui_screen_t ui_scr_netaddr = { "IP address & mask", na_build, na_refresh, NULL };

/* --------------------------------------------------------------------------
 * Tabbed pages: PID, Valve and Motor
 * ------------------------------------------------------------------------ */
typedef struct {
    const char *label;
    void (*make)(lv_obj_t *body);   // the tab's cards, rows 1 and 2
    void (*refresh)(void);          // its own values, every refresh (the setting cards refresh anyway)
    void (*strip)(void);            // its strip; NULL: the save state
    void (*fast)(void);             // 10 Hz, or NULL
} tab_t;

static struct {
    const tab_t *tabs;
    int ntabs, tab;
    lv_obj_t *scr, *body, *segb[4];
    ecard_t f[6];
    cfg_key_t fk[6];
    int nf;
    int set;   // the PID set the Gains tab shows, 1 to 4
} pg;

// A setting card on the tab.
static void fcard(lv_obj_t *b, int32_t x, int32_t y, int32_t w, const char *cap, cfg_key_t k)
{
    ecard_make(&pg.f[pg.nf], b, x, y, w, cap, field_event, (void *)(intptr_t)k);
    pg.fk[pg.nf++] = k;
}

// A grey track of n equal segments; the one picked raised white (tabs,
// the PID sets).
static void segs_make(lv_obj_t *p, int32_t y, int n, const char *const *labels, lv_obj_t **b,
                      lv_event_cb_t cb, int first)
{
    lv_obj_t *track = ui_card(p, 10, y, 343, RH);
    lv_obj_set_style_bg_color(track, TRACK_GREY, 0);
    lv_obj_set_style_border_width(track, 0, 0);
    lv_obj_set_style_radius(track, 6, 0);
    int32_t iw = (343 - 8) / n;
    for (int i = 0; i < n; i++) {
        b[i] = ui_card(track, 4 + i * iw, 4, iw, RH - 8);
        lv_obj_add_flag(b[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_radius(b[i], 4, 0);
        lv_obj_set_style_border_width(b[i], 0, 0);
        lv_obj_set_style_border_color(b[i], UI_TEXT, 0);
        lv_obj_set_style_shadow_width(b[i], 0, 0);
        lv_obj_add_event_cb(b[i], cb, LV_EVENT_CLICKED, (void *)(intptr_t)(first + i));
        lv_obj_t *l = ui_label(b[i], 0, 0, iw, &ui_font_16, UI_TEXT, LV_TEXT_ALIGN_CENTER, labels[i]);
        lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
    }
}

// on: the raised one; boxed: outlined dark (the PID set in use), 0 none.
static void segs_draw(lv_obj_t *const *b, int n, int on, int boxed)
{
    for (int i = 0; i < n; i++) {
        lv_opa_t o = i == on ? LV_OPA_COVER : LV_OPA_TRANSP;
        if (lv_obj_get_style_bg_opa(b[i], 0) != o)
            lv_obj_set_style_bg_opa(b[i], o, 0);
        int32_t bw = i + 1 == boxed ? 2 : 0;
        if (lv_obj_get_style_border_width(b[i], 0) != bw)
            lv_obj_set_style_border_width(b[i], bw, 0);
        ui_set_color(lv_obj_get_child(b[i], 0), i == on || i + 1 == boxed ? UI_TEXT : UI_MUTED);
    }
}

static void pg_refresh(void);

static void pg_show(int i)
{
    if (pg.body)
        lv_obj_delete(pg.body);   // the last tab's cards (and the jog row's timer)
    pg.tab = i;
    pg.nf = 0;
    memset(pg.f, 0, sizeof pg.f);
    lv_obj_t *b = pg.body = lv_obj_create(pg.scr);
    lv_obj_remove_style_all(b);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(b, 0, 0);
    lv_obj_set_size(b, 353, 216);
    segs_draw(pg.segb, pg.ntabs, pg.tab, 0);
    pg.tabs[i].make(b);
    pg_refresh();   // the values now, not at the next 0.5 s refresh (Ryan, 28 Oct)
}

static void tab_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i != pg.tab)
        pg_show(i);
}

static void pg_build(lv_obj_t *scr, const char *title, const tab_t *tabs, int n, const char *help)
{
    memset(&pg, 0, sizeof pg);
    pg.scr = scr;
    pg.tabs = tabs;
    pg.ntabs = n;
    pg.set = valve_pid_set();   // the Gains tab opens on the set in use
    const char *labels[4];
    for (int i = 0; i < n; i++)
        labels[i] = tabs[i].label;
    segs_make(scr, RY[0], n, labels, pg.segb, tab_cb, 0);
    side_make(scr, title, help);
    pg_show(0);
}

static void pg_refresh(void)
{
    for (int i = 0; i < pg.nf; i++)
        field_update(&pg.f[i], pg.fk[i]);
    const tab_t *t = &pg.tabs[pg.tab];
    if (t->refresh)
        t->refresh();
    side_refresh(t->strip);
}

static void pg_fast(void)
{
    const tab_t *t = &pg.tabs[pg.tab];
    if (t->fast)
        t->fast();
}

/* A bar card across row 1 (PID Ranges, Valve Ignore): a grey track with
 * markers, labels over it and the ends under it. */
typedef struct {
    lv_obj_t *card, *track, *fill, *mark[3], *top[4], *lo, *hi, *mlab[2];
} bar_t;

#define BAR_X 12
#define BAR_W 319

static void bar_make(bar_t *b, lv_obj_t *p, int ntop, int nmark, bool fill, bool mlab)
{
    b->card = ui_card(p, 10, RY[1], 343, RH);
    b->track = ui_card(b->card, BAR_X, 22, BAR_W, 6);
    lv_obj_set_style_bg_color(b->track, TRACK_GREY, 0);
    lv_obj_set_style_border_width(b->track, 0, 0);
    lv_obj_set_style_radius(b->track, 3, 0);
    if (fill) {
        b->fill = ui_card(b->card, BAR_X, 22, 0, 6);
        lv_obj_set_style_bg_color(b->fill, UI_FILL, 0);
        lv_obj_set_style_border_width(b->fill, 0, 0);
        lv_obj_set_style_radius(b->fill, 0, 0);
    }
    for (int i = 0; i < nmark; i++) {
        b->mark[i] = ui_card(b->card, BAR_X, 20, 2, 10);
        lv_obj_set_style_bg_color(b->mark[i], UI_TEXT, 0);
        lv_obj_set_style_border_width(b->mark[i], 0, 0);
        lv_obj_set_style_radius(b->mark[i], 0, 0);
    }
    for (int i = 0; i < ntop; i++) {
        b->top[i] = ui_label(b->card, 0, 3, 0, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_CENTER, "");
        lv_obj_set_width(b->top[i], LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(b->top[i], 3, 0);
        lv_obj_set_style_border_color(b->top[i], UI_TEXT, 0);
        lv_obj_set_style_radius(b->top[i], 2, 0);
    }
    b->lo = ui_label(b->card, BAR_X - 2, 31, 60, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT, "");
    b->hi = ui_label(b->card, BAR_X + BAR_W - 60, 31, 62, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_RIGHT, "");
    for (int i = 0; i < 2 && mlab; i++) {
        b->mlab[i] = ui_label(b->card, 0, 31, 0, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_CENTER, "");
        lv_obj_set_width(b->mlab[i], LV_SIZE_CONTENT);
    }
}

static int32_t bar_x(float v, float max)
{
    if (v < 0) v = 0;
    if (v > max) v = max;
    return BAR_X + (int32_t)(v / max * (float)(BAR_W - 2) + 0.5f);
}

static void show_obj(lv_obj_t *o, bool on)
{
    if (on == lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
        if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

// A label centred on x, kept inside the card.
static void label_at(lv_obj_t *l, const char *text, int32_t x, bool show)
{
    ui_set_text(l, text);
    show_obj(l, show);
    if (!show)
        return;
    lv_obj_update_layout(l);
    int32_t w = lv_obj_get_width(l), lx = x - w / 2;
    if (lx < 4) lx = 4;
    if (lx + w > 339) lx = 339 - w;
    if (lv_obj_get_x(l) != lx)
        lv_obj_set_x(l, lx);
}

/* --------------------------------------------------------------------------
 * PID (PID tuning 0.14.21 to 0.16.33): Gains | Ranges | AUTO.
 * Four PID sets by the pressure (0.15.15; four from 0.16.14, the table
 * always on from 0.16.20). Gains shows one set; the set row is a track of
 * four, the set shown raised, the set in use boxed (0.17.0, Ryan 14 Nov:
 * "make a rectangle around the set x instead of having the dot").
 * ------------------------------------------------------------------------ */
static struct {
    lv_obj_t *sb[4];
    bar_t bar;
} pid;

static void set_cb(lv_event_t *e)
{
    int s = (int)(intptr_t)lv_event_get_user_data(e);
    if (s != pg.set) {
        pg.set = s;
        pg_show(pg.tab);
    }
}

static void gains_make(lv_obj_t *b)
{
    static const char *const sets[4] = { "Set 1", "Set 2", "Set 3", "Set 4" };
    static const char *const caps[3] = { "KP, %/mB", "TI, min", "KD, %s/mB" };
    segs_make(b, RY[1], 4, sets, pid.sb, set_cb, 1);
    for (int i = 0; i < 3; i++)
        fcard(b, 10 + i * 117, RY[2], 109, caps[i], cfg_pid_key(pg.set, i));
}

static void gains_refresh(void)
{
    segs_draw(pid.sb, 4, pg.set - 1, valve_pid_set());
}

static void ranges_make(lv_obj_t *b)
{
    bar_make(&pid.bar, b, 4, 3, false, false);
    static const cfg_key_t k[3] = { CFG_B1, CFG_B2, CFG_B3 };
    char cap[24];
    for (int i = 0; i < 3; i++)
        fcard(b, 10 + i * 117, RY[2], 109, caption(cap, sizeof cap, k[i]), k[i]);
}

/* Where the Ranges bar draws its boundaries: x[0] to x[n], n ranges. To
 * scale, except that every range keeps at least SEG_MIN px for its "Set n"
 * label (0.17.0, Ryan 14 Nov: with boundaries 2, 5 and 12 below a fourth at
 * 900 mB the labels ran together). A range under that gets SEG_MIN and the
 * rest share what is left, to scale. */
#define SEG_MIN 44

static void ranges_x(const float *edge, int n, int32_t *x)
{
    const int32_t W = BAR_W - 2;
    bool fixed[4] = { false, false, false, false };
    int32_t w[4] = { 0, 0, 0, 0 };
    for (int pass = 0; pass < 4; pass++) {
        int nf = 0;
        float sum = 0;
        for (int i = 0; i < n; i++) {
            if (fixed[i]) nf++;
            else sum += edge[i + 1] - edge[i];
        }
        int32_t room = W - nf * SEG_MIN;
        bool again = false;
        for (int i = 0; i < n; i++) {
            if (fixed[i]) {
                w[i] = SEG_MIN;
                continue;
            }
            float len = edge[i + 1] - edge[i];
            w[i] = sum > 0 ? (int32_t)((float)room * len / sum + 0.5f) : room / (n - nf);
            if (w[i] < SEG_MIN) {
                fixed[i] = true;
                again = true;
            }
        }
        if (!again)
            break;
    }
    x[0] = BAR_X;
    for (int i = 0; i < n; i++)
        x[i + 1] = i + 1 == n ? BAR_X + W : x[i] + w[i];   // the last ends at the bar's end
}

static void ranges_refresh(void)
{
    // 0 to 20 mB, or past the last boundary when it is above 16 mB.
    float bnd[3] = { live(CFG_B1) / 10.0f, live(CFG_B2) / 10.0f, live(CFG_B3) / 10.0f };
    bool four = live(CFG_B3) != 0;
    float top = four ? bnd[2] : bnd[1], max = 20;
    if (top > 16)
        max = (float)(((int)(top + 4) + 4) / 5 * 5);
    char t[16];
    lv_snprintf(t, sizeof t, "%d mB", (int)max);
    ui_set_text(pid.bar.hi, t);
    ui_set_text(pid.bar.lo, "0");
    int use = valve_pid_set(), n = four ? 4 : 3;
    float edge[5] = { 0, bnd[0], bnd[1], four ? bnd[2] : max, max };
    int32_t x[5];
    ranges_x(edge, n, x);
    for (int i = 0; i < 3; i++) {
        bool on = i < 2 || four;
        show_obj(pid.bar.mark[i], on);
        if (on && lv_obj_get_x(pid.bar.mark[i]) != x[i + 1] - 1)
            lv_obj_set_x(pid.bar.mark[i], x[i + 1] - 1);
    }
    for (int i = 0; i < 4; i++) {
        char s[8];
        lv_snprintf(s, sizeof s, "Set %d", i + 1);
        label_at(pid.bar.top[i], s, i < n ? (x[i] + x[i + 1]) / 2 : 0, i < n);
        int32_t bw = i + 1 == use ? 1 : 0;
        if (lv_obj_get_style_border_width(pid.bar.top[i], 0) != bw)
            lv_obj_set_style_border_width(pid.bar.top[i], bw, 0);
        ui_set_color(pid.bar.top[i], i + 1 == use ? UI_TEXT : UI_MUTED);
    }
}

static void auto_make(lv_obj_t *b)
{
    fcard(b, C2X[0], RY[1], C2W[0], "AUTO min, %", CFG_OUTMIN);
    fcard(b, C2X[1], RY[1], C2W[1], "SP band, mB", CFG_BAND);
    fcard(b, C2X[0], RY[2], C2W[0], "Hysteresis, %", CFG_HYST);
    fcard(b, C2X[1], RY[2], C2W[1], "Deadband, steps", CFG_DB);
}

static const tab_t pid_tabs[3] = {
    { "Gains", gains_make, gains_refresh, NULL, NULL },
    { "Ranges", ranges_make, ranges_refresh, NULL, NULL },
    { "Settings", auto_make, NULL, NULL, NULL },   // AUTO on the canvas to v38 (Ryan 14 Nov)
};

static void pid_build(lv_obj_t *scr)
{
    memset(&pid, 0, sizeof pid);
    pg_build(scr, "PID", pid_tabs, 3,
             "Gains: KP, TI and KD of each set. Box = set in use.\n\n"
             "Ranges: the pressure where each set takes over.\n\n"
             "Settings: lowest valve %, setpoint band, hysteresis and deadband.");
}

const ui_screen_t ui_scr_tune = { "PID", pid_build, pg_refresh, NULL };

/* --------------------------------------------------------------------------
 * Valve (0.17.0; Valve position and Motor settings' Travel, Homing and
 * Override tabs before): Position | Homing | Ignore. What is in % and the
 * limit switch: the position, 100 %, homing and the Ignore range.
 * ------------------------------------------------------------------------ */
static struct {
    lv_obj_t *pos, *ref, *ref_cap, *setas, *home;
    bar_t bar;
    lv_obj_t *sw, *sw_v;
} vv;

static void setas_cb(lv_event_t *e)
{
    if (ui_btn_disabled(e)) {
        ui_msg(valve_mode() == VALVE_CALIBRATING ? "Homing: STOP to cancel"
               : valve_auto() ? "Stop AUTO first" : "Reset the fault first");
        return;
    }
    ui_set_position_ask();   // the Set as keypad (ui_home.c)
}

static void home_cb(lv_event_t *e)
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

static void vpos_make(lv_obj_t *b)
{
    vv.pos = ui_info(b, 10, RY[1], 109, RH, "Position, %");
    fcard(b, 127, RY[1], 109, "100 %, turns", CFG_POSMAX);
    vv.ref = ui_info(b, 244, RY[1], 109, RH, "Last saved, %");
    vv.ref_cap = lv_obj_get_child(lv_obj_get_parent(vv.ref), 0);
    vv.setas = ui_btn(b, 10, RY[2], 167, RH, UI_SECONDARY, "Set as %", setas_cb, NULL);
    ui_label(vv.setas, 167 - 24, 15, 0, &lv_font_montserrat_14, UI_IDLE, LV_TEXT_ALIGN_LEFT, LV_SYMBOL_RIGHT);
    vv.home = ui_btn(b, 185, RY[2], 168, RH, UI_PRIMARY, "Home valve", home_cb, NULL);
}

static void vpos_fast(void)
{
    char t[16];
    bool have = valve_calibrated() && valve_mode() != VALVE_CALIBRATING;
    ui_set_text(vv.pos, have ? ui_fmt2(t, sizeof t, valve_pct_milli()) : "----");
}

static void vpos_refresh(void)
{
    char cap[24], v[24];
    bool calib = valve_mode() == VALVE_CALIBRATING;
    valve_ref_t how = valve_reference();
    uint32_t age = 0, took = 0;
    bool ran = valve_last_cal(&age, &took);
    const char *iso = valve_last_cal_iso();
    if (!calib && how == VALVE_REF_HOMED && ran && iso[0] && strlen(iso) >= 16) {
        // "Homed, 14:01" over "24/10/26".
        lv_snprintf(cap, sizeof cap, "Homed, %.5s", iso + 11);
        lv_snprintf(v, sizeof v, "%.2s/%.2s/%.2s", iso + 8, iso + 5, iso + 2);
    } else if (!calib && how == VALVE_REF_HOMED) {
        lv_strlcpy(cap, "Homed", sizeof cap);
        if (ran)
            ui_when(v, sizeof v, age);
        else
            lv_strlcpy(v, "Yes", sizeof v);
    } else if (!calib && how == VALVE_REF_HAND) {
        lv_strlcpy(cap, "Not homed", sizeof cap);
        lv_strlcpy(v, "Set by hand", sizeof v);
    } else if (!calib && how == VALVE_REF_PARKED) {
        lv_strlcpy(cap, "Homed", sizeof cap);
        lv_strlcpy(v, "Before shut down", sizeof v);
    } else {
        int32_t q;
        lv_strlcpy(cap, "Last saved, %", sizeof cap);
        if (valvepos_offer(&q)) {
            int32_t pm = valve_pos_max();
            ui_fmt2(v, sizeof v, ((int64_t)q * 100000 + (q >= 0 ? pm / 2 : -pm / 2)) / pm);
        } else {
            lv_strlcpy(v, "None", sizeof v);
        }
    }
    ui_set_text(vv.ref_cap, cap);
    ui_set_text(vv.ref, v);
    vpos_fast();
    ui_btn_text(vv.home, calib ? "Homing" : "Home valve", NULL);
    ui_btn_enable(vv.home, !calib && !valve_motion_blocked() && !valve_auto());
    ui_btn_enable(vv.setas, !calib && !valve_motion_blocked() && !valve_auto());
}

// "0.00 turns. Valve idle" once homed; else the status line.
static void vpos_strip(void)
{
    valve_mode_t m = valve_mode();
    if (m == VALVE_MANUAL && valve_calibrated() && !stepper3_busy() && !valve_warning()) {
        char t[48], b[16];
        int64_t full = (int64_t)stepper3_position();
        lv_snprintf(t, sizeof t, "%s turns. Valve idle",
                    fixed_fmt(b, sizeof b, fixed_round(full * 1000 / STEPS_PER_TURN, 1), 2));
        ui_strip_set(UI_STRIP_INFO, UI_ICON_VALVE, t);
    } else {
        ui_status_line();
    }
}

static void vhome_make(lv_obj_t *b)
{
    fcard(b, C2X[0], RY[1], C2W[0], "Fast close, steps/s", CFG_FHOME);
    fcard(b, C2X[1], RY[1], C2W[1], "Slow close, steps/s", CFG_FCREEP);
    fcard(b, C2X[0], RY[2], C2W[0], "Back-off, steps", CFG_BACKOFF);
    fcard(b, C2X[1], RY[2], C2W[1], "Return, steps", CFG_RETURN);
}

// Ignore switch (Disable switch on Motor settings > Override 0.16.19 to
// 0.16.33; renamed 0.17.0, Ryan 14 Nov): while On and homed, the limit
// switch does not stop closing between From and To % open. A press
// anywhere on the card turns it On or Off, at once; Off after a restart.
static void ignore_cb(lv_event_t *e)
{
    (void)e;
    valve_limit_band_set(!valve_limit_band());
    pg_refresh();
}

static void vign_make(lv_obj_t *b)
{
    bar_make(&vv.bar, b, 1, 2, true, true);
    fcard(b, 10, RY[2], 84, "From, %", CFG_PASS_LO);
    fcard(b, 102, RY[2], 84, "To, %", CFG_PASS_HI);
    lv_obj_t *c = ui_card(b, 194, RY[2], 159, RH);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c, ignore_cb, LV_EVENT_PRESSED, NULL);
    ui_label(c, 11, 5, 100, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT, "Ignore switch");
    vv.sw_v = ui_label(c, 11, 22, 60, &ui_font_16, UI_TEXT, LV_TEXT_ALIGN_LEFT, "");
    vv.sw = card_switch(c, 44, 24);
}

static void vign_refresh(void)
{
    bool on = valve_limit_band();
    int lo = valve_pass_lo(), hi = valve_pass_hi();
    switch_show(vv.sw, on);
    ui_set_text(vv.sw_v, on ? "On" : "Off");
    int32_t xl = bar_x((float)lo, 100), xh = bar_x((float)hi, 100);
    lv_obj_set_x(vv.bar.mark[0], xl - 1);
    lv_obj_set_x(vv.bar.mark[1], xh - 1);
    lv_obj_set_x(vv.bar.fill, xl);
    lv_obj_set_width(vv.bar.fill, xh - xl > 0 ? xh - xl : 0);
    lv_opa_t fo = on ? LV_OPA_COVER : LV_OPA_TRANSP;
    if (lv_obj_get_style_bg_opa(vv.bar.fill, 0) != fo)
        lv_obj_set_style_bg_opa(vv.bar.fill, fo, 0);
    label_at(vv.bar.top[0], on ? "Ignored" : "Ignore range", (xl + xh) / 2, true);
    ui_set_color(vv.bar.top[0], on ? UI_TEXT : UI_MUTED);
    char a[8], b[8];
    lv_snprintf(a, sizeof a, "%d", lo);
    lv_snprintf(b, sizeof b, "%d", hi);
    label_at(vv.bar.mlab[0], a, xl, true);
    label_at(vv.bar.mlab[1], b, xh, hi < 90);
    ui_set_text(vv.bar.hi, "100 %");
}

static void vign_strip(void)
{
    char t[64];
    limit_state_t lim = limit_state();
    const char *ls = lim == LIMIT_CLOSED ? "Switch released" : lim == LIMIT_OPEN ? "Switch pressed" : "Switch not read yet";
    if (valve_limit_band())
        lv_snprintf(t, sizeof t, "%s. Ignored from %d to %d %%", ls, valve_pass_lo(), valve_pass_hi());
    else
        lv_strlcpy(t, ls, sizeof t);
    ui_strip_set(UI_STRIP_INFO, UI_ICON_VALVE, t);
}

static const tab_t valve_tabs[3] = {
    { "Position", vpos_make, vpos_refresh, vpos_strip, vpos_fast },
    { "Homing", vhome_make, NULL, NULL, NULL },
    { "Ignore", vign_make, vign_refresh, vign_strip, NULL },
};

static void valve_build(lv_obj_t *scr)
{
    memset(&vv, 0, sizeof vv);
    pg_build(scr, "Valve", valve_tabs, 3,
             "Position: 0 % is the limit switch, 100 % the turns set here. Set as types in the position.\n\n"
             "Homing: speeds and steps used to find the switch.\n\n"
             "Ignore: the limit switch is ignored between From and To.");
}

const ui_screen_t ui_scr_valve_set = { "Valve", valve_build, pg_refresh, pg_fast };

/* --------------------------------------------------------------------------
 * Motor (0.17.0; Motor override and Motor settings' Drive tab before): Jog
 * | Settings. What is in steps: jogging, speeds, hold torque, direction.
 * ------------------------------------------------------------------------ */
static struct {
    ecard_t hold;
} mo;

static void hold_cb(lv_event_t *e)
{
    (void)e;
    if (stepper3_busy()) {
        ui_msg("Wait for the valve to stop");
        return;
    }
    (void)stepper3_set_hold(!stepper3_hold());
    valve_event("CFG", "Hold torque %s (screen), in use, not saved", stepper3_hold() ? "on" : "off");
    pg_refresh();
}

static void mjog_make(lv_obj_t *b) { motor_jog_make(b); }

static void mset_make(lv_obj_t *b)
{
    fcard(b, 10, RY[1], 109, "Speed, st/s", CFG_RATE);
    fcard(b, 127, RY[1], 109, "Start, st/s", CFG_START);
    fcard(b, 244, RY[1], 109, "Accel, st/s/s", CFG_ACCEL);
    ecard_make(&mo.hold, b, 10, RY[2], 109, "Hold torque", hold_cb, NULL);
    // Steps per output turn, a setting (0.17.0, in place of Coil order;
    // Ryan 14 Nov "make this a changeable value"): the turns shown.
    fcard(b, 127, RY[2], 109, "Steps/turn", CFG_SPT);
    fcard(b, 244, RY[2], 109, "Direction", CFG_REVERSE);
}

static void mset_refresh(void)
{
    ecard_state(&mo.hold, stepper3_hold() ? "On" : "Off", false);
}

static void mset_strip(void)
{
    char t[64], b[24];
    lv_snprintf(t, sizeof t, "Full travel %s at this speed", motor_travel_text(b, sizeof b));
    ui_strip_set(UI_STRIP_INFO, UI_ICON_VALVE, t);
}

static const tab_t motor_tabs[2] = {
    { "Jog", mjog_make, NULL, motor_jog_strip, motor_jog_fast },
    { "Settings", mset_make, mset_refresh, mset_strip, NULL },
};

static void motor_build(lv_obj_t *scr)
{
    memset(&mo, 0, sizeof mo);
    pg_build(scr, "Motor", motor_tabs, 2,
             "Jog: hold Close or Open to move. To close past the switch, hold Limit override, then Close.\n\n"
             "Settings: speeds, hold torque and direction of the stepper. Steps per turn: motor steps for one output turn.");
}

const ui_screen_t ui_scr_motor = { "Motor", motor_build, pg_refresh, pg_fast };

/* --------------------------------------------------------------------------
 * Pressure sensor (Sensor calibration 0.16.27 to 0.16.33): CAL VAC / CAL
 * AIR on the screen. Row 1 the signal, the pressure and the calibration in
 * use (Nominal: the datasheet line, 0 to 10 mV/V for 0 to 1034.2 mB; Ryan
 * 14 Nov "nominal", Factory before), then a wide card for each point with
 * Capture on it. A new calibration is in use once both points are held,
 * not saved until Save (Undo puts the saved one back).
 * ------------------------------------------------------------------------ */
static struct {
    lv_obj_t *sig, *now, *calv;
    lv_obj_t *card[2], *sub[2], *cap[2], *edge[2];
} cs;

static bool reading_ok(void)
{
    const clicks_state_t *s = clicks_state();
    return s->pm_found && s->reading_ok && !s->boost_tripped;
}

static void cs_start(int what, int32_t p_mmbar)
{
    const char *why;
    char t[80];
    if (!cal_capture(what, p_mmbar, &why)) {
        lv_snprintf(t, sizeof t, "Not started: %s", why);
        ui_msg(t);
    }
}

static void cs_vac0(void) { cs_start(1, 0); }

static bool cs_vac_set(int32_t v100, const char **why)
{
    (void)why;
    cs_start(1, v100 * 10);
    return true;
}

static const char *signal_now(char *t, size_t n)
{
    char b[16];
    lv_snprintf(t, n, "Signal now %s mV/V",
                reading_ok() ? fixed_fmt(b, sizeof b, fixed_round(clicks_state()->r_ppb, 2), 4) : "----");
    return t;
}

static void cs_vac_typed(void)
{
    static char info[40];
    ui_kp_t k = { .title = "Vacuum point, mB", .info = signal_now(info, sizeof info), .dec = 2,
                  .min = 0, .max = 5000, .on_set = cs_vac_set };
    ui_keypad_ex(&k);
}

static bool cs_air_set(int32_t v10, const char **why)
{
    (void)why;
    cs_start(2, v10 * 100);
    return true;
}

static void cs_air_typed(void)
{
    static char info[40];
    ui_kp_t k = { .title = "Air point, mB", .info = signal_now(info, sizeof info), .dec = 1,
                  .min = 5000, .max = 11000, .on_set = cs_air_set };
    ui_keypad_ex(&k);
}

static void cs_air_baro(void) { cs_start(2, CAL_P_BARO); }

static void point_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (!reading_ok() || cal_capturing()) {
        ui_msg(cal_capturing() ? "A capture is running" : "No pressure signal");
        return;
    }
    static char q[96];
    if (i == 0) {
        ui_ask3("Sensor pumped down? Capture sets this point to 0 mB.",
               "Cancel", NULL, "Enter value", cs_vac_typed, "Capture", cs_vac0);
        return;
    }
    // With the Pressure 21 fitted the room pressure is read (0.14.0); Enter
    // value keeps the lab barometer possible.
    const clicks_state_t *s = clicks_state();
    if (!s->baro_ok) {
        cs_air_typed();
        return;
    }
    char v[16];
    lv_snprintf(q, sizeof q, "Sensor open to room air? Capture sets this point to %s mB.",
                ui_fmt1(v, sizeof v, s->baro_mmbar));
    ui_ask3(q, "Cancel", NULL, "Enter value", cs_air_typed, "Capture", cs_air_baro);
}

static void cs_build(lv_obj_t *scr)
{
    memset(&cs, 0, sizeof cs);
    cs.now = ui_info(scr, 10, RY[0], 109, RH, "Pressure, mB");   // Pressure first (Ryan 14 Nov)
    cs.sig = ui_info(scr, 127, RY[0], 109, RH, "Signal, mV/V");
    cs.calv = ui_info(scr, 244, RY[0], 109, RH, "Calibration");
    static const char *const names[2] = { "Vacuum point", "Air point" };
    for (int i = 0; i < 2; i++) {
        lv_obj_t *c = cs.card[i] = ui_card(scr, 10, RY[1 + i], 343, RH);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_border_width(c, 1, 0);
        lv_obj_set_style_border_color(c, UI_TICK, 0);
        lv_obj_set_style_bg_color(c, UI_TRACK, LV_STATE_PRESSED);
        lv_obj_add_event_cb(c, point_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        ui_label(c, 12, 5, 200, &ui_font_16, UI_TEXT, LV_TEXT_ALIGN_LEFT, names[i]);
        cs.sub[i] = ui_label(c, 12, 27, 240, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT, "");
        cs.cap[i] = ui_label(c, 343 - 110, 14, 80, &ui_font_16, UI_BLUE, LV_TEXT_ALIGN_RIGHT, "Capture");
        ui_label(c, 343 - 22, 15, 0, &lv_font_montserrat_14, UI_IDLE, LV_TEXT_ALIGN_LEFT, LV_SYMBOL_RIGHT);
        lv_obj_t *e = cs.edge[i] = ui_card(c, -1, 5, 3, 36);
        lv_obj_set_style_radius(e, 0, 0);
        lv_obj_set_style_border_width(e, 0, 0);
        lv_obj_set_style_bg_color(e, COL_EDGE, 0);
        lv_obj_add_flag(e, LV_OBJ_FLAG_HIDDEN);
    }
    side_make(scr, "Pressure sensor",
              "Vacuum point: pump the sensor down, then Capture.\n\n"
              "Air point: open it to room air, then Capture. The on-board barometer is the reference.\n\n"
              "Save keeps them. Undo restores the saved ones.");
}

// A point new since the save: not the saved one.
static bool pt_new(const cal_rec_t *saved, int i, const cal_point_t *p)
{
    if (!p->have)
        return false;
    if (!saved)
        return true;
    return saved->r_ppb[i] != p->r_ppb || saved->p_mmbar[i] != p->p_mmbar;
}

static void cs_strip(void)
{
    char t[64], w[24];
    uint32_t rev;
    const char *at;
    const cal_rec_t *saved = NULL;
    bool in_save = cfg_saved_cal(&saved);
    if (!cal_saved())
        ui_strip_set(UI_STRIP_INFO, LV_SYMBOL_SAVE, "New calibration in use, not saved");
    else if (!saved)
        ui_strip_set(UI_STRIP_INFO, LV_SYMBOL_OK, in_save ? "Nominal calibration" : "Nominal calibration, nothing saved yet");
    else {
        if (cfg_rev(CFG_FROM_SAVED, &rev, &at) && at[0])
            lv_snprintf(t, sizeof t, "Calibration saved %s", save_when(w, sizeof w, at));
        else
            lv_strlcpy(t, "Calibration saved", sizeof t);
        ui_strip_set(UI_STRIP_INFO, LV_SYMBOL_OK, t);
    }
}

static void cs_refresh(void)
{
    char t[48], a[16], b[16];
    const clicks_state_t *s = clicks_state();
    bool ok = reading_ok();
    ui_set_text(cs.sig, ok ? fixed_fmt(t, sizeof t, fixed_round(s->r_ppb, 2), 4) : "----");
    ui_set_text(cs.now, ok ? ui_fmt1(t, sizeof t, s->p_mmbar) : "----");
    const cal_rec_t *saved = NULL;
    (void)cfg_saved_cal(&saved);
    bool as_saved = cal_saved();
    ui_set_text(cs.calv, !as_saved ? "Not saved" : saved ? "Saved" : "Nominal");
    cal_point_t pt[2];
    cal_points(&pt[0], &pt[1]);
    int cap = cal_capturing(), left = cal_capture_left();
    for (int i = 0; i < 2; i++) {
        if (cap == i + 1)
            lv_snprintf(t, sizeof t, "Capturing %d of %d", CAL_CAPTURE_N - left, CAL_CAPTURE_N);
        else if (pt[i].have)
            lv_snprintf(t, sizeof t, "%s mB at %s mV/V", fixed_fmt(a, sizeof a, fixed_round(pt[i].p_mmbar, 2), 1),
                        fixed_fmt(b, sizeof b, fixed_round(pt[i].r_ppb, 2), 4));
        else if (i == 1 && s->baro_ok)
            lv_snprintf(t, sizeof t, "Not captured. Room %s mB", ui_fmt1(a, sizeof a, s->baro_mmbar));
        else
            lv_strlcpy(t, "Not captured", sizeof t);
        ui_set_text(cs.sub[i], t);
        bool can = ok && !cap;
        ui_set_color(cs.cap[i], can ? UI_BLUE : UI_IDLE);
        show_obj(cs.edge[i], !as_saved && cap != i + 1 && pt_new(saved, i, &pt[i]));
    }
    side_refresh(cs_strip);
}

const ui_screen_t ui_scr_sensor_cal = { "Pressure sensor", cs_build, cs_refresh, NULL };
