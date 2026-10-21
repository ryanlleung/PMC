#include <string.h>

#include "lvgl.h"
#include "clicks.h"
#include "cal.h"
#include "cfg.h"
#include "fixed.h"
#include "stepper3.h"
#include "valve.h"
#include "ui.h"

/* --------------------------------------------------------------------------
 * Settings (stage 6, simulator v63): Settings, Choose sensor, Valve
 * setup, Control tuning, Review and save, and Calibrate sensor (Menu >
 * Calibrate > Sensor).
 *
 * An edit is a draft (violet edge and value) until Save settings, which
 * checks the drafts and the settings in use together, puts them in use all
 * or nothing, and writes the whole record (cfg_save_with).
 * A grey edge is a setting in use that differs from the last save (a
 * capture, an added sensor, a COM3 command or Load settings). Load
 * settings > Previous save is the undo.
 * ------------------------------------------------------------------------ */

#define COL_DRAFT  lv_color_hex(0x8050C0)
#define COL_GREY   lv_color_hex(0x505050)
#define STEPS_PER_TURN 16512   // 48 steps x 344:1 gearbox

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

// A save is named by when it was made: "16/10/26 14:32", or "no date"
// when the clock was not set at the time.
static const char *save_when(char *buf, size_t n, const char *at)
{
    return at[0] ? ui_date(buf, n, at) : "no date";
}

static const char *plural(char *buf, size_t n, int count, const char *word)
{
    lv_snprintf(buf, n, "%d %s%s", count, word, count == 1 ? "" : "s");
    return buf;
}

/* --------------------------------------------------------------------------
 * Fields
 * ------------------------------------------------------------------------ */
typedef enum { K_NUM, K_TURNS, K_TOGGLE, K_CHOICE } kind_t;

typedef struct {
    const char *cap, *unit;
    uint8_t dec;
    uint8_t kind;
    const char *info;
} field_t;

// In cfg_key_t order.
static const field_t F[CFG_N] = {
    [CFG_POSMAX]  = { "100 % travel", "turns", 2, K_TURNS, "Output turns from closed to fully open" },
    [CFG_REVERSE] = { "Motor direction", NULL, 0, K_TOGGLE, "Reversed if Open drives the valve closed; saving it clears the zero" },
    [CFG_OFFSET]  = { "Zero offset", "steps", 0, K_NUM, "Count set at the closed limit" },
    [CFG_BACKOFF] = { "Back-off", "steps", 0, K_NUM, "Homing step 2: off the switch before the slow close" },
    [CFG_FHOME]   = { "Fast close", "steps/s", 0, K_NUM, "Homing step 1: closing fast to the switch" },
    [CFG_FCREEP]  = { "Slow close", "steps/s", 0, K_NUM, "Homing step 3: closing slowly onto the switch" },
    [CFG_RATE]    = { "Run speed", "steps/s", 0, K_NUM, "Normal move speed" },
    [CFG_START]   = { "Start speed", "steps/s", 0, K_NUM, "Speed at the start and end of a move" },
    [CFG_ACCEL]   = { "Acceleration", "steps/s/s", 0, K_NUM, "Ramp between start and run" },
    [CFG_KP]      = { "Gain", "%/mB", 2, K_NUM, "% open per mB of error" },
    [CFG_TI]      = { "Integral time", "min", 2, K_NUM, "Integral time, 0 is off" },
    [CFG_OUTMIN]  = { "Auto min", "% open", 1, K_NUM, "AUTO never closes the valve below this" },
    [CFG_DB]      = { "Deadband", "steps", 0, K_NUM, "AUTO skips moves smaller than this" },
    [CFG_KDIR]    = { "Pressure response", NULL, 0, K_TOGGLE, "How the pressure moves when the valve opens; a needle valve feeding the pot normally raises it" },
    [CFG_BAND]    = { "Target band", "mB", 2, K_NUM, "Within this of the target reads Within target band" },
    [CFG_SENSOR]  = { "Sensor in use", NULL, 0, K_CHOICE, "The sensor read for the pressure" },
};

static const char *psi_text(char *buf, size_t n, int32_t psi10)
{
    if (psi10 % 10)
        lv_snprintf(buf, n, "%ld.%ld psi", (long)(psi10 / 10), (long)(psi10 % 10));
    else
        lv_snprintf(buf, n, "%ld psi", (long)(psi10 / 10));
    return buf;
}

static int sensor_index(uint32_t serial)
{
    cfg_sensor_t s;
    for (int i = 0; cfg_sensor(i, &s); i++)
        if (s.serial == serial)
            return i;
    return -1;
}

// "15 psi, s/n 5880156"
static const char *sensor_name(char *buf, size_t n, uint32_t serial)
{
    cfg_sensor_t s;
    char p[16];
    if (sensor_index(serial) >= 0 && cfg_sensor(sensor_index(serial), &s))
        lv_snprintf(buf, n, "%s, s/n %lu", psi_text(p, sizeof p, s.psi10), (unsigned long)serial);
    else
        lv_snprintf(buf, n, "s/n %lu", (unsigned long)serial);
    return buf;
}

static const char *value_text(char *buf, size_t n, cfg_key_t k, int32_t v)
{
    switch (F[k].kind) {
    case K_TURNS:
        return fixed_fmt(buf, n, fixed_round((int64_t)v * 1000 / STEPS_PER_TURN, 1), 2);
    case K_TOGGLE:
        if (k == CFG_REVERSE)
            return v ? "Reversed" : "Normal";
        return v > 0 ? "Opening raises" : "Opening lowers";
    case K_CHOICE:
        return sensor_name(buf, n, (uint32_t)v);
    default:
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
        lv_snprintf(buf + used, n - used, "%s: %s.\n", F[keys[i]].cap, F[keys[i]].info);
        used += strlen(buf + used);
    }
    if (used && buf[used - 1] == '\n')
        buf[used - 1] = '\0';
    return buf;
}

// "16/10/26" from a bench cal id "5880156 bench 26-10-16"; NULL if none.
static const char *cal_date(char *buf, size_t n, const cal_rec_t *c)
{
    const char *d = c ? strstr(c->id, "bench ") : NULL;
    if (!d || strlen(d) < 14)
        return NULL;
    d += 6;
    int m = (d[3] - '0') * 10 + d[4] - '0';
    int day = (d[6] - '0') * 10 + d[7] - '0';
    if (m < 1 || m > 12 || day < 1 || day > 31)
        return NULL;
    lv_snprintf(buf, n, "%.2s/%.2s/%.2s", d + 6, d + 3, d);   // DD/MM/YY
    return buf;
}

/* Editable card: white, outlined, 12 px caption, 16 px value, chevron; a
 * 3 px edge on the left for draft (violet) or unsaved (grey). */
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
    ui_label(e->card, 10, 5, w - 30, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT, cap);
    e->v = ui_label(e->card, 10, 22, w - 30, &ui_font_16, UI_TEXT, LV_TEXT_ALIGN_LEFT, "");
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
    ui_set_color(e->v, draft ? COL_DRAFT : UI_TEXT);
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
    static char info[32], now[16], sv[16];
    int32_t lo, hi, s;
    cfg_range(CFG_POSMAX, &lo, &hi);
    lv_snprintf(info, sizeof info, "%d steps per output turn", STEPS_PER_TURN);
    ui_kp_t k = { .title = "100 % travel, output turns", .info = info, .dec = 2,
                  .min = (int32_t)(((int64_t)lo * 100 + STEPS_PER_TURN - 1) / STEPS_PER_TURN),
                  .max = (int32_t)((int64_t)hi * 100 / STEPS_PER_TURN), .on_set = ed_turns };
    kp_rows(&k, CFG_POSMAX, now, sv, sizeof now);
    (void)s;
    ui_keypad_ex(&k);
}

static void ed_turns_present(void)
{
    set_draft(CFG_POSMAX, stepper3_position());
}

static void ed_toggle_ok(void)
{
    set_draft(ed_key, ed_key == CFG_REVERSE ? !shown(ed_key) : shown(ed_key) > 0 ? -1 : 1);
}

static void sensor_pick(int i);

static void edit(cfg_key_t k)
{
    static char t[160], cap[40], info[40], now[24], sv[24];
    char a[16], b[16];
    ed_key = k;
    if (F[k].kind == K_TURNS) {
        int32_t lo, hi, pos = stepper3_position();
        cfg_range(CFG_POSMAX, &lo, &hi);
        if (!valve_calibrated() || stepper3_busy() || pos < lo || pos > hi) {
            ed_turns_keypad();
            return;
        }
        lv_snprintf(t, sizeof t, "100 %% travel is %s turns (%ld steps). Type a value, or use the present "
                    "position (%s turns)?", value_text(a, sizeof a, k, shown(k)), (long)shown(k),
                    value_text(b, sizeof b, k, pos));
        ui_ask(t, "Present", ed_turns_present, "Type turns", ed_turns_keypad);
    } else if (F[k].kind == K_TOGGLE) {
        if (k == CFG_REVERSE)
            lv_snprintf(t, sizeof t, "Set motor direction to %s? Use Reversed if Open drives the valve closed. "
                        "Saving it clears the zero.", shown(k) ? "Normal" : "Reversed");
        else
            lv_snprintf(t, sizeof t, "Set pressure response to %s? A needle valve feeding the pot normally "
                        "raises it.", shown(k) > 0 ? "Opening lowers pressure" : "Opening raises pressure");
        ui_confirm(t, "Change", ed_toggle_ok);
    } else if (F[k].kind == K_CHOICE) {
        static char names[CFG_SENS_MAX][40];
        static const char *items[CFG_SENS_MAX];
        cfg_sensor_t s;
        int n = 0, sel = -1;
        for (int i = 0; cfg_sensor(i, &s) && n < CFG_SENS_MAX; i++, n++) {
            sensor_name(names[n], sizeof names[n], s.serial);
            if (!s.has_cal)
                lv_strlcpy(names[n] + strlen(names[n]), ", not calibrated", sizeof names[n] - strlen(names[n]));
            items[n] = names[n];
            if ((int32_t)s.serial == shown(CFG_SENSOR))
                sel = n;
        }
        ui_pick("Sensor in use", items, n, sel, sensor_pick);
    } else {
        int32_t lo, hi, s;
        cfg_range(k, &lo, &hi);
        lv_snprintf(info, sizeof info, "Range %s to %s", fixed_fmt(a, sizeof a, lo, F[k].dec),
                    fixed_fmt(b, sizeof b, hi, F[k].dec));
        ui_kp_t kp = { .title = caption(cap, sizeof cap, k), .info = info, .dec = F[k].dec, .min = lo, .max = hi,
                       .on_set = ed_set };
        kp_rows(&kp, k, now, sv, sizeof now);
        (void)s;
        ui_keypad_ex(&kp);
    }
}

static void sensor_pick(int i)
{
    cfg_sensor_t s;
    if (cfg_sensor(i, &s))
        set_draft(CFG_SENSOR, (int32_t)s.serial);
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
    // refuses anything else with "Switch to MANUAL first").
    if (!valve_auto() && (stepper3_busy() || valve_mode() == VALVE_CALIBRATING)) {
        ui_msg("Wait for the valve to stop");
        return;
    }
    (void)rev; (void)at; (void)w;
    lv_snprintf(t, sizeof t, "Save %s? They are used now and after restart.", plural(c, sizeof c, n, "change"));
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
        lv_snprintf(t, sizeof t, "Discard %s? Settings in use are not affected.",
                    plural(c, sizeof c, n, "unsaved change"));
        ui_confirm(t, "Discard changes", discard_drafts);
    } else if (cfg_unsaved() && cfg_rev(CFG_FROM_SAVED, &rev, &at)) {
        char w[24];
        lv_snprintf(t, sizeof t, "Put the last save%s%s back in use? The %s in use since then %s lost.",
                    at[0] ? ", " : "", at[0] ? save_when(w, sizeof w, at) : "", plural(c, sizeof c, cfg_unsaved(), "change"),
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
        lv_snprintf(t, sizeof t, "All settings saved%s%s.", at[0] ? ", " : "", at[0] ? save_when(w, sizeof w, at) : "");
    else
        lv_snprintf(t, sizeof t, "Factory settings, nothing saved yet.");
    ui_strip_set(UI_STRIP_INFO, drafts() ? COL_DRAFT : n ? COL_GREY : UI_MUTED, t);
}

/* --------------------------------------------------------------------------
 * Settings
 * ------------------------------------------------------------------------ */
extern const ui_screen_t ui_scr_name, ui_scr_sensor_set, ui_scr_valve_set, ui_scr_tune, ui_scr_review;

static struct {
    lv_obj_t *tile[5], *over;
} st;

static void tile_cb(lv_event_t *e)
{
    ui_open(lv_event_get_user_data(e));
}

static void override_on(void)
{
    stepper3_override_arm();
    valve_event("LIM", "Limit override on for the next move (screen)");
}

static void override_cb(lv_event_t *e)
{
    (void)e;
    if (stepper3_override()) {
        bool stopped = stepper3_override_cancel();
        valve_event("LIM", "Limit override off (screen)%s", stopped ? ", valve stopped" : "");
        ui_msg(stopped ? "Limit override off, valve stopped." : "Limit override off.");
        return;
    }
    ui_confirm("Let the next move close past the closed switch? Only for a stuck switch: the valve can be "
               "driven into the seat. Ends when that move stops.", "Override", override_on);
}

static void motor_go(void) { ui_service(); }

static void motor_cb(lv_event_t *e)
{
    (void)e;
    if (stepper3_busy()) {
        ui_msg("Wait for the valve to stop");
        return;
    }
    ui_confirm("Open Motor override? It drives the motor directly by steps.", "Open", motor_go);
}

static void settings_build(lv_obj_t *scr)
{
    static const char *const names[5] = { "Cryostat name", "Choose sensor", "Valve setup", "Control tuning",
                                          "Review and save" };
    const ui_screen_t *const to[5] = { &ui_scr_name, &ui_scr_sensor_set, &ui_scr_valve_set, &ui_scr_tune,
                                       &ui_scr_review };
    for (int i = 0; i < 5; i++)
        st.tile[i] = ui_btn(scr, i % 2 ? 244 : 10, RY[i / 2], 226, RH, UI_NAV, names[i], tile_cb, (void *)to[i]);
    // The overrides look apart from the pages: a dark outline, and Limit
    // override filled dark while it is armed.
    st.over = ui_btn(scr, 244, RY[2], 109, RH, UI_SECONDARY, "Limit\noverride", override_cb, NULL);
    lv_obj_t *m = ui_btn(scr, 361, RY[2], 109, RH, UI_SECONDARY, "Motor\noverride", motor_cb, NULL);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = i ? m : st.over;
        lv_obj_set_style_border_side(b, LV_BORDER_SIDE_FULL, 0);
        lv_obj_set_style_border_width(b, 2, 0);
        lv_obj_set_style_border_color(b, UI_WARN, 0);
    }
    side.save = side.discard = NULL;
}

static void settings_refresh(void)
{
    char t[48], p[32], d[24];
    cfg_sensor_t s;
    // Which sensor is read; calibrating it is Menu > Calibrate > Sensor.
    if (cfg_sensor(cfg_sensor_active(), &s)) {
        lv_snprintf(t, sizeof t, "In use: %s", sensor_name(p, sizeof p, s.serial));
        ui_btn_text(st.tile[1], "Choose sensor", t);
    }
    ui_btn_text(st.tile[0], "Cryostat name", cfg_name()[0] ? cfg_name() : "None");
    lv_snprintf(t, sizeof t, "%s turns", value_text(d, sizeof d, CFG_POSMAX, live(CFG_POSMAX)));
    ui_btn_text(st.tile[2], "Valve setup", t);
    lv_snprintf(t, sizeof t, "Gain %s", fixed_fmt(d, sizeof d, live(CFG_KP), 2));
    ui_btn_text(st.tile[3], "Control tuning", t);
    bool ov = stepper3_override();
    // Armed shows as the dark fill and the strip; the state line would not fit.
    ui_btn_text(st.over, ov ? "Cancel\noverride" : "Limit\noverride", NULL);
    lv_color_t ob = ov ? UI_WARN : UI_CARD;
    if (!lv_color_eq(lv_obj_get_style_bg_color(st.over, 0), ob)) {
        lv_obj_set_style_bg_color(st.over, ob, 0);
        // Its two labels: name and state line, white on the dark fill.
        for (uint32_t i = 0; i < lv_obj_get_child_count(st.over); i++)
            lv_obj_set_style_text_color(lv_obj_get_child(st.over, (int32_t)i),
                                        ov ? lv_color_white() : i ? UI_MUTED : UI_TEXT, 0);
    }
    // Without side buttons: the strip only.
    char c[32], w[24];
    uint32_t rev;
    const char *at;
    int n = unsaved();
    if (drafts())
        lv_snprintf(t, sizeof t, "%s. Save settings to use.", plural(c, sizeof c, n, "unsaved change"));
    else if (n)
        lv_snprintf(t, sizeof t, "%s in use, not saved.", plural(c, sizeof c, n, "change"));
    else if (cfg_rev(CFG_FROM_SAVED, &rev, &at))
        lv_snprintf(t, sizeof t, "All settings saved%s%s.", at[0] ? ", " : "", at[0] ? save_when(w, sizeof w, at) : "");
    else
        lv_snprintf(t, sizeof t, "Factory settings, nothing saved yet.");
    ui_strip_set(ov ? UI_STRIP_WARN : UI_STRIP_INFO, drafts() ? COL_DRAFT : n ? COL_GREY : UI_MUTED,
                 ov ? "Limit override on for the next move" : t);
}

const ui_screen_t ui_scr_settings = { "Settings", settings_build, settings_refresh, NULL };

/* --------------------------------------------------------------------------
 * Cryostat name (Settings > Cryostat name): a keyboard for the name in the
 * Home title, "<name> pressure controller". Use name puts it in use at
 * once, unsaved until Save settings, like a sensor change.
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

static void nm_build(lv_obj_t *scr)
{
    lv_strlcpy(nm.buf, cfg_name(), sizeof nm.buf);
    nm.kb = lv_buttonmatrix_create(scr);
    lv_obj_set_pos(nm.kb, 10, RY[0]);
    lv_obj_set_size(nm.kb, 343, 160);
    lv_buttonmatrix_set_map(nm.kb, nm_map);
    // Bottom row: Z to M as the keys above, Space and Del wider.
    for (uint32_t i = 30; i < 37; i++)
        lv_buttonmatrix_set_button_width(nm.kb, i, 4);
    lv_buttonmatrix_set_button_width(nm.kb, 37, 7);
    lv_buttonmatrix_set_button_width(nm.kb, 38, 5);
    lv_obj_set_style_bg_opa(nm.kb, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(nm.kb, 0, 0);
    lv_obj_set_style_radius(nm.kb, 0, 0);
    lv_obj_set_style_pad_all(nm.kb, 0, 0);
    lv_obj_set_style_pad_gap(nm.kb, 6, 0);
    lv_obj_set_style_bg_opa(nm.kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(nm.kb, UI_CARD, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(nm.kb, lv_color_hex(0xC0C0C0), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_radius(nm.kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_border_side(nm.kb, LV_BORDER_SIDE_BOTTOM, LV_PART_ITEMS);
    lv_obj_set_style_border_width(nm.kb, 2, LV_PART_ITEMS);
    lv_obj_set_style_border_width(nm.kb, 0, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_border_color(nm.kb, lv_color_hex(0x202020), LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(nm.kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_text_font(nm.kb, &ui_font_16, LV_PART_ITEMS);
    lv_obj_set_style_text_color(nm.kb, UI_TEXT, LV_PART_ITEMS);
    lv_obj_add_event_cb(nm.kb, nm_key, LV_EVENT_VALUE_CHANGED, NULL);
    nm.text = ui_info(scr, 361, RY[0], 109, RH, "Name");
    nm.use = ui_btn(scr, 361, RY[1], 109, RH, UI_PRIMARY, "Use name", nm_use, NULL);
    ui_btn(scr, 361, RY[2], 109, RH, UI_SECONDARY, "Cancel", nm_cancel, NULL);
    nm_show();
}

static void nm_refresh(void)
{
    ui_strip_set(UI_STRIP_INFO, UI_MUTED, "Shown on Home. Save settings to keep it.");
}

const ui_screen_t ui_scr_name = { "Cryostat name", nm_build, nm_refresh, NULL };

/* --------------------------------------------------------------------------
 * Choose sensor
 * ------------------------------------------------------------------------ */
static struct {
    ecard_t use, cal;
    lv_obj_t *rm;
} ps;

static const char *ps_help(void)
{
    static char buf[600];
    static const cfg_key_t k[1] = { CFG_SENSOR };
    return field_help(buf, sizeof buf,
                      "Add sensor: type its serial number and pick its range in psi. It starts uncalibrated: "
                      "Calibrate puts it in use and opens Calibrate sensor; Later leaves it to select "
                      "in Sensor in use.\n"
                      "Remove sensor: removes a sensor that is not in use.\n"
                      "Calibration: tap for its points and state.", k, 1);
}

// The sensor that will be in use after Save.
static bool ps_sensor(cfg_sensor_t *s)
{
    int i = sensor_index((uint32_t)shown(CFG_SENSOR));
    return cfg_sensor(i < 0 ? cfg_sensor_active() : i, s);
}

static const char *cal_short(char *buf, size_t n, const cfg_sensor_t *s)
{
    char d[20];
    if (!s->has_cal || !s->cal)
        return "Not calibrated";
    if (s->cal->kind == CAL_KIND_BENCH)
        lv_snprintf(buf, n, cal_date(d, sizeof d, s->cal) ? "Two-point, %s" : "Two-point", d);
    else
        lv_snprintf(buf, n, "Table, %lu points", (unsigned long)s->cal->npts);
    return buf;
}

static const char *pt_text(char *buf, size_t n, bool have, int32_t p, int32_t r)
{
    char a[16], b[16];
    if (!have)
        return "Not held";
    lv_snprintf(buf, n, "%s at %s", fixed_fmt(a, sizeof a, fixed_round(p, 2), 1), fixed_fmt(b, sizeof b, fixed_round(r, 2), 4));
    return buf;
}

static void cal_detail_cb(lv_event_t *e)
{
    (void)e;
    static char t[400], title[40];
    char k[48], v[32], a[40], d[20];
    cfg_sensor_t s;
    if (!ps_sensor(&s))
        return;
    lv_snprintf(title, sizeof title, "Calibration, s/n %lu", (unsigned long)s.serial);
    if (!s.has_cal || !s.cal) {
        char fs[16];
        lv_snprintf(t, sizeof t, "Kind: Nominal, 0 to 10 mV/V is 0 to %s mB\nState: %s",
                    ui_fmt1(fs, sizeof fs, s.fs_mmbar), s.saved ? "Saved" : "In use, not saved");
    } else {
        const cal_rec_t *c = s.cal;
        bool bench = c->kind == CAL_KIND_BENCH && c->npts == 2;
        lv_snprintf(k, sizeof k, bench ? "Two-point bench" : "%lu-point table", (unsigned long)c->npts);
        lv_snprintf(t, sizeof t, "Identity: %s\nDate: %s\nKind: %s", c->id,
                    cal_date(d, sizeof d, c) ? d : "None recorded", k);
        if (bench) {
            size_t u = strlen(t);
            lv_snprintf(t + u, sizeof t - u, "\nVacuum point, mB at mV/V: %s\nAir point, mB at mV/V: %s",
                        pt_text(v, sizeof v, true, c->p_mmbar[0], c->r_ppb[0]),
                        pt_text(a, sizeof a, true, c->p_mmbar[1], c->r_ppb[1]));
        }
        size_t u = strlen(t);
        lv_snprintf(t + u, sizeof t - u, "\nState: %s", s.saved ? "Saved" : s.in_use ? "In use, not saved" : "Not saved");
    }
    ui_text_view(title, t);
}

static uint32_t add_serial;

// After Add sensor: the new sensor in use now, then Calibrate sensor.
static void add_cal(void)
{
    const char *why;
    char t[80];
    if (!cfg_sensor_use(add_serial, &why)) {
        lv_snprintf(t, sizeof t, "Not in use: %s", why);
        ui_msg(t);
        return;
    }
    draft_set[CFG_SENSOR] = false;
    ui_open(&ui_scr_sensor_cal);
}

static void add_range(int i)
{
    static const int32_t psi10[6] = { 50, 100, 150, 300, 500, 1000 };
    const char *why;
    char t[80], n[40];
    if (!cfg_sensor_add(add_serial, psi10[i], &why)) {
        lv_snprintf(t, sizeof t, "Not added: %s", why);
        ui_msg(t);
        return;
    }
    static char q[140];
    lv_snprintf(q, sizeof q, "%s added. Calibrate it now? It goes in use straight away, not saved; "
                "Discard changes puts the last save back.", sensor_name(n, sizeof n, add_serial));
    ui_ask(q, "Later", NULL, "Calibrate", add_cal);
}

static bool add_serial_set(int32_t v, const char **why)
{
    static const char *const ranges[6] = { "5 psi", "10 psi", "15 psi", "30 psi", "50 psi", "100 psi" };
    static char title[40];
    if (sensor_index((uint32_t)v) >= 0) { *why = "That serial number is already on the board"; return false; }
    if (cfg_sensor_count() >= CFG_SENS_MAX) { *why = "Four sensors already on the board"; return false; }
    add_serial = (uint32_t)v;
    lv_snprintf(title, sizeof title, "Range of s/n %lu", (unsigned long)add_serial);
    ui_pick(title, ranges, 6, -1, add_range);
    return true;
}

static void add_cb(lv_event_t *e)
{
    (void)e;
    if (cfg_sensor_count() >= CFG_SENS_MAX) {
        ui_msg("Four sensors already on the board");
        return;
    }
    ui_kp_t k = { .title = "New sensor serial number", .info = "On the Druck label", .dec = 0,
                  .min = 1, .max = 99999999, .on_set = add_serial_set };
    ui_keypad_ex(&k);
}

static uint32_t rm_list[CFG_SENS_MAX], rm_serial;

static void rm_do(void)
{
    const char *why;
    char t[80], n[40];
    sensor_name(n, sizeof n, rm_serial);
    if (!cfg_sensor_remove(rm_serial, &why)) {
        lv_snprintf(t, sizeof t, "Not removed: %s", why);
        ui_msg(t);
        return;
    }
    lv_snprintf(t, sizeof t, "%s removed.", n);
    ui_msg(t);
}

static void rm_pick(int i)
{
    static char t[140];
    char n[40];
    rm_serial = rm_list[i];
    lv_snprintf(t, sizeof t, "Remove %s and its calibration from the board? The last save keeps it until "
                "Save settings.", sensor_name(n, sizeof n, rm_serial));
    ui_confirm(t, "Remove", rm_do);
}

static void rm_cb(lv_event_t *e)
{
    static char names[CFG_SENS_MAX][40];
    static const char *items[CFG_SENS_MAX];
    (void)e;
    cfg_sensor_t s;
    int n = 0;
    for (int i = 0; cfg_sensor(i, &s); i++) {
        if (s.in_use || (int32_t)s.serial == shown(CFG_SENSOR))
            continue;
        rm_list[n] = s.serial;
        items[n] = sensor_name(names[n], sizeof names[n], s.serial);
        n++;
    }
    if (!n) {
        ui_msg("Only the sensor in use is on the board");
        return;
    }
    ui_pick("Remove which sensor?", items, n, -1, rm_pick);
}

static void ps_build(lv_obj_t *scr)
{
    ecard_make(&ps.use, scr, 10, RY[0], 226, "Sensor in use", field_event, (void *)(intptr_t)CFG_SENSOR);
    ui_btn(scr, 244, RY[0], 109, RH, UI_SECONDARY, "Add\nsensor", add_cb, NULL);   // two lines, as Remove sensor
    ecard_make(&ps.cal, scr, 10, RY[1], 226, "Calibration", cal_detail_cb, NULL);
    ps.rm = ui_btn(scr, 244, RY[1], 109, RH, UI_SECONDARY, "Remove sensor", rm_cb, NULL);
    side_make(scr, "Choose sensor", ps_help);
}

static void ps_refresh(void)
{
    char t[40];
    cfg_sensor_t s;
    field_update(&ps.use, CFG_SENSOR);
    if (ps_sensor(&s))
        ecard_state(&ps.cal, cal_short(t, sizeof t, &s), false, !s.saved);
    ui_btn_enable(ps.rm, cfg_sensor_count() > 1);
    side_refresh();
}

const ui_screen_t ui_scr_sensor_set = { "Choose sensor", ps_build, ps_refresh, NULL };

/* --------------------------------------------------------------------------
 * Valve setup (three tabs) and Control tuning
 * ------------------------------------------------------------------------ */
typedef struct {
    const char *label;
    cfg_key_t keys[6];
    int n;
    const char *note;
} tab_t;

static const tab_t valve_tabs[3] = {
    { "Valve", { CFG_POSMAX, CFG_REVERSE }, 2, NULL },
    { "Homing", { CFG_BACKOFF, CFG_FHOME, CFG_FCREEP }, 3, NULL },
    { "Drive", { CFG_RATE, CFG_START, CFG_ACCEL }, 3, "Used by manual and AUTO moves. Saved when the valve is stopped." },
};
static const tab_t tune_tab[1] = {
    { "Control", { CFG_KP, CFG_TI, CFG_OUTMIN, CFG_DB, CFG_KDIR, CFG_BAND }, 6,
      "AUTO uses these from a later firmware; they can be set and saved now." },
};

static struct {
    const tab_t *tabs;
    int ntabs, tab;
    ecard_t f[6];
    lv_obj_t *seg, *segb[3];
    lv_obj_t *scr;
} pg;

static void pg_fields(void)
{
    // Rebuilt on a tab change: only one tab's cards exist at a time.
    for (int i = 0; i < 6; i++)
        if (pg.f[i].card) {
            lv_obj_delete(pg.f[i].card);
            pg.f[i].card = NULL;
        }
    const tab_t *t = &pg.tabs[pg.tab];
    int y0 = pg.ntabs > 1 ? 1 : 0;
    char cap[40];
    for (int i = 0; i < t->n; i++)
        ecard_make(&pg.f[i], pg.scr, C2X[i % 2], RY[y0 + i / 2], C2W[i % 2], caption(cap, sizeof cap, t->keys[i]),
                   field_event, (void *)(intptr_t)t->keys[i]);
}

static void seg_draw(void)
{
    for (int i = 0; i < pg.ntabs && pg.ntabs > 1; i++) {
        bool on = i == pg.tab;
        lv_obj_set_style_bg_opa(pg.segb[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_width(pg.segb[i], 0, 0);
        ui_set_color(lv_obj_get_child(pg.segb[i], 0), on ? UI_TEXT : UI_MUTED);
    }
}

static void seg_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i == pg.tab)
        return;
    pg.tab = i;
    seg_draw();
    pg_fields();
}

static const char *pg_help(void)
{
    static char buf[700];
    const tab_t *t = &pg.tabs[pg.tab];
    return field_help(buf, sizeof buf, t->note, t->keys, t->n);
}

static void pg_build(lv_obj_t *scr, const char *title, const tab_t *tabs, int n)
{
    memset(&pg, 0, sizeof pg);
    pg.scr = scr;
    pg.tabs = tabs;
    pg.ntabs = n;
    if (n > 1) {
        // Segmented choice: a grey track, the page shown as a raised white segment.
        pg.seg = ui_card(scr, 10, RY[0], 343, RH);
        lv_obj_set_style_bg_color(pg.seg, lv_color_hex(0xC0C0C0), 0);
        lv_obj_set_style_border_width(pg.seg, 0, 0);
        lv_obj_set_style_radius(pg.seg, 0, 0);
        int32_t iw = (343 - 8) / n;
        for (int i = 0; i < n; i++) {
            lv_obj_t *b = ui_card(pg.seg, 4 + i * iw, 4, iw, RH - 8);
            lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_radius(b, 0, 0);
            lv_obj_set_style_border_width(b, 0, 0);
            lv_obj_t *l = ui_label(b, 0, 0, iw, &ui_font_16, UI_TEXT, LV_TEXT_ALIGN_CENTER, tabs[i].label);
            lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
            lv_obj_add_event_cb(b, seg_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            pg.segb[i] = b;
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
        field_update(&pg.f[i], t->keys[i]);
    side_refresh();
}

static void valve_set_build(lv_obj_t *scr) { pg_build(scr, "Valve setup", valve_tabs, 3); }
static void tune_build(lv_obj_t *scr) { pg_build(scr, "Control tuning", tune_tab, 1); }

const ui_screen_t ui_scr_valve_set = { "Valve setup", valve_set_build, pg_refresh, NULL };
const ui_screen_t ui_scr_tune = { "Control tuning", tune_build, pg_refresh, NULL };

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
    // Each setting with a field comes in as a draft; the sensors and their
    // cals of a save come in use straight away (they have no draft).
    if (from != CFG_FROM_DEFAULTS) {
        const char *why;
        // What the load replaces: settings in use but unsaved, and drafts.
        before = cfg_unsaved();
        for (int k = 0; k < CFG_N; k++)
            before += draft_set[k];
        drafts_clear();
        if (!cfg_load_live(from, &why)) {
            char t[64];
            lv_snprintf(t, sizeof t, "Not loaded: %s", why);
            ui_msg(t);
            return;
        }
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
    lv_snprintf(t, sizeof t, "Put the %s save%s%s in use now, sensors and calibrations included? Drafts are "
                "dropped. Save settings keeps it; Discard changes puts the last save back.",
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
        ui_msg("Switch to MANUAL first");
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
    return "Last saved and Previous save: the two saves the board keeps, by date and time.\n"
           "Unsaved changes: tap for the list, drafts and changes in use alike.\n"
           "Save settings puts the drafts in use and saves; the last save becomes Previous save.\n"
           "Load settings: a save comes back in use at once, sensors and calibrations included; "
           "factory defaults come in as drafts and keep the sensors. Discard changes puts the last save back.";
}

static void rv_build(lv_obj_t *scr)
{
    // Save settings and Discard changes in the same places as on the other pages.
    rv.saved = ui_info(scr, C2X[0], RY[0], C2W[0], RH, "Last saved");
    rv.prev = ui_info(scr, C2X[1], RY[0], C2W[1], RH, "Previous save");
    ecard_make(&rv.changes, scr, 10, RY[1], 343, "Unsaved changes", changes_cb, NULL);
    rv.load = ui_btn(scr, 10, RY[2], 343, RH, UI_SECONDARY, "Load settings", load_cb, NULL);
    ui_btn_text(rv.load, "Load settings", "last save, previous save or factory defaults");
    side_make(scr, "Review and save", rv_help);
}

static const char *rev_text(char *buf, size_t n, cfg_from_t from)
{
    uint32_t rev;
    const char *at;
    if (!cfg_rev(from, &rev, &at))
        return "None";
    // Saves from 0.8.x have no date; the older one still reads apart.
    return at[0] ? save_when(buf, n, at) : from == CFG_FROM_PREVIOUS ? "Earlier, no date" : "Saved, no date";
}

static void rv_refresh(void)
{
    char t[48], c[24];
    int n = unsaved(), d = drafts(), u = n - d;
    // The state in words, not only the violet / grey edge (review of 0.10.11).
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
 * Calibrate sensor (Menu > Calibrate > Sensor): CAL VAC / CAL AIR on the
 * screen. The new cal is in use once both points are held, unsaved until
 * Save settings (as on COM3).
 * ------------------------------------------------------------------------ */
static struct {
    lv_obj_t *sig, *now, *nw, *vac, *air, *bv, *ba;
} cs;

// The header names the sensor being calibrated: "Calibrate 5880156" (with
// "s/n" it does not fit beside NOT HOMED).
static char cs_title[32] = "Calibrate sensor";

static void cs_title_set(void)
{
    cfg_sensor_t q;
    if (cfg_sensor(cfg_sensor_active(), &q))
        lv_snprintf(cs_title, sizeof cs_title, "Calibrate %lu", (unsigned long)q.serial);
}

static const char *cs_help(void)
{
    return "A two-point calibration of the sensor in use, read live through the board, so the board input "
           "is inside it.\n"
           "Capture vacuum: pump the sensor down, then Capture. Nothing to type: the point is 0 mB (Not 0 mB "
           "if a gauge reads more).\n"
           "Capture air: vent the sensor to the room, then type the lab barometer reading in mB, for example "
           "1013.2.\n"
           "Each capture averages 10 readings. Either point can be redone on its own; the other is kept.\n"
           "Signal: the Druck output over its excitation, in mV/V.\n"
           "Saved cal: the pressure from the saved calibration. Captured: the pressure from the points "
           "captured here, once both are held (None yet until then).\n"
           "Once both are held the new calibration is in use and unsaved: check Saved cal against Captured, "
           "then Save settings. Discard changes puts the last save back.";
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
    if (!cal_capture(what, p_mmbar, 0, &why)) {
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
        ui_msg(cal_capturing() ? "A capture is running" : "No Druck signal");
        return;
    }
    ui_ask("Pump the sensor down to vacuum first. Capture takes 10 readings and calls them 0 mB.",
           "Not 0 mB", cs_vac_typed, "Capture", cs_vac0);
}

static bool cs_air_set(int32_t v10, const char **why)
{
    (void)why;
    cs_start(2, v10 * 100);
    return true;
}

static void air_cb(lv_event_t *e)
{
    if (ui_btn_disabled(e)) {
        ui_msg(cal_capturing() ? "A capture is running" : "No Druck signal");
        return;
    }
    ui_kp_t k = { .title = "Barometer reading, mB", .info = "Vent sensor to room air first", .dec = 1,
                  .min = 5000, .max = 11000, .live_label = "Now, mB", .live_milli = now_milli, .live_dec = 1,
                  .on_set = cs_air_set };
    ui_keypad_ex(&k);
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
    cs_title_set();
}

static void cs_refresh(void)
{
    char t[40];
    const clicks_state_t *s = clicks_state();
    bool ok = reading_ok();
    ui_set_text(cs.sig, ok ? fixed_fmt(t, sizeof t, fixed_round(s->r_ppb, 2), 4) : "----");   // 9.3580
    cfg_sensor_t q;
    const cal_rec_t *saved = NULL;
    bool in_save = cfg_sensor(cfg_sensor_active(), &q) && cfg_saved_cal(q.serial, &saved);
    int64_t p_saved = saved ? cal_pressure_with(saved, s->r_ppb)
                    : (int64_t)s->r_ppb * q.fs_mmbar / 10000000;   // nominal: 0 to 10 mV/V
    cal_point_t v, a;
    cal_points(&v, &a);
    // Saved cal: none for a sensor not in the last save. Captured: only once
    // both points are held and the cal in use is not the saved one.
    ui_set_text(cs.now, !ok ? "----" : !in_save ? "None" : ui_fmt1(t, sizeof t, q.saved ? s->p_mmbar : p_saved));
    ui_set_text(cs.nw, !ok ? "----" : v.have && a.have && !q.saved ? ui_fmt1(t, sizeof t, s->p_mmbar) : "None yet");
    int cap = cal_capturing(), left = cal_capture_left();
    if (cap == 1)
        lv_snprintf(t, sizeof t, "Capturing %d of 10", 10 - left);
    ui_set_text(cs.vac, cap == 1 ? t : pt_text(t, sizeof t, v.have, v.p_mmbar, v.r_ppb));
    char u[40];
    if (cap == 2)
        lv_snprintf(u, sizeof u, "Capturing %d of 10", 10 - left);
    ui_set_text(cs.air, cap == 2 ? u : pt_text(u, sizeof u, a.have, a.p_mmbar, a.r_ppb));
    ui_btn_enable(cs.bv, ok && !cap);
    ui_btn_enable(cs.ba, ok && !cap);
    cs_title_set();
    side_refresh();
}

const ui_screen_t ui_scr_sensor_cal = { cs_title, cs_build, cs_refresh, NULL };
