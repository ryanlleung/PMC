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
#include "fixed.h"
#include "stepper3.h"
#include "valve.h"
#include "ui.h"

/* --------------------------------------------------------------------------
 * The settings pages (stage 6, simulator v63), each on the Menu from 0.16.2
 * (Ryan 9 Nov: "remove the settings page and put everything in the menu"):
 * System (with Cryostat name and IP address), Motor settings, PID tuning,
 * Review and save, and Calibrate sensor. The Settings page of navigation
 * tiles (0.15.9 to 0.16.1) and Choose sensor (one sensor per board, Ryan 9
 * Nov: "a new one will be recalibrated anyway") are gone.
 *
 * An edit is a draft (dark edge) until Save settings, which
 * checks the drafts and the settings in use together, puts them in use all
 * or nothing, and writes the whole record (cfg_save_with).
 * A grey edge is a setting in use that differs from the last save (a
 * capture, a COM3 command or Load settings). Load settings > Previous save
 * is the undo.
 * ------------------------------------------------------------------------ */

#define COL_DRAFT  UI_TEXT   // dark edge, no colour (Ryan, 24 Oct; was violet)
#define COL_GREY   UI_IDLE   // light, apart from the draft's dark edge
#define STEPS_PER_TURN STEPPER3_STEPS_PER_TURN

static const int32_t RY[4] = { 48, 104, 160, 216 };
#define RH 48
// Two columns inside 10-353, as cols(2) in the simulator.
static const int32_t C2X[2] = { 10, 186 }, C2W[2] = { 168, 167 };

/* --------------------------------------------------------------------------
 * Drafts
 * ------------------------------------------------------------------------ */
static int32_t draft_v[CFG_N];
static bool draft_set[CFG_N];

static int drafts(void)
{
    int n = 0;
    for (int k = 0; k < CFG_N; k++)
        n += draft_set[k];
    return n;
}

static int32_t live(cfg_key_t k)
{
    int32_t v = 0;
    cfg_value(CFG_FROM_LIVE, k, &v);
    return v;
}

static int32_t shown(cfg_key_t k)
{
    return draft_set[k] ? draft_v[k] : live(k);
}

// A draft equal to the value in use is no draft.
static void set_draft(cfg_key_t k, int32_t v)
{
    draft_set[k] = v != live(k);
    draft_v[k] = v;
}

static void drafts_clear(void)
{
    memset(draft_set, 0, sizeof draft_set);
}

// Unsaved changes: drafts plus settings in use that differ from the save.
static int unsaved(void)
{
    int n = drafts();
    int other = cfg_unsaved();
    for (int k = 0; k < CFG_N; k++) {
        int32_t s;
        // A draft that puts a changed setting back to its saved value.
        if (draft_set[k] && cfg_value(CFG_FROM_SAVED, k, &s) && live(k) != s)
            other--;
    }
    return n + (other > 0 ? other : 0);
}

// The settings line's icon: a pencil for unsaved drafts, the save symbol
// for changes in use but not saved, a tick when all is saved.
static const char *save_icon(int n)
{
    return drafts() ? LV_SYMBOL_EDIT : n ? LV_SYMBOL_SAVE : LV_SYMBOL_OK;
}

// A save is named by when it was made: "16/10/26 14:32", or "date unknown"
// when the clock was not set at the time.
static const char *save_when(char *buf, size_t n, const char *at)
{
    return at[0] ? ui_date(buf, n, at) : "date unknown";
}

static const char *plural(char *buf, size_t n, int count, const char *word)
{
    lv_snprintf(buf, n, "%d %s%s", count, word, count == 1 ? "" : "s");
    return buf;
}

/* --------------------------------------------------------------------------
 * Fields
 * ------------------------------------------------------------------------ */
typedef enum { K_NUM, K_TURNS, K_TOGGLE, K_SCHED } kind_t;

typedef struct {
    const char *cap, *unit;
    uint8_t dec;
    uint8_t kind;
    const char *info;
} field_t;

// In cfg_key_t order.
static const field_t F[CFG_N] = {
    [CFG_POSMAX]  = { "100 % travel", "turns", 2, K_TURNS, "Output turns, closed to open" },
    [CFG_REVERSE] = { "Motor direction", NULL, 0, K_TOGGLE, "Reverse if Open closes; saving clears the zero" },
    [CFG_BACKOFF] = { "Back-off", "steps", 0, K_NUM, "Homing step 2, off the switch" },
    [CFG_RETURN]  = { "Return", "steps", 0, K_NUM, "Homing step 4, off the switch; sets 0 %" },
    [CFG_FHOME]   = { "Fast close", "steps/s", 0, K_NUM, "Homing step 1, to the switch" },
    [CFG_FCREEP]  = { "Slow close", "steps/s", 0, K_NUM, "Homing step 3, onto the switch" },
    [CFG_RATE]    = { "Run speed", "steps/s", 0, K_NUM, "Move speed" },
    [CFG_START]   = { "Start speed", "steps/s", 0, K_NUM, "Start and stop speed" },
    [CFG_ACCEL]   = { "Acceleration", "steps/s/s", 0, K_NUM, "Ramp from start to run speed" },
    [CFG_KP]      = { "KP", "%/mB", 2, K_NUM, "Proportional gain" },
    [CFG_TI]      = { "TI", "min", 2, K_NUM, "Integral time; 0 = off" },
    [CFG_OUTMIN]  = { "AUTO min", "% open", 1, K_NUM, "Lowest opening AUTO commands" },
    [CFG_DB]      = { "Deadband open", "steps", 0, K_NUM, "AUTO opening moves gather to this, then go at once" },
    [CFG_KDIR]    = { "Pressure response", NULL, 0, K_TOGGLE, "Normally opening raises the pressure" },
    [CFG_BAND]    = { "Setpoint band", "mB", 2, K_NUM, "Tolerance about the setpoint; 0 = none" },
    [CFG_KD]      = { "KD", "%s/mB", 2, K_NUM, "Derivative, % per mB/s; 0 = off" },
    // The gain schedule (0.15.15): sets 2 and 3, as set 1 (KP, TI, KD);
    // set 4 and boundary 3 from 0.16.14.
    [CFG_KP2]     = { "Set 2 KP", "%/mB", 2, K_NUM, "Proportional gain" },
    [CFG_TI2]     = { "Set 2 TI", "min", 2, K_NUM, "Integral time; 0 = off" },
    [CFG_KD2]     = { "Set 2 KD", "%s/mB", 2, K_NUM, "Derivative, % per mB/s; 0 = off" },
    [CFG_KP3]     = { "Set 3 KP", "%/mB", 2, K_NUM, "Proportional gain" },
    [CFG_TI3]     = { "Set 3 TI", "min", 2, K_NUM, "Integral time; 0 = off" },
    [CFG_KD3]     = { "Set 3 KD", "%s/mB", 2, K_NUM, "Derivative, % per mB/s; 0 = off" },
    [CFG_SCHED]   = { "PID table", NULL, 0, K_SCHED, "Always on" },
    [CFG_B1]      = { "Boundary 1", "mB", 1, K_NUM, "Set 1 / set 2 switch point" },
    [CFG_B2]      = { "Boundary 2", "mB", 1, K_NUM, "Set 2 / set 3 switch point" },
    [CFG_HYST]    = { "Hysteresis", "%", 0, K_NUM, "Switching band about each boundary" },
    [CFG_DBC]     = { "Deadband close", "steps", 0, K_NUM, "AUTO closing moves below this are skipped" },
    [CFG_KP4]     = { "Set 4 KP", "%/mB", 2, K_NUM, "Proportional gain" },
    [CFG_TI4]     = { "Set 4 TI", "min", 2, K_NUM, "Integral time; 0 = off" },
    [CFG_KD4]     = { "Set 4 KD", "%s/mB", 2, K_NUM, "Derivative, % per mB/s; 0 = off" },
    [CFG_B3]      = { "Boundary 3", "mB", 1, K_NUM, "Set 3 / set 4 switch point; 0 = no set 4" },
    // Disable switch edges (0.16.21; "From, %" and "To, %" on their cards).
    [CFG_PASS_LO] = { "Disable from", "%", 0, K_NUM, "Switch works below this" },
    [CFG_PASS_HI] = { "Disable to", "%", 0, K_NUM, "Switch works above this" },
};

static const char *value_text(char *buf, size_t n, cfg_key_t k, int32_t v)
{
    switch (F[k].kind) {
    case K_TURNS:
        return fixed_fmt(buf, n, fixed_round((int64_t)v * 1000 / STEPS_PER_TURN, 1), 2);
    case K_TOGGLE:
        if (k == CFG_REVERSE)
            return v ? "Reversed" : "Normal";
        return v > 0 ? "Opening raises" : "Opening lowers";
    case K_SCHED:
        return v == CTRL_SCHED_PV ? "On" : "Off";
    default:
        // 0 means none here (0.16.25, Fable): no set 4, no band.
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

// Help lines from the field descriptions.
static const char *field_help(char *buf, size_t n, const char *notes, const cfg_key_t *keys, int nk)
{
    size_t used = 0;
    buf[0] = '\0';
    if (notes) {
        lv_snprintf(buf, n, "%s\n\n", notes);
        used = strlen(buf);
    }
    for (int i = 0; i < nk && used < n; i++) {
        // Terse, for a trained operator (0.15.18, Ryan): the unit in brackets.
        const field_t *f = &F[keys[i]];
        if (f->unit)
            lv_snprintf(buf + used, n - used, "%s [%s]: %s.\n", f->cap, f->unit, f->info);
        else
            lv_snprintf(buf + used, n - used, "%s: %s.\n", f->cap, f->info);
        used += strlen(buf + used);
    }
    if (used && buf[used - 1] == '\n')
        buf[used - 1] = '\0';
    return buf;
}

/* Editable card: white, outlined, 12 px caption, 16 px value, chevron; a
 * 3 px edge on the left for draft (dark) or unsaved (light grey). */
typedef struct {
    lv_obj_t *card, *edge, *v;
} ecard_t;

static void ecard_make(ecard_t *e, lv_obj_t *scr, int32_t x, int32_t y, int32_t w, const char *cap,
                       lv_event_cb_t cb, void *user)
{
    e->card = ui_card(scr, x, y, w, RH);
    lv_obj_add_flag(e->card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_width(e->card, 1, 0);
    lv_obj_set_style_border_color(e->card, UI_TICK, 0);
    lv_obj_set_style_bg_color(e->card, UI_TRACK, LV_STATE_PRESSED);
    lv_obj_add_event_cb(e->card, cb, LV_EVENT_CLICKED, user);
    ui_label(e->card, 11, 5, w - 31, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT, cap);
    e->v = ui_label(e->card, 11, 22, w - 31, &ui_font_16, UI_TEXT, LV_TEXT_ALIGN_LEFT, "");
    lv_label_set_long_mode(e->v, LV_LABEL_LONG_DOT);
    lv_obj_t *chev = ui_label(e->card, w - 20, 15, 0, &lv_font_montserrat_14, UI_IDLE, LV_TEXT_ALIGN_LEFT,
                              LV_SYMBOL_RIGHT);
    (void)chev;
    e->edge = ui_card(e->card, -1, 5, 3, 36);
    lv_obj_set_style_radius(e->edge, 0, 0);
    lv_obj_set_style_border_width(e->edge, 0, 0);
    lv_obj_set_style_bg_opa(e->edge, LV_OPA_TRANSP, 0);
}

static void ecard_state(ecard_t *e, const char *text, bool draft, bool applied)
{
    ui_set_text(e->v, text);
    ui_set_color(e->v, UI_TEXT);
    lv_opa_t opa = draft || applied ? LV_OPA_COVER : LV_OPA_TRANSP;
    if (lv_obj_get_style_bg_opa(e->edge, 0) != opa)
        lv_obj_set_style_bg_opa(e->edge, opa, 0);
    lv_color_t c = draft ? COL_DRAFT : COL_GREY;
    if (opa && !lv_color_eq(lv_obj_get_style_bg_color(e->edge, 0), c))
        lv_obj_set_style_bg_color(e->edge, c, 0);
}

static void field_update(ecard_t *e, cfg_key_t k)
{
    char buf[40];
    int32_t s;
    bool applied = cfg_value(CFG_FROM_SAVED, k, &s) && s != live(k);
    ecard_state(e, value_text(buf, sizeof buf, k, shown(k)), draft_set[k], applied);
}

/* Editing */
static cfg_key_t ed_key;

static bool ed_set(int32_t v, const char **why)
{
    (void)why;
    set_draft(ed_key, v);
    return true;
}

static bool ed_turns(int32_t v100, const char **why)
{
    (void)why;
    set_draft(CFG_POSMAX, (int32_t)(((int64_t)v100 * STEPS_PER_TURN + 50) / 100));
    return true;
}

// The keypad's side rows. A setting is normally in use as saved, so one
// row, Saved; In use as well only when they differ (after Load settings or
// a COM3 change), or In use alone when nothing is saved yet.
static void kp_rows(ui_kp_t *k, cfg_key_t key, char *now, char *sv, size_t n)
{
    int32_t s;
    bool have = cfg_value(CFG_FROM_SAVED, key, &s);
    int r = 0;
    if (!have || s != live(key)) {
        k->side_label[r] = "In use";
        k->side_text[r++] = value_text(now, n, key, live(key));
    }
    if (have) {
        k->side_label[r] = "Saved";
        k->side_text[r] = value_text(sv, n, key, s);
    }
}

static void ed_turns_keypad(void)
{
    static char now[16], sv[16];
    int32_t lo, hi, s;
    cfg_range(CFG_POSMAX, &lo, &hi);
    ui_kp_t k = { .title = "100 % travel, output turns", .dec = 2,   // no steps-per-turn hint (template)
                  .min = (int32_t)(((int64_t)lo * 100 + STEPS_PER_TURN - 1) / STEPS_PER_TURN),
                  .max = (int32_t)((int64_t)hi * 100 / STEPS_PER_TURN), .on_set = ed_turns };
    kp_rows(&k, CFG_POSMAX, now, sv, sizeof now);
    (void)s;
    ui_keypad_ex(&k);
}

static int32_t ed_present;   // full steps, the position "Use the present position" showed

static void ed_turns_present(void)
{
    set_draft(CFG_POSMAX, ed_present);
}

static void ed_toggle_ok(void)
{
    set_draft(ed_key, ed_key == CFG_REVERSE ? !shown(ed_key) : shown(ed_key) > 0 ? -1 : 1);
}

static void edit(cfg_key_t k)
{
    static char t[160], cap[40], now[24], sv[24];
    char b[16];
    ed_key = k;
    if (F[k].kind == K_TURNS) {
        int32_t lo, hi, pos = stepper3_position() / stepper3_scale();
        cfg_range(CFG_POSMAX, &lo, &hi);
        if (!valve_calibrated() || stepper3_busy() || pos < lo || pos > hi) {
            ed_turns_keypad();
            return;
        }
        ed_present = pos;
        lv_snprintf(t, sizeof t, "Use the present position (%s turns) as 100 %%, or type turns?",
                    value_text(b, sizeof b, k, pos));
        ui_ask(t, "Present", ed_turns_present, "Type turns", ed_turns_keypad);
    } else if (F[k].kind == K_TOGGLE) {
        if (k == CFG_REVERSE)
            lv_snprintf(t, sizeof t, "Set motor direction to %s? Saving clears the zero.",
                        shown(k) ? "Normal" : "Reversed");
        else
            lv_snprintf(t, sizeof t, "Set to %s?", shown(k) > 0 ? "Opening lowers pressure" : "Opening raises pressure");
        ui_confirm(t, "Change", ed_toggle_ok);
    } else {
        int32_t lo, hi, s;
        cfg_range(k, &lo, &hi);
        // The range is in the entry field's corners (Min, Max): no info line.
        ui_kp_t kp = { .title = caption(cap, sizeof cap, k), .dec = F[k].dec, .min = lo, .max = hi,
                       .on_set = ed_set };
        kp_rows(&kp, k, now, sv, sizeof now);
        (void)s;
        ui_keypad_ex(&kp);
    }
}

static void field_event(lv_event_t *e)
{
    edit((cfg_key_t)(intptr_t)lv_event_get_user_data(e));
}

/* --------------------------------------------------------------------------
 * Save, discard and the side column (Help, Save settings, Discard changes)
 * ------------------------------------------------------------------------ */
static void save_now(void)
{
    const char *why;
    char t[96];
    uint32_t rev;
    const char *at;
    if (!cfg_save_with(draft_v, draft_set, &why)) {
        lv_snprintf(t, sizeof t, "Not saved: %s", why);
        ui_msg(t);
        return;
    }
    drafts_clear();
    if (cfg_rev(CFG_FROM_SAVED, &rev, &at)) {
        char w[24];
        lv_snprintf(t, sizeof t, at[0] ? "Saved, %s." : "Saved, %s (clock not set).", save_when(w, sizeof w, at));
        ui_msg(t);
    }
}

static void save_ask(void)
{
    static char t[140];
    char c[24], w[24];
    uint32_t rev = 0;
    const char *at;
    int n = unsaved();
    if (!n) {
        ui_msg("Nothing to save");
        return;
    }
    // In AUTO the valve moves all the time; tuning saves there (cfg.c
    // refuses anything else with "Stop AUTO first").
    if (!valve_auto() && (stepper3_busy() || valve_mode() == VALVE_CALIBRATING)) {
        ui_msg("Wait for the valve to stop");
        return;
    }
    (void)rev; (void)at; (void)w;
    lv_snprintf(t, sizeof t, "Save %s?", plural(c, sizeof c, n, "change"));
    ui_confirm(t, "Save settings", save_now);
}

static void discard_drafts(void)
{
    char t[48], c[24];
    lv_snprintf(t, sizeof t, "%s discarded.", plural(c, sizeof c, drafts(), "draft"));
    drafts_clear();
    ui_msg(t);
}

static void discard_live(void)
{
    const char *why;
    char t[64];
    if (!cfg_load_live(CFG_FROM_SAVED, &why)) {
        lv_snprintf(t, sizeof t, "Not put back: %s", why);
        ui_msg(t);
    } else {
        ui_msg("Last save put back.");
    }
}

static void discard_ask(void)
{
    static char t[160];
    char c[24];
    uint32_t rev;
    const char *at;
    int n = drafts();
    if (n) {
        lv_snprintf(t, sizeof t, "Discard %s?",
                    plural(c, sizeof c, n, "unsaved change"));
        ui_confirm(t, "Discard changes", discard_drafts);
    } else if (cfg_unsaved() && cfg_rev(CFG_FROM_SAVED, &rev, &at)) {
        lv_snprintf(t, sizeof t, "Put the last save back? %s %s lost.", plural(c, sizeof c, cfg_unsaved(), "change"),
                    cfg_unsaved() == 1 ? "is" : "are");
        ui_confirm(t, "Put back", discard_live);
    } else {
        ui_msg("No unsaved changes");
    }
}

static void save_cb(lv_event_t *e) { (void)e; save_ask(); }
static void discard_cb(lv_event_t *e) { (void)e; discard_ask(); }

static const char *help_title, *(*help_text)(void);
static void help_cb(lv_event_t *e)
{
    (void)e;
    ui_text_view(help_title, help_text());
}

static struct {
    lv_obj_t *save, *discard;
} side;

// Help in row 0 (or Discard on Review and save), Save in row 1, Discard in row 2.
static void side_make(lv_obj_t *scr, const char *title, const char *(*help)(void))
{
    help_title = title;
    help_text = help;
    if (help)
        ui_btn(scr, 361, RY[0], 109, RH, UI_NAV, "Help", help_cb, NULL);
    side.save = ui_btn(scr, 361, RY[1], 109, RH, UI_PRIMARY, "Save settings", save_cb, NULL);
    side.discard = ui_btn(scr, 361, help ? RY[2] : RY[0], 109, RH, UI_SECONDARY, "Discard changes", discard_cb, NULL);
}

// Strip and side buttons, every refresh.
static void side_refresh(void)
{
    char t[64], c[32], w[24];
    uint32_t rev;
    const char *at;
    int n = unsaved();
    ui_btn_enable(side.save, n > 0);
    ui_btn_enable(side.discard, drafts() > 0 || cfg_unsaved() > 0);
    if (drafts())
        lv_snprintf(t, sizeof t, "%s. Save settings to use.", plural(c, sizeof c, n, "unsaved change"));
    else if (n)
        lv_snprintf(t, sizeof t, "%s in use, not saved.", plural(c, sizeof c, n, "change"));
    else if (cfg_rev(CFG_FROM_SAVED, &rev, &at))
        lv_snprintf(t, sizeof t, "All settings saved%s%s.", at[0] ? " " : "", at[0] ? save_when(w, sizeof w, at) : "");
    else
        lv_snprintf(t, sizeof t, "Factory settings, nothing saved yet.");
    ui_strip_set(UI_STRIP_INFO, save_icon(n), t);
}

/* --------------------------------------------------------------------------
 * Restart (System)
 * ------------------------------------------------------------------------ */
// Restart (on Diagnostics from 0.12.19, Name and network from 0.13.2,
// next to Motor override on Settings from 0.13.5, Ryan 28 Oct; on System
// beside Firmware in the common HMI template): asks
// first, stops the valve (AUTO ends, homing is cancelled), then waits 2.5 s
// so the valve position is saved at standstill (valvepos.h, 2 s) and
// offered at start-up.
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

static void do_restart(void)
{
    valve_stop();
    valve_event("SYS", "Restart from the screen");
    ui_msg_no_ok("Restarting.");   // nothing to acknowledge (Ryan, 28 Oct)
    lv_timer_t *t = lv_timer_create(restart_now, 2500, NULL);
    lv_timer_set_repeat_count(t, 1);
}

void ui_restart_ask(void)
{
    ui_confirm("Restart the controller? The valve stops and holds where it is.", "Restart", do_restart);
}

static void restart_cb(lv_event_t *e)
{
    (void)e;
    ui_restart_ask();
}

/* --------------------------------------------------------------------------
 * Cryostat name (System > Cryostat name): a keyboard for the name in the
 * Home title, "<name> pressure controller". Use name puts it in use at
 * once, unsaved until Save settings, like a captured calibration.
 * ------------------------------------------------------------------------ */
static struct {
    lv_obj_t *kb, *text, *use;
    char buf[CFG_NAME_LEN + 1];
} nm;

static const char *const nm_map[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", "-", "\n",
    "Z", "X", "C", "V", "B", "N", "M", "Space", "Del", ""
};

static void nm_show(void)
{
    // The end being typed stays in view.
    const char *t = nm.buf;
    lv_point_t sz;
    for (;;) {
        lv_text_get_size(&sz, t, &ui_font_16, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (sz.x <= 89 || !*t)
            break;
        t++;
    }
    ui_set_text(nm.text, t[0] ? t : "None");
    ui_set_color(nm.text, t[0] ? UI_TEXT : UI_MUTED);
    ui_btn_enable(nm.use, strcmp(nm.buf, cfg_name()) != 0);
}

static void nm_key(lv_event_t *e)
{
    (void)e;
    uint32_t id = lv_buttonmatrix_get_selected_button(nm.kb);
    const char *k = lv_buttonmatrix_get_button_text(nm.kb, id);
    size_t n = strlen(nm.buf);
    if (!k)
        return;
    if (!strcmp(k, "Del")) {
        if (n)
            nm.buf[n - 1] = '\0';
    } else if (n >= CFG_NAME_LEN) {
        ui_msg("Up to 12 characters");
        return;
    } else {
        nm.buf[n] = strcmp(k, "Space") ? k[0] : ' ';
        nm.buf[n + 1] = '\0';
    }
    nm_show();
}

static void nm_use(lv_event_t *e)
{
    if (ui_btn_disabled(e))
        return;
    // No spaces at the ends.
    char *a = nm.buf;
    while (*a == ' ')
        a++;
    size_t n = strlen(a);
    while (n && a[n - 1] == ' ')
        a[--n] = '\0';
    const char *why = NULL;
    if (!cfg_set_name(a, &why)) {
        ui_msg(why);
        return;
    }
    valve_event("CFG", a[0] ? "Cryostat name set (screen)" : "Cryostat name cleared (screen)");
    ui_back();
}

static void nm_cancel(lv_event_t *e)
{
    (void)e;
    ui_back();
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

static void nm_build(lv_obj_t *scr)
{
    lv_strlcpy(nm.buf, cfg_name(), sizeof nm.buf);
    nm.kb = key_grid(scr, 343, nm_map, nm_key);
    // Bottom row: Z to M as the keys above, Space and Del wider.
    for (uint32_t i = 30; i < 37; i++)
        lv_buttonmatrix_set_button_width(nm.kb, i, 4);
    lv_buttonmatrix_set_button_width(nm.kb, 37, 7);
    lv_buttonmatrix_set_button_width(nm.kb, 38, 5);
    nm.text = ui_info(scr, 361, RY[0], 109, RH, "Name");
    nm.use = ui_btn(scr, 361, RY[1], 109, RH, UI_PRIMARY, "Use name", nm_use, NULL);
    ui_btn(scr, 361, RY[2], 109, RH, UI_SECONDARY, "Cancel", nm_cancel, NULL);
    nm_show();
}

static void nm_refresh(void)
{
    ui_strip_set(UI_STRIP_INFO, LV_SYMBOL_HOME, "Shown on Home. Save settings to keep it.");
}

const ui_screen_t ui_scr_name = { "Cryostat name", nm_build, nm_refresh, NULL };

/* --------------------------------------------------------------------------
 * System (on the Menu; Name and network 0.12.13 to 0.13.6, System 0.13.7 to 0.13.18,
 * Ryan 28 Oct): the cryostat name, the Ethernet address (static, no DHCP
 * from 0.14.9), the address in use, the MAC, the
 * firmware and its build date, the Remote control switch, Restart and
 * Shut down (0.16.24).
 * ------------------------------------------------------------------------ */
static struct { lv_obj_t *name, *net, *mac, *fw, *remote, *rsw, *rtext; } nn;

static void nn_go(lv_event_t *e) { ui_open((const ui_screen_t *)lv_event_get_user_data(e)); }

// Remote control (0.13.4, Ryan 28 Oct, network plan A): a toggle switch
// since 0.13.7; a press anywhere on the card turns it On or Off, live at
// once, kept by Save settings like the name.
static void remote_cb(lv_event_t *e)
{
    (void)e;
    cfg_set_remote(!cfg_remote());
    lv_obj_set_state(nn.rsw, LV_STATE_CHECKED, cfg_remote());
}

// Restart and Shut down on System are actions, not pages: filled dark
// (Restart from 0.13.5, Ryan 28 Oct; Shut down beside it from 0.16.24).
static void nn_dark(lv_obj_t *b)
{
    lv_obj_set_style_bg_color(b, UI_TEXT, 0);
    lv_obj_set_style_text_color(b, lv_color_white(), 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x3C4856), LV_STATE_PRESSED);
}

static void shut_cb(lv_event_t *e)
{
    (void)e;
    ui_shutdown_ask();   // the power icon's question
}

static void nn_build(lv_obj_t *scr)
{
    // 0.16.24 (Ryan 13 Nov: "diagnostics should not be a button in system.
    // put remote control on the first row right. ip on second row left and
    // mac on second row right ... maybe put restart in diagnostics place and
    // shut down in where restart is now"): Cryostat name and Remote control,
    // IP address and MAC, then Firmware, Restart and Shut down. Diagnostics
    // is on the Menu only.
    nn.name = ui_btn(scr, 10, RY[0], 226, RH, UI_NAV, "Cryostat name", nn_go, (void *)&ui_scr_name);
    // Remote control: the name and what it means, the switch on the right.
    lv_obj_t *c = nn.remote = ui_card(scr, 244, RY[0], 226, RH);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c, remote_cb, LV_EVENT_PRESSED, NULL);
    ui_label(c, 12, 6, 150, &ui_font_16, UI_TEXT, LV_TEXT_ALIGN_LEFT, "Remote control");
    nn.rtext = ui_label(c, 12, 26, 150, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT, "");
    lv_obj_t *sw = nn.rsw = lv_switch_create(c);
    lv_obj_set_size(sw, 52, 28);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_remove_flag(sw, LV_OBJ_FLAG_CLICKABLE);   // the card takes the press
    lv_obj_set_style_bg_color(sw, lv_color_hex(0xC3CDD7), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, UI_BLUE, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_anim_duration(sw, 0, 0);
    nn.net = ui_btn(scr, 10, RY[1], 226, RH, UI_NAV, "IP address", nn_go, (void *)&ui_scr_netaddr);
    // The MAC, for the site's network records (Ryan, 28 Oct).
    nn.mac = ui_info(scr, 244, RY[1], 226, RH, "MAC address");
    nn.fw = ui_info(scr, 10, RY[2], 226, RH, "Firmware");
    nn_dark(ui_btn(scr, 244, RY[2], 109, RH, UI_SECONDARY, "Restart", restart_cb, NULL));
    nn_dark(ui_btn(scr, 361, RY[2], 109, RH, UI_SECONDARY, "Shut down", shut_cb, NULL));
}

static void nn_refresh(void)
{
    char t[48];
    uint32_t ip, mask;
    cfg_net(&ip, &mask);
    ui_btn_text(nn.name, "Cryostat name", cfg_name()[0] ? cfg_name() : "None");
    cfg_net_text(t, sizeof t, ip, mask);
    if (ip == 0 && mask == 0)
        lv_strlcpy(t, "Build default", sizeof t);
    else
        *strchr(t, ' ') = '\0';   // the address; the mask is on its page
    ui_btn_text(nn.net, "IP address", t);
#if PMC_LINK_ETHERNET
    ui_set_text(nn.mac, ethlink_mac()[0] ? ethlink_mac() : "Not read");
#else
    ui_set_text(nn.mac, "None, USB build");
#endif
    ui_set_text(nn.fw, ui_fw_text());
    if (lv_obj_has_state(nn.rsw, LV_STATE_CHECKED) != cfg_remote())
        lv_obj_set_state(nn.rsw, LV_STATE_CHECKED, cfg_remote());
    ui_set_text(nn.rtext, cfg_remote() ? "On: network can set" : "Off: network read only");
    ui_strip_set(UI_STRIP_INFO, LV_SYMBOL_SETTINGS, cfg_net_restart_due()
                 ? "New IP address: Save settings, then restart." : "Static IP address.");   // no DHCP wording (Ryan 6 Nov)
}

const ui_screen_t ui_scr_namenet = { "System", nn_build, nn_refresh, NULL };

/* IP address: digits and dots for the address and the mask; Use puts
 * them in use for the next restart, unsaved until Save settings. */
static struct {
    lv_obj_t *kb, *box[2], *val[2], *use;
    char buf[2][16];
    int sel;
} na;

static const char *const na_map[] = {
    "1", "2", "3", "\n",
    "4", "5", "6", "\n",
    "7", "8", "9", "\n",
    ".", "0", "Del", ""
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
    if (!strcmp(k, "Del")) {
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
    valve_event("CFG", "IP address set to %s (screen), not saved", t);
    ui_back();
}

static void na_default(lv_event_t *e)
{
    (void)e;
    const char *why;
    cfg_set_net(0, 0, &why);
    valve_event("CFG", "IP address set to the build default (screen), not saved");
    ui_back();
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
    static const char *const cap[2] = { "Address (tap to type)", "Mask (tap to type)" };
    for (int i = 0; i < 2; i++) {
        na.val[i] = ui_info(scr, 244, RY[i], 226, RH, cap[i]);
        na.box[i] = lv_obj_get_parent(na.val[i]);
        lv_obj_set_style_border_color(na.box[i], UI_BLUE, 0);
        lv_obj_add_flag(na.box[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(na.box[i], na_pick, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    na.use = ui_btn(scr, 244, RY[2], 109, RH, UI_PRIMARY, "Use", na_use, NULL);
    ui_btn(scr, 361, RY[2], 109, RH, UI_SECONDARY, "Default", na_default, NULL);
    na_show();
}

static void na_refresh(void)
{
    ui_strip_set(UI_STRIP_INFO, LV_SYMBOL_SETTINGS, "The controller's address, used after Save settings and a restart.");
}

const ui_screen_t ui_scr_netaddr = { "IP address", na_build, na_refresh, NULL };

/* --------------------------------------------------------------------------
 * Motor settings (three tabs) and PID tuning (three)
 * ------------------------------------------------------------------------ */
typedef struct {
    const char *label;
    cfg_key_t keys[6];
    int n;
    const char *note;
    bool tall;                // the keys as three tall cards (PID tuning's PID tab)
    bool over;                // the Disable switch toggle, its two edges and the Motor override button (Override tab)
    bool brow;                // B1, B2 and B3 in a row, the last key under them (Set ranges tab)
    bool arow;                // 109 + 226 on the first row, 109 under it (AUTO tab)
    const char *aside;        // grey text right of the last row's 109 px card
} tab_t;

// Motor settings (Valve setup to 0.16.1, Valve settings to 0.16.15, Ryan 12 Nov
// "change valve settings to motor settings"; Ryan 9 Nov: "the functionality
// between valve position and valve setup is a bit confusing"): the valve's
// saved numbers, drafts until Save settings; Travel, Homing and Drive move
// nothing. Motor override, which drives the motor by steps, opens from the
// fourth tab, Override (0.16.17, Ryan 12 Nov: "it doesnt feel right to be in
// valve position"; the side column and the strip have no room for it). Home
// valve and Set position are on Valve position.
// Drive is the first tab from 0.16.21 (Ryan 12 Nov: "put drive in the
// first tab"). The Override tab's Ignore switch wording is from 0.16.21
// (Ryan 12 Nov: the description was unclear; "Switch pass" to 0.16.20), and
// its edges are saved settings (Ryan: "make the boundary editable"). From
// 0.16.22 (Ryan 13 Nov: "change ignore to disable, and make the description
// less verbose. also make all help notes or subtexts less verbose"):
// Disable switch, and every note here a line or two.
#define VALVE_SET_NOTE "Saved numbers; nothing moves. Home or set the valve on Valve position."
static const tab_t valve_tabs[4] = {
    { "Drive", { CFG_RATE, CFG_START, CFG_ACCEL }, 3, "Manual and AUTO moves. Saved with the valve stopped." },
    // The deadbands (0.15.15: opening and closing) are the valve's: from
    // the 5 Nov bench, the needle sticks opening at low openings.
    { "Travel", { CFG_POSMAX, CFG_REVERSE, CFG_DB, CFG_DBC }, 4, VALVE_SET_NOTE },
    { "Homing", { CFG_BACKOFF, CFG_RETURN, CFG_FHOME, CFG_FCREEP }, 4, "Used by Home valve. " VALVE_SET_NOTE },
    { "Override", { CFG_PASS_LO, CFG_PASS_HI }, 2,
      "Disable switch: when On and homed, the limit switch does not stop closing from From to To % open "
      "(no W-1, W-4 or F-4). Off after a restart.\n\nMotor override: drives the motor by steps.", false, true },
};
// PID tuning (0.14.21, Ryan 4 Nov; was Control tuning): KP, TI and KD by
// name on their own tab, the rest of AUTO's settings on the other. From
// 0.15.15 (Ryan 5 Nov, "build like industry ones"): three PID sets, one
// shown at a time (the keys here are set 1's; the set row picks), and the
// gain schedule on its own tab. From 0.16.14 (Ryan 11 Nov, "Yes fourth
// set"): four sets and a third boundary. From 0.16.20 (Ryan 12 Nov) the
// table is always on. From 0.16.25 (Ryan 13 Nov: "rework the pid tuning
// page, use fable to evaluate whether each control is intuitive"; Fable's
// review): tabs named by what they hold, Gains, Set ranges and AUTO; the
// set row as four cards, the one shown outlined, so it does not read as a
// second tab bar; the set in the gain captions; each boundary captioned by
// the two sets it splits, the hysteresis under them; 0 shown as None; a
// grey line of explanation in the empty cell; no "Save to keep".
static const tab_t tune_tabs[3] = {
    { "Gains", { CFG_KP, CFG_TI, CFG_KD }, 3, "Tap a set to edit it. Dot = set in use now.", true },
    { "Set ranges", { CFG_B1, CFG_B2, CFG_B3, CFG_HYST }, 4,
      "Each boundary splits two sets. Set 3 | 4 None: three sets.", false, false, true, false,
      "The lower of pressure and setpoint picks the set. Hysteresis: % of the boundary." },
    { "AUTO", { CFG_OUTMIN, CFG_KDIR, CFG_BAND }, 3,
      "AUTO min: lowest opening AUTO commands. Band 0 = none.", false, false, false, true,
      "SP band: the In band and Stable tolerance about the setpoint." },
};

static struct {
    const tab_t *tabs;
    int ntabs, tab;
    ecard_t f[6];
    lv_obj_t *seg, *segb[4];
    lv_obj_t *scr;
    int set;                                   // the PID set the PID tab shows, 1 to 4
    bool sets;                                 // the PID tab shows the set row (schedule on)
    bool sel;                                  // the set row is up
    lv_obj_t *selb[4], *selr[4], *seld[4];     // the set row: cards, range lines, in-use dots
    lv_obj_t *band_sw, *band_title, *band_text;   // Motor settings' Override tab: the Disable switch toggle
} pg;

// The key a tab shows in place i: on the PID tab, the set picked.
static cfg_key_t pg_key(const tab_t *t, int i)
{
    return t->tall ? cfg_pid_key(pg.set, i) : t->keys[i];
}

static void segs_draw(lv_obj_t *const *b, int n, int on);
static void pg_fields(void);
static void pg_refresh(void);
static void motor_cb(lv_event_t *e);
static void band_cb(lv_event_t *e);

static void set_cb(lv_event_t *e)
{
    int s = (int)(intptr_t)lv_event_get_user_data(e);
    if (s == pg.set)
        return;
    pg.set = s;
    pg_fields();
    pg_refresh();
}

// A grey track with n raised segments (the tabs, and the PID tab's sets).
static lv_obj_t *segs_make(lv_obj_t *scr, int32_t y, int n, lv_obj_t **b, lv_event_cb_t cb, int first)
{
    lv_obj_t *track = ui_card(scr, 10, y, 343, RH);
    lv_obj_set_style_bg_color(track, lv_color_hex(0xC3CDD7), 0);
    lv_obj_set_style_border_width(track, 0, 0);
    lv_obj_set_style_radius(track, 6, 0);
    int32_t iw = (343 - 8) / n;
    for (int i = 0; i < n; i++) {
        b[i] = ui_card(track, 4 + i * iw, 4, iw, RH - 8);
        lv_obj_add_flag(b[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_radius(b[i], 4, 0);
        lv_obj_set_style_border_width(b[i], 0, 0);
        lv_obj_add_event_cb(b[i], cb, LV_EVENT_CLICKED, (void *)(intptr_t)(first + i));
    }
    return track;
}

// The set row: "Set 2" over its pressures, a dot on the set in use. Four
// white cards of 82 px from 0.16.25 (a grey track with segments, like the
// tab bar, to 0.16.24), the one shown with a 2 px dark outline.
static void sel_draw(void)
{
    for (int i = 0; i < 4; i++) {
        bool on = i + 1 == pg.set;
        lv_obj_set_style_border_width(pg.selb[i], on ? 2 : 1, 0);
        lv_obj_set_style_border_color(pg.selb[i], on ? UI_TEXT : UI_TICK, 0);
    }
}

static void sel_make(void)
{
    const int32_t iw = 82;
    pg.sel = true;
    for (int i = 0; i < 4; i++) {
        char t[8];
        lv_obj_t *b = pg.selb[i] = ui_card(pg.scr, 10 + i * 87, RY[1], iw, RH);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(b, UI_TRACK, LV_STATE_PRESSED);
        lv_obj_add_event_cb(b, set_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(i + 1));
        lv_snprintf(t, sizeof t, "Set %d", i + 1);
        ui_label(b, 0, 4, iw, &ui_font_16, UI_TEXT, LV_TEXT_ALIGN_CENTER, t);
        pg.selr[i] = ui_label(b, 0, 24, iw, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_CENTER, "");
        // The dot 6 px before "Set n", on the middle of its capitals (0.15.20,
        // Ryan: it sat at the segment's left edge, apart from the name).
        lv_point_t sz;
        lv_text_get_size(&sz, t, &ui_font_16, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        const int32_t cap_mid = 4 + ui_font_16.line_height - ui_font_16.base_line - 6;   // half a 12 px capital
        pg.seld[i] = ui_card(b, (iw - sz.x) / 2 - 6 - 8, cap_mid - 4, 8, 8);
        lv_obj_set_style_radius(pg.seld[i], 4, 0);
        lv_obj_set_style_border_width(pg.seld[i], 0, 0);
        lv_obj_set_style_bg_color(pg.seld[i], UI_TEXT, 0);
    }
    sel_draw();
}

static void sel_refresh(void)
{
    // Four segments of 83 px (0.16.14): the pressures without "mB",
    // "< 6.0", "6.0-10.0", "> 14.0"; B3 0 (three sets): set 4 "Not used".
    char b[3][16], t[4][28];
    fixed_fmt(b[0], sizeof b[0], shown(CFG_B1), 1);
    fixed_fmt(b[1], sizeof b[1], shown(CFG_B2), 1);
    fixed_fmt(b[2], sizeof b[2], shown(CFG_B3), 1);
    lv_snprintf(t[0], sizeof t[0], "< %s", b[0]);
    lv_snprintf(t[1], sizeof t[1], "%s-%s", b[0], b[1]);
    if (shown(CFG_B3) != 0) {
        lv_snprintf(t[2], sizeof t[2], "%s-%s", b[1], b[2]);
        lv_snprintf(t[3], sizeof t[3], "> %s", b[2]);
    } else {
        lv_snprintf(t[2], sizeof t[2], "> %s", b[1]);
        lv_snprintf(t[3], sizeof t[3], "Not used");
    }
    const int use = valve_pid_set();
    for (int i = 0; i < 4; i++) {
        ui_set_text(pg.selr[i], t[i]);
        if ((i + 1 == use) == lv_obj_has_flag(pg.seld[i], LV_OBJ_FLAG_HIDDEN)) {
            if (i + 1 == use) lv_obj_remove_flag(pg.seld[i], LV_OBJ_FLAG_HIDDEN);
            else              lv_obj_add_flag(pg.seld[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void pg_fields(void)
{
    // Rebuilt on a tab change: only one tab's cards exist at a time.
    for (int i = 0; i < 6; i++)
        if (pg.f[i].card) {
            lv_obj_delete(pg.f[i].card);
            pg.f[i].card = NULL;
        }
    if (pg.sel) {
        for (int i = 0; i < 4; i++) {
            lv_obj_delete(pg.selb[i]);
            pg.selb[i] = NULL;
        }
        pg.sel = false;
    }
    pg.band_sw = pg.band_title = pg.band_text = NULL;   // went with their card (the Disable switch's)
    const tab_t *t = &pg.tabs[pg.tab];
    int y0 = pg.ntabs > 1 ? 1 : 0;
    char cap[40];
    // The PID tab: the set row and the set picked as three cards under it
    // (the PID table always on from 0.16.20; set 1 as three tall cards with
    // it Off to 0.16.19).
    pg.sets = t->tall;
    if (pg.sets) {
        // The set in the caption (0.16.25): "KP 2, %/mB" (78 px of caption).
        static const char *const nm[3] = { "KP", "TI", "KD" };
        sel_make();
        for (int i = 0; i < 3; i++) {
            const cfg_key_t k = pg_key(t, i);
            lv_snprintf(cap, sizeof cap, "%s %d, %s", nm[i], pg.set, F[k].unit);   // KD in %s/mB: % per mB/s (0.16.25)
            ecard_make(&pg.f[i], pg.scr, 10 + i * 117, RY[y0 + 1], 109, cap, field_event, (void *)(intptr_t)k);
        }
    }
    // Motor settings' Override tab: the Disable switch toggle across both
    // columns (0.16.19, Ryan 12 Nov: "make switch override a toggle on the
    // override screen in motor settings page"; the Switch tab of Motor
    // override from 0.16.5), its edges From and To under it (0.16.21), then
    // the Motor override button (0.16.17), set apart by a dark outline. A
    // press anywhere on the toggle card turns it on or off, live at once, as
    // Remote control on System. The button and the toggle card go in the
    // spare f[] slots so a tab change deletes them.
    if (t->over) {
        static const char *const en[2] = { "From, %", "To, %" };
        lv_obj_t *c = pg.f[2].card = ui_card(pg.scr, C2X[0], RY[y0], 343, RH);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, band_cb, LV_EVENT_PRESSED, NULL);
        pg.band_title = ui_label(c, 12, 6, 260, &ui_font_16, UI_TEXT, LV_TEXT_ALIGN_LEFT, "");
        pg.band_text = ui_label(c, 12, 26, 260, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT, "");
        lv_obj_t *sw = pg.band_sw = lv_switch_create(c);
        lv_obj_set_size(sw, 52, 28);
        lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -12, 0);
        lv_obj_remove_flag(sw, LV_OBJ_FLAG_CLICKABLE);   // the card takes the press
        lv_obj_set_style_bg_color(sw, lv_color_hex(0xC3CDD7), LV_PART_MAIN);
        lv_obj_set_style_bg_color(sw, UI_BLUE, LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_anim_duration(sw, 0, 0);
        for (int i = 0; i < 2; i++)
            ecard_make(&pg.f[i], pg.scr, 10 + i * 117, RY[y0 + 1], 109, en[i], field_event,
                       (void *)(intptr_t)t->keys[i]);
        lv_obj_t *b = pg.f[3].card = ui_btn(pg.scr, 244, RY[y0 + 1], 109, RH, UI_SECONDARY, "Motor\noverride",
                                            motor_cb, NULL);
        lv_obj_set_style_border_side(b, LV_BORDER_SIDE_FULL, 0);
        lv_obj_set_style_border_width(b, 2, 0);
        lv_obj_set_style_border_color(b, UI_WARN, 0);
    }
    // Set ranges (brow, 0.16.25): B1, B2 and B3 in a row, each captioned by
    // the two sets it splits, then the hysteresis under B1. AUTO (arow):
    // AUTO min and Pressure response, the setpoint band under AUTO min.
    // Either way a grey line (aside) fills the cell right of the last card.
    for (int i = 0; i < t->n && (t->brow || t->arow); i++) {
        static const char *const bn[4] = { "Set 1 | 2, mB", "Set 2 | 3, mB", "Set 3 | 4, mB", "Hysteresis, %" };
        static const char *const an[3] = { "AUTO min, %", "Pressure response", "SP band, mB" };
        int32_t x, y, w = 109;
        if (t->brow) {
            x = i < 3 ? 10 + i * 117 : 10;
            y = RY[y0 + (i < 3 ? 0 : 1)];
        } else {
            x = i == 1 ? 127 : 10;
            y = RY[y0 + (i < 2 ? 0 : 1)];
            if (i == 1) w = 226;
        }
        ecard_make(&pg.f[i], pg.scr, x, y, w, t->brow ? bn[i] : an[i], field_event, (void *)(intptr_t)t->keys[i]);
    }
    if (t->aside) {
        // Up to three 12 px lines, centred on the row.
        lv_obj_t *l = pg.f[5].card = ui_label(pg.scr, 139, 0, 212, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT, t->aside);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_height(l, LV_SIZE_CONTENT);
        lv_obj_update_layout(l);
        lv_obj_set_y(l, RY[y0 + 1] + (RH - lv_obj_get_height(l)) / 2);
    }
    // Two columns (Motor settings' Drive, Travel and Homing tabs).
    for (int i = 0; i < t->n && !t->tall && !t->over && !t->brow && !t->arow; i++)
        ecard_make(&pg.f[i], pg.scr, C2X[i % 2], RY[y0 + i / 2], C2W[i % 2], caption(cap, sizeof cap, t->keys[i]),
                   field_event, (void *)(intptr_t)t->keys[i]);
}

static void segs_draw(lv_obj_t *const *b, int n, int on)
{
    for (int i = 0; i < n; i++) {
        lv_obj_set_style_bg_opa(b[i], i == on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_width(b[i], 0, 0);
        ui_set_color(lv_obj_get_child(b[i], 0), i == on ? UI_TEXT : UI_MUTED);
    }
}

static void seg_draw(void)
{
    if (pg.ntabs > 1)
        segs_draw(pg.segb, pg.ntabs, pg.tab);
}

static void seg_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i == pg.tab)
        return;
    pg.tab = i;
    seg_draw();
    pg_fields();
    pg_refresh();   // the values now, not at the next 0.5 s refresh (Ryan, 28 Oct)
}

static const char *pg_help(void)
{
    static char buf[1100];
    const tab_t *t = &pg.tabs[pg.tab];
    cfg_key_t keys[6];
    for (int i = 0; i < t->n; i++)
        keys[i] = pg_key(t, i);
    return field_help(buf, sizeof buf, t->note, keys, t->n);
}

static void pg_build(lv_obj_t *scr, const char *title, const tab_t *tabs, int n)
{
    memset(&pg, 0, sizeof pg);
    pg.scr = scr;
    pg.tabs = tabs;
    pg.ntabs = n;
    pg.set = valve_pid_set();   // the PID tab opens on the set in use
    if (n > 1) {
        // Segmented choice: a grey track, the page shown as a raised white segment.
        pg.seg = segs_make(scr, RY[0], n, pg.segb, seg_cb, 0);
        int32_t iw = (343 - 8) / n;
        for (int i = 0; i < n; i++) {
            lv_obj_t *l = ui_label(pg.segb[i], 0, 0, iw, &ui_font_16, UI_TEXT, LV_TEXT_ALIGN_CENTER, tabs[i].label);
            lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
        }
        seg_draw();
    }
    pg_fields();
    side_make(scr, title, pg_help);
}

static void pg_refresh(void)
{
    const tab_t *t = &pg.tabs[pg.tab];
    for (int i = 0; i < t->n; i++)
        field_update(&pg.f[i], pg_key(t, i));
    if (pg.sel)
        sel_refresh();
    if (pg.band_sw) {
        bool band = valve_limit_band();
        if (lv_obj_has_state(pg.band_sw, LV_STATE_CHECKED) != band)
            lv_obj_set_state(pg.band_sw, LV_STATE_CHECKED, band);
        // Wording by Fable (0.16.21), shorter and Disable from 0.16.22.
        char t1[40], t2[48];
        lv_snprintf(t1, sizeof t1, "Disable switch %d to %d %%", valve_pass_lo(), valve_pass_hi());
        if (valve_pass_lo() > 0)
            lv_snprintf(t2, sizeof t2, "On: still stops below %d %%", valve_pass_lo());
        else
            lv_snprintf(t2, sizeof t2, "On: stops by count at 0 %%");   // From 0: the move's own end
        ui_set_text(pg.band_title, t1);
        ui_set_text(pg.band_text, !band ? "Off: switch stops closing" : valve_calibrated() ? t2 : "On: works once homed");
    }
    side_refresh();
}

// Motor override (Settings to 0.14.13, Valve position to 0.16.16, Motor
// settings > Override from 0.16.17): set apart from the saved numbers by a
// dark outline, and asked first. Limit override is on it since 0.11.33.
// Back from it reopens Motor settings on the Override tab.
static int valve_back_tab = -1;

static void band_cb(lv_event_t *e)
{
    (void)e;
    valve_limit_band_set(!valve_limit_band());
    pg_refresh();
}

static void motor_go(void)
{
    valve_back_tab = pg.tab;
    ui_service();
}

static void motor_cb(lv_event_t *e)
{
    (void)e;
    if (stepper3_busy()) {
        ui_msg("Wait for the valve to stop");
        return;
    }
    ui_confirm("Open Motor override? Drives the motor by steps.", "Open", motor_go);
}

static void valve_set_build(lv_obj_t *scr)
{
    pg_build(scr, "Motor settings", valve_tabs, 4);
    if (valve_back_tab > 0) {
        pg.tab = valve_back_tab;
        seg_draw();
        pg_fields();
    }
    valve_back_tab = -1;
}
static void tune_build(lv_obj_t *scr) { pg_build(scr, "PID tuning", tune_tabs, 3); }

const ui_screen_t ui_scr_valve_set = { "Motor settings", valve_set_build, pg_refresh, NULL };
const ui_screen_t ui_scr_tune = { "PID tuning", tune_build, pg_refresh, NULL };

/* --------------------------------------------------------------------------
 * Review and save
 * ------------------------------------------------------------------------ */
static struct {
    ecard_t changes;
    lv_obj_t *saved, *prev, *load;
} rv;

static void changes_cb(lv_event_t *e)
{
    (void)e;
    static char t[900], title[40];
    char a[40], b[40], cap[40], c[24];
    size_t u = 0;
    int n = 0;
    t[0] = '\0';
    for (int k = 0; k < CFG_N; k++) {
        int32_t s;
        bool have = cfg_value(CFG_FROM_SAVED, (cfg_key_t)k, &s);
        int32_t l = live((cfg_key_t)k);
        if (draft_set[k]) {
            lv_snprintf(t + u, sizeof t - u, "%s: %s to %s (draft)\n", caption(cap, sizeof cap, (cfg_key_t)k),
                        value_text(a, sizeof a, (cfg_key_t)k, have ? s : l),
                        value_text(b, sizeof b, (cfg_key_t)k, draft_v[k]));
        } else if (have && s != l) {
            lv_snprintf(t + u, sizeof t - u, "%s: %s to %s (in use)\n", caption(cap, sizeof cap, (cfg_key_t)k),
                        value_text(a, sizeof a, (cfg_key_t)k, s), value_text(b, sizeof b, (cfg_key_t)k, l));
        } else {
            continue;
        }
        u += strlen(t + u);
        n++;
    }
    char other[300];
    if (cfg_unsaved_other(other, sizeof other)) {
        lv_snprintf(t + u, sizeof t - u, "%s\n", other);
        u += strlen(t + u);
        for (const char *p = other; *p; p++)
            n += *p == '\n';
        n++;
    }
    if (n)
        lv_snprintf(t + u, sizeof t - u, "\nSave settings puts them in use and keeps them.");
    else
        lv_snprintf(t, sizeof t, "None");
    lv_snprintf(title, sizeof title, n ? "Unsaved changes (%s)" : "Unsaved changes", plural(c, sizeof c, n, "line"));
    ui_text_view(title, t);
}

static int load_n;
static cfg_from_t load_from[3];

static cfg_from_t load_sel;

static void load_do(void)
{
    cfg_from_t from = load_sel;
    int n = 0, before = 0;
    // Each setting with a field comes in as a draft; the calibration of a
    // save comes in use straight away (it has no draft).
    if (from != CFG_FROM_DEFAULTS) {
        const char *why;
        // What the load replaces: settings in use but unsaved, and drafts.
        before = cfg_unsaved();
        for (int k = 0; k < CFG_N; k++)
            before += draft_set[k];
        if (!cfg_load_live(from, &why)) {
            char t[64];
            lv_snprintf(t, sizeof t, "Not loaded: %s", why);
            ui_msg(t);   // the drafts are kept
            return;
        }
        drafts_clear();
        n = cfg_unsaved();
    } else {
        for (int k = 0; k < CFG_N; k++) {
            int32_t v;
            if (cfg_value(from, (cfg_key_t)k, &v) && v != shown((cfg_key_t)k)) {
                set_draft((cfg_key_t)k, v);
                n++;
            }
        }
    }
    char t[80], c[24];
    if (!n && before)
        // Loading the last save puts it back over changes: say so, not "the same".
        ui_msg(from == CFG_FROM_SAVED ? "Last save restored and in use." : "Restored and in use, the same as the last save.");
    else if (!n)
        ui_msg("Already the same");
    else {
        if (from == CFG_FROM_DEFAULTS)
            lv_snprintf(t, sizeof t, "Factory defaults loaded as %s. Save settings to use them.",
                        plural(c, sizeof c, n, "unsaved change"));
        else
            lv_snprintf(t, sizeof t, "Loaded and in use, %s from the last save. Save settings to keep.",
                        plural(c, sizeof c, n, "change"));
        ui_msg(t);
    }
}

static void load_pick(int i)
{
    static char t[160];
    uint32_t rev;
    const char *at;
    load_sel = load_from[i];
    if (load_sel == CFG_FROM_DEFAULTS) {
        load_do();
        return;
    }
    char w[24];
    cfg_rev(load_sel, &rev, &at);
    lv_snprintf(t, sizeof t, "Load the %s save%s%s? Drafts are dropped.",
                load_sel == CFG_FROM_SAVED ? "last" : "previous", at[0] ? ", " : "",
                at[0] ? save_when(w, sizeof w, at) : "");
    ui_confirm(t, "Load", load_do);
}

static void load_cb(lv_event_t *e)
{
    (void)e;
    static char names[3][40];
    static const char *items[3];
    char w[24];
    uint32_t rev;
    const char *at;
    load_n = 0;
    if (valve_auto()) {
        ui_msg("Stop AUTO first");
        return;
    }
    if (stepper3_busy() || valve_mode() == VALVE_CALIBRATING) {
        ui_msg("Wait for the valve to stop");
        return;
    }
    if (cfg_rev(CFG_FROM_SAVED, &rev, &at)) {
        lv_snprintf(names[load_n], sizeof names[0], "Last save, %s", save_when(w, sizeof w, at));
        load_from[load_n++] = CFG_FROM_SAVED;
    }
    if (cfg_rev(CFG_FROM_PREVIOUS, &rev, &at)) {
        lv_snprintf(names[load_n], sizeof names[0], "Previous save, %s", save_when(w, sizeof w, at));
        load_from[load_n++] = CFG_FROM_PREVIOUS;
    }
    lv_snprintf(names[load_n], sizeof names[0], "Factory defaults");
    load_from[load_n++] = CFG_FROM_DEFAULTS;
    for (int i = 0; i < load_n; i++)
        items[i] = names[i];
    ui_pick("Load which settings?", items, load_n, -1, load_pick);
}

static const char *rv_help(void)
{
    return "Last saved, Previous save: the two stored records.\n"
           "Save settings: applies drafts and saves.\n"
           "Load settings: a save loads live; factory loads as drafts, calibration kept.\n"
           "Discard changes: back to Last saved.";
}

static void rv_build(lv_obj_t *scr)
{
    // Save settings and Discard changes in the same places as on the other pages.
    rv.saved = ui_info(scr, C2X[0], RY[0], C2W[0], RH, "Last saved");
    rv.prev = ui_info(scr, C2X[1], RY[0], C2W[1], RH, "Previous save");
    ecard_make(&rv.changes, scr, 10, RY[1], 343, "Unsaved changes", changes_cb, NULL);
    rv.load = ui_btn(scr, 10, RY[2], 343, RH, UI_SECONDARY, "Load settings", load_cb, NULL);
    ui_btn_text(rv.load, "Load settings", "last, previous or factory");
    side_make(scr, "Review and save", rv_help);
}

static const char *rev_text(char *buf, size_t n, cfg_from_t from)
{
    uint32_t rev;
    const char *at;
    if (!cfg_rev(from, &rev, &at))
        return "None";
    // Saves from 0.8.x have no date; the caption tells the two apart.
    return at[0] ? save_when(buf, n, at) : "Date unknown";
}

static void rv_refresh(void)
{
    char t[48], c[24];
    int n = unsaved(), d = drafts(), u = n - d;
    // The state in words, not only the dark / light edge (review of 0.10.11).
    if (d && u > 0)
        lv_snprintf(t, sizeof t, "%s, %d in use not saved", plural(c, sizeof c, d, "draft"), u);
    else if (d)
        lv_snprintf(t, sizeof t, "%s, not in use yet", plural(c, sizeof c, d, "draft"));
    else if (u > 0)
        lv_snprintf(t, sizeof t, "%d in use, not saved", u);
    else
        lv_strlcpy(t, "None", sizeof t);
    ecard_state(&rv.changes, t, d > 0, n > 0 && !d);
    ui_set_text(rv.saved, rev_text(t, sizeof t, CFG_FROM_SAVED));
    ui_set_text(rv.prev, rev_text(t, sizeof t, CFG_FROM_PREVIOUS));
    side_refresh();
}

const ui_screen_t ui_scr_review = { "Review and save", rv_build, rv_refresh, NULL };

/* --------------------------------------------------------------------------
 * Calibrate sensor (Menu > Pressure setup): CAL VAC / CAL AIR on the screen.
 * The new cal is in use once both points are held, unsaved until Save
 * settings (as on COM3). One sensor per board from 0.16.2: a new sensor is
 * calibrated here, nothing else to choose (the header named the serial,
 * "Calibrate 5880156", to 0.16.1).
 * ------------------------------------------------------------------------ */
static struct {
    lv_obj_t *sig, *now, *nw, *vac, *air, *bv, *ba;
} cs;

// "0.0 at 0.0128": a cal point, mB at mV/V.
static const char *pt_text(char *buf, size_t n, bool have, int32_t p, int32_t r)
{
    char a[16], b[16];
    if (!have)
        return "Not held";
    lv_snprintf(buf, n, "%s at %s", fixed_fmt(a, sizeof a, fixed_round(p, 2), 1), fixed_fmt(b, sizeof b, fixed_round(r, 2), 4));
    return buf;
}

static const char *cs_help(void)
{
    return "Two-point calibration.\n"
           "Capture vacuum: sensor pumped down (0 mB, or enter a gauge reading).\n"
           "Capture air: sensor vented; Pressure 21 or a typed barometer reading.\n"
           "The new cal is live but unsaved: Save settings or Discard changes.";
}

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

static int32_t now_milli(void)
{
    return reading_ok() ? clicks_state()->p_mmbar : 0;
}

static void cs_vac_typed(void)
{
    ui_kp_t k = { .title = "Vacuum gauge, mB", .info = "Only if a gauge reads above 0", .dec = 2,
                  .min = 0, .max = 5000, .live_label = "Now, mB", .live_milli = now_milli, .live_dec = 1,
                  .on_set = cs_vac_set };
    ui_keypad_ex(&k);
}

static void vac_cb(lv_event_t *e)
{
    (void)e;
    if (ui_btn_disabled(e)) {
        ui_msg(cal_capturing() ? "A capture is running" : "No pressure signal");
        return;
    }
    ui_ask("Sensor at vacuum? Capture sets 0 mB.",
           "Not 0 mB", cs_vac_typed, "Capture", cs_vac0);
}

static bool cs_air_set(int32_t v10, const char **why)
{
    (void)why;
    cs_start(2, v10 * 100);
    return true;
}

static void cs_air_typed(void)
{
    ui_kp_t k = { .title = "Barometer reading, mB", .info = "Vent sensor to room air first", .dec = 1,
                  .min = 5000, .max = 11000, .live_label = "Now, mB", .live_milli = now_milli, .live_dec = 1,
                  .on_set = cs_air_set };
    ui_keypad_ex(&k);
}

static void cs_air_baro(void) { cs_start(2, CAL_P_BARO); }

static void air_cb(lv_event_t *e)
{
    if (ui_btn_disabled(e)) {
        ui_msg(cal_capturing() ? "A capture is running" : "No pressure signal");
        return;
    }
    // With the Pressure 21 fitted the room pressure is read, not typed
    // (0.14.0); Enter value keeps the lab barometer possible.
    const clicks_state_t *s = clicks_state();
    if (!s->baro_ok) {
        cs_air_typed();
        return;
    }
    static char q[96];
    char v[16];
    lv_snprintf(q, sizeof q, "Sensor open to room air? Capture uses the room sensor, now %s mB.",
                ui_fmt1(v, sizeof v, s->baro_mmbar));
    ui_ask(q, "Enter value", cs_air_typed, "Capture", cs_air_baro);
}

static void cs_build(lv_obj_t *scr)
{
    cs.sig = ui_info(scr, 10, RY[0], 109, RH, "Signal, mV/V");
    cs.now = ui_info(scr, 127, RY[0], 109, RH, "Saved cal, mB");
    cs.nw = ui_info(scr, 244, RY[0], 109, RH, "Captured, mB");
    cs.vac = ui_info(scr, 10, RY[1], 226, RH, "Vacuum point, mB at mV/V");
    cs.air = ui_info(scr, 10, RY[2], 226, RH, "Air point, mB at mV/V");
    cs.bv = ui_btn(scr, 244, RY[1], 109, RH, UI_SECONDARY, "Capture\nvacuum", vac_cb, NULL);
    cs.ba = ui_btn(scr, 244, RY[2], 109, RH, UI_SECONDARY, "Capture\nair", air_cb, NULL);   // both on two lines
    side_make(scr, "Calibrate sensor", cs_help);
}

static void cs_refresh(void)
{
    char t[40];
    const clicks_state_t *s = clicks_state();
    bool ok = reading_ok();
    ui_set_text(cs.sig, ok ? fixed_fmt(t, sizeof t, fixed_round(s->r_ppb, 2), 4) : "----");   // 9.3580
    const cal_rec_t *saved = NULL;
    bool in_save = cfg_saved_cal(&saved);   // a save exists; saved NULL if it holds the nominal
    bool as_saved = cal_saved();             // the cal in use is the saved one
    int64_t p_saved = saved ? cal_pressure_with(saved, s->r_ppb)
                    : (int64_t)s->r_ppb * cfg_sensor_range_mmbar() / 10000000;   // nominal: 0 to 10 mV/V
    cal_point_t v, a;
    cal_points(&v, &a);
    // Saved cal: none before the first save. Captured: only once both
    // points are held and the cal in use is not the saved one.
    ui_set_text(cs.now, !ok ? "----" : !in_save ? "None" : ui_fmt1(t, sizeof t, as_saved ? s->p_mmbar : p_saved));
    ui_set_text(cs.nw, !ok ? "----" : v.have && a.have && !as_saved ? ui_fmt1(t, sizeof t, s->p_mmbar) : "None yet");
    int cap = cal_capturing(), left = cal_capture_left();
    if (cap == 1)
        lv_snprintf(t, sizeof t, "Capturing %d of %d", CAL_CAPTURE_N - left, CAL_CAPTURE_N);
    ui_set_text(cs.vac, cap == 1 ? t : pt_text(t, sizeof t, v.have, v.p_mmbar, v.r_ppb));
    char u[40];
    if (cap == 2)
        lv_snprintf(u, sizeof u, "Capturing %d of %d", CAL_CAPTURE_N - left, CAL_CAPTURE_N);
    ui_set_text(cs.air, cap == 2 ? u : pt_text(u, sizeof u, a.have, a.p_mmbar, a.r_ppb));
    ui_btn_enable(cs.bv, ok && !cap);
    ui_btn_enable(cs.ba, ok && !cap);
    side_refresh();
}

const ui_screen_t ui_scr_sensor_cal = { "Calibrate sensor", cs_build, cs_refresh, NULL };
