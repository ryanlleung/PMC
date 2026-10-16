#ifndef _UI_H_
#define _UI_H_

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

/**
 * @brief Screen framework (stage 4 of the porting plan), after simulator v63
 * and simulator/STYLE-GUIDE.md. 480 x 272.
 *
 * Built once on lv_layer_top(), over every screen:
 *   header  0,0 480x40    Back (one screen back; not on Home), title, mode word and dot
 *   strip   10,216 343x48 status: fault, message (with OK), prompt or info
 *   STOP    361,216 109x48 latches F-12 (CTRL STOP), on every screen
 * Home is kept; other screens are created on open and deleted on leave.
 * A keypad or confirm dialog covers 0,40 480x232 except the STOP slot.
 *
 * The 0.6.0 motor screen opens from Menu > Motor service; the frame is
 * hidden there and its own buttons apply.
 */

LV_FONT_DECLARE(ui_font_12)
LV_FONT_DECLARE(ui_font_16)
LV_FONT_DECLARE(ui_font_26)
LV_FONT_DECLARE(ui_font_40)

// Fonts with every digit as wide as the widest, for right-aligned numbers.
extern lv_font_t ui_num_16, ui_num_26, ui_num_40;

// Colour roles (STYLE-GUIDE.md section 3).
#define UI_BG      lv_color_hex(0xDCE3EA)
#define UI_CARD    lv_color_hex(0xFFFFFF)
#define UI_TEXT    lv_color_hex(0x1F2933)
#define UI_MUTED   lv_color_hex(0x546374)
#define UI_IDLE    lv_color_hex(0x969EA6)
#define UI_FAULT   lv_color_hex(0xD63031)
#define UI_BLUE    lv_color_hex(0x2D5877)
#define UI_WARN    lv_color_hex(0x3F3F3F)
#define UI_TRACK   lv_color_hex(0xE8EEF4)
#define UI_FILL    lv_color_hex(0x7B8794)
#define UI_TICK    lv_color_hex(0xA3AFBB)

typedef enum { UI_SECONDARY, UI_PRIMARY, UI_NAV, UI_RED, UI_DARK } ui_btn_kind_t;

// Button with a 16 px label (and an optional 12 px state line). Acts on
// release. Disabled buttons are greyed and still report a press through
// the same callback (so it can say why), with lv_obj_has_state(DISABLED).
lv_obj_t *ui_btn(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h,
                 ui_btn_kind_t kind, const char *text, lv_event_cb_t cb, void *user);
void ui_btn_text(lv_obj_t *btn, const char *text, const char *small);
void ui_btn_enable(lv_obj_t *btn, bool on);
bool ui_btn_disabled(lv_event_t *e);

lv_obj_t *ui_card(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h);
lv_obj_t *ui_label(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, const lv_font_t *font,
                   lv_color_t color, lv_text_align_t align, const char *text);
// Changes the text or colour only if different (no redraw otherwise).
void ui_set_text(lv_obj_t *label, const char *text);
void ui_set_color(lv_obj_t *obj, lv_color_t c);

// Card with a 12 px caption; returns the 16 px value label under it.
lv_obj_t *ui_info(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, const char *caption);
// "15/10/26 14:28" (DD/MM/YY) from "YYYY-MM-DD HH:MM:SS".
const char *ui_date(char *buf, size_t n, const char *iso);
// When something happened age_ms ago: "14:03:22" once the clock is set, else "up 0:12:05".
const char *ui_when(char *buf, size_t n, uint32_t age_ms);

// "12.3" from 0.001 units, one decimal, rounded half away from zero.
const char *ui_fmt1(char *buf, size_t n, int64_t milli);
const char *ui_fmt2(char *buf, size_t n, int64_t milli);   // two decimals (valve %)

// Screens. A screen's build() makes its objects on scr; refresh() runs at
// 2 Hz while it is shown, fast() (optional) at 10 Hz for moving readings.
typedef struct {
    const char *title;
    void (*build)(lv_obj_t *scr);
    void (*refresh)(void);
    void (*fast)(void);
} ui_screen_t;

void ui_init(void);            // after the drivers and valve/cfg init
void ui_open(const ui_screen_t *s);
bool ui_showing(const ui_screen_t *s);   // that screen is up
void ui_home(void);            // back to Home (deletes the screen left)
void ui_back(void);            // as the header Back
void ui_service(void);         // the 0.6.0 motor screen (Settings > Motor override), frame hidden
bool ui_on_service(void);

// Status strip. A message shows for 5 s with an OK, over the screen's own
// line; refusals use it. The strip's action button is Home's.
void ui_msg(const char *text);
typedef enum { UI_STRIP_INFO, UI_STRIP_PROMPT, UI_STRIP_WARN, UI_STRIP_FAULT } ui_strip_kind_t;
void ui_strip_set(ui_strip_kind_t kind, lv_color_t dot, const char *text);
// Strip width for this screen (343 unless a screen sets it in build()).
void ui_strip_width(int32_t w);
void ui_strip_action(const char *text, const char *small, lv_event_cb_t cb);   // NULL text: none

// Keypad over the screen. on_set gets the value in 0.01 units (two
// decimals) and returns false with *why to refuse. side_milli gives the
// present value (0.001 units, shown to 2 dp) at the side, updated at 10 Hz.
// Header Back cancels.
typedef bool (*ui_keypad_set_t)(int32_t v100, const char **why);
void ui_keypad(const char *title, const char *info, int32_t min100, int32_t max100,
               const char *side_label, int32_t (*side_milli)(void), ui_keypad_set_t on_set);
// The same with dec decimals (0-2); min, max and the value are in 10^-dec
// units. The side card shows a live value (live_label, 0.001 units shown to
// live_dec decimals) or up to two fixed rows (e.g. In use, Saved).
typedef struct {
    const char *title, *info;
    int dec;
    int32_t min, max;
    const char *live_label;
    int32_t (*live_milli)(void);
    int live_dec;
    const char *side_label[2], *side_text[2];
    bool (*on_set)(int32_t v, const char **why);
} ui_kp_t;
void ui_keypad_ex(const ui_kp_t *k);

// Confirm dialog: Cancel and OK (OK primary). Buttons sit inside x70-410,
// clear of the Home mode button.
void ui_confirm(const char *text, const char *ok, void (*on_ok)(void));
// Same with a named cancel button and its own action.
void ui_ask(const char *text, const char *cancel, void (*on_cancel)(void), const char *ok, void (*on_ok)(void));
// With a third button between them (NULL mid: as ui_ask).
void ui_ask3(const char *text, const char *cancel, void (*on_cancel)(void), const char *mid,
             void (*on_mid)(void), const char *ok, void (*on_ok)(void));
// A list to pick one of (Sensor in use, Load settings); sel -1 for none.
// Tapping outside closes it.
void ui_pick(const char *title, const char *const *items, int n, int sel, void (*on_pick)(int i));
// Scrolling text over the screen (Help); header Back closes it.
void ui_text_view(const char *title, const char *text);
// Empty overlay over 0,40 480x232 (STOP stays on top) for a screen's own dialog.
lv_obj_t *ui_overlay(void);
// The closed-switch question opens itself on every screen (ui.c).

bool ui_overlay_open(void);
void ui_overlay_close(void);

// Home and Menu (ui_home.c).
extern const ui_screen_t ui_scr_home, ui_scr_menu;
void ui_status_line(void);     // mode/move line and Reset fault, in a screen's refresh()
void ui_calibrate_ask(void);   // the Calibrate valve confirm
int32_t ui_log_px(int32_t p_mmbar, int32_t w);   // 0.1-1000 mbar on a log scale, 0 to w px
int32_t ui_pct_shown(int32_t milli);   // valve % as shown (2 dp), in 0.01 %, 0 to 10000
// Trend and the Home sparklines (ui_trend.c). A sparkline redraws when
// invalidated (Home does it once a second); a tap opens Trend.
extern const ui_screen_t ui_scr_trend;
lv_obj_t *ui_spark(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, bool valve);
// Valve, Calibrate valve, Faults and log, Diagnostics (ui_valve.c).
extern const ui_screen_t ui_scr_valve, ui_scr_calib, ui_scr_faults, ui_scr_diag;
// Settings, its five pages and Calibrate sensor (ui_settings.c).
extern const ui_screen_t ui_scr_settings, ui_scr_name, ui_scr_sensor_set, ui_scr_valve_set, ui_scr_tune, ui_scr_review,
                         ui_scr_sensor_cal;

#endif // _UI_H_
