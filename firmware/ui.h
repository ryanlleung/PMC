#ifndef _UI_H_
#define _UI_H_

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

/**
 * @brief Screen framework. 480 x 272.
 *
 * Built once on lv_layer_top(), over every screen:
 *   header  0,0 480x40    Back (one screen back; not on Home), title, mode panel
 *   strip   10,216 343x48 status: fault, message (with OK), prompt or info
 *   STOP    361,216 109x48 VALVE STOP (in AUTO: MANUAL), on every screen
 * Home is kept; other screens are created on open and deleted on leave.
 * A keypad or confirm dialog covers 0,40 480x232 except the STOP slot.
 *
 * Motor > Jog (motor_screen.c makes its row) is a framework screen too.
 */

LV_FONT_DECLARE(ui_font_12)
LV_FONT_DECLARE(ui_font_16)
LV_FONT_DECLARE(ui_font_26)
LV_FONT_DECLARE(ui_font_40)

// Fonts with every digit as wide as the widest, for right-aligned numbers.
extern lv_font_t ui_num_12, ui_num_16, ui_num_26, ui_num_40;

// Colour roles. Grey is normal; colour marks only what is abnormal or needs
// a look (red a fault or STOP, yellow a caution), and the one primary
// action on a screen is dark blue with white text. Green is not used.
#define UI_BG      lv_color_hex(0xDCE3EA)   // the screen background
#define UI_CARD    lv_color_hex(0xFFFFFF)   // cards, popups, the info line
#define UI_TEXT    lv_color_hex(0x1F2933)   // text and readings
#define UI_MUTED   lv_color_hex(0x546374)   // captions and units
#define UI_IDLE    lv_color_hex(0x969EA6)   // greyed text: a value not in use, a greyed button
#define UI_FAULT   lv_color_hex(0xD63031)   // fault edges and words, STOP
#define UI_BLUE    lv_color_hex(0x2D5877)   // the primary action, an editable field's underline, the pressure pen
#define UI_WARN    lv_color_hex(0x3F3F3F)   // the warning line's fill, the prompt line's edge
#define UI_YELLOW  lv_color_hex(0xF2C230)   // caution: the caution line's edge, the unsaved icon, the Disable range
#define UI_MODE_PANEL lv_color_hex(0x384652)   // the header's mode panel outside AUTO
#define UI_MODE_AUTO  lv_color_hex(0x2D5877)   // the mode panel in AUTO, white lettering
#define UI_MODE_AUTO_TEXT lv_color_white()
#define UI_TRACK   lv_color_hex(0xE8EEF4)   // the empty part of a bar; a pressed list row
#define UI_FILL    lv_color_hex(0x7B8794)   // the filled part of a bar, the valve pen
#define UI_TICK    lv_color_hex(0xA3AFBB)   // bar ticks, the edge of a card that takes a press
#define UI_PRESS   lv_color_hex(0xC9D3DC)   // the pressed fill of every tappable
#define UI_CHEV    lv_color_hex(0x7B8794)   // a chevron on a live button; UI_IDLE on a greyed one

typedef enum { UI_SECONDARY, UI_PRIMARY, UI_NAV, UI_RED, UI_DARK } ui_btn_kind_t;

// Button with a 16 px label (and an optional 12 px state line). Acts on
// release. Disabled buttons are greyed and still report a press through
// the same callback (so it can say why), with lv_obj_has_state(DISABLED).
lv_obj_t *ui_btn(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h,
                 ui_btn_kind_t kind, const char *text, lv_event_cb_t cb, void *user);
void ui_btn_text(lv_obj_t *btn, const char *text, const char *small);
void ui_btn_enable(lv_obj_t *btn, bool on);
// Filled (primary) or plain, for a button whose role changes with the state.
void ui_btn_primary(lv_obj_t *btn, bool on);
void ui_btn_outline(lv_obj_t *btn, bool on);   // white, blue edge and word
bool ui_btn_disabled(lv_event_t *e);
void ui_btn_arrow(lv_obj_t *btn, bool up);       // a drawn 21 x 12 Up or Down chevron in place of the label
// An icon (a font symbol or a UI_ICON_ below) centred over the label, which
// moves to the bottom: the Menu's tiles. After ui_btn.
void ui_btn_icon(lv_obj_t *btn, const char *icon);

lv_obj_t *ui_card(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h);
lv_obj_t *ui_label(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, const lv_font_t *font,
                   lv_color_t color, lv_text_align_t align, const char *text);
// Changes the text or colour only if different (no redraw otherwise).
void ui_set_text(lv_obj_t *label, const char *text);
void ui_set_color(lv_obj_t *obj, lv_color_t c);

// Card with a 12 px caption; returns the 16 px value label under it.
lv_obj_t *ui_info(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, const char *caption);
// Every digit as wide as the widest, for a value that ticks (a clock, a
// live reading): a 12, 16, 26 or 40 px label.
void ui_tabular(lv_obj_t *label);
// The chevron of a button that opens something: 12 px from the right edge.
lv_obj_t *ui_chev(lv_obj_t *parent);
// "15/10/26 14:28" (DD/MM/YY) from "YYYY-MM-DD HH:MM:SS".
const char *ui_date(char *buf, size_t n, const char *iso);
// When something happened age_ms ago: "14:03:22" once the clock is set, else "up 0:12:05".
const char *ui_when(char *buf, size_t n, uint64_t age_ms);

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

// The OFF screen from the first frame (every start is OFF), right
// after the display driver, until ui_init.
void ui_splash(void);
// "0.18.0, built 18/11/26": the version and the build date (DD/MM/YY).
const char *ui_fw_text(void);
// Seconds since power-up, past the 49.7 day wrap of lv_tick_get.
uint32_t ui_uptime_s(void);
void ui_init(void);            // after the drivers and valve/cfg init
void ui_open(const ui_screen_t *s);
bool ui_showing(const ui_screen_t *s);   // that screen is up
const char *ui_screen_title(void);       // the screen up (the simulator's tests)
void ui_home(void);            // back to Home (deletes the screen left)
void ui_back(void);            // as the header Back
void ui_service(void);         // Motor (its Jog tab)

// Status strip. A message shows for 5 s with an OK, over the screen's own
// line; refusals use it. The strip's action button is Home's.
void ui_home_valve(void);   // homes at once (ui_home.c)
void ui_fault_detail(void);   // the first latched fault's detail (ui_valve.c)
void ui_note(const char *text);   // a confirmation: the plain line, 5 s, with a tick
void ui_note_icon(const char *icon, const char *text);   // the same with another icon (STOP's square)
void ui_mb(char *t);   // "mbar" -> "mB" in place, for shared texts
// Drops what only the text port needs from an event, in place: " (count
// 1350)", " (slot A)", " (switch at count ..)".
void ui_plain(char *t);
// Breaks t for a label w wide, in place (spaces become new lines), so a
// number stays whole and with its unit (never "at -" / "1.51 %").
void ui_wrap(char *t, const lv_font_t *font, int32_t w);
// A network note still showing goes: the operator set the same thing.
void ui_net_note_drop(void);
// The longest homing in whole minutes, rounded up ("up to 4 min").
int ui_home_minutes(void);
void ui_dates(char *t);   // "2026-10-24 14:02:05" -> "24/10/26 14:02" in place, for shared texts
const char *ui_steps(char *buf, size_t n, int32_t steps);   // "1 step", "-120 steps"
void ui_msg(const char *text);
void ui_wake(void);   // the backlight to full, as a touch does
void ui_msg_no_ok(const char *text);   // the same without the OK (Restarting)
// CAUTION: a white line with a yellow edge
// and the triangle, for a state to look at that stops nothing.
typedef enum { UI_STRIP_INFO, UI_STRIP_PROMPT, UI_STRIP_CAUTION, UI_STRIP_WARN, UI_STRIP_FAULT } ui_strip_kind_t;
// icon: a font symbol (LV_SYMBOL_...), one of the drawn icons below, or
// NULL; drawn grey on an info line. Warning
// and fault lines take the triangle. UI_ICON_MODE: the status line's icon,
// picked from the valve state, with the pressure sparkline.
#define UI_ICON_MODE   "\x01m"
#define UI_ICON_VALVE  "\x01v"
#define UI_ICON_TARGET "\x01t"
#define UI_ICON_TREND  "\x01p"   // a chart with a pressure line (the Menu's Trend tile)
#define UI_ICON_MOTOR  "\x01o"   // body, end plate and shaft (the Menu's Motor tile)
#define UI_ICON_PID    "\x01k"   // two sliders (the Menu's PID tile)
#define UI_ICON_GAUGE  "\x01g"   // a dial and needle (the Menu's Pressure sensor tile)
void ui_strip_set(ui_strip_kind_t kind, const char *icon, const char *text);
// Redraws the frame (header, strip, the screen's refresh) within 0.1 s
// rather than at the next 0.5 s refresh; for a state the strip must show
// at once.
void ui_refresh_soon(void);
void ui_strip_action(const char *text, const char *small, lv_event_cb_t cb);   // NULL text: none

// Keypad over the screen. on_set gets the value in 0.01 units (two
// decimals) and returns false with *why to refuse. side_milli gives the
// present value (0.001 units, shown to 2 dp) at the side, updated at 10 Hz.
// Header Back cancels.
typedef bool (*ui_keypad_set_t)(int32_t v100, const char **why);
void ui_keypad(const char *title, const char *info, int32_t min100, int32_t max100,
               const char *side_label, int32_t (*side_milli)(void), ui_keypad_set_t on_set);
// The same with dec decimals (0-2); min, max and the value are in 10^-dec
// units. The popup shows a live value (live_label, 0.001 units shown to
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

// Confirm dialog: Cancel and OK (OK primary). Buttons sit inside x70-410.
void ui_confirm(const char *text, const char *ok, void (*on_ok)(void));
// Restart at once (the Power options question's Restart).
void ui_restart_now(void);
// The Power options question (the power icon, and System's Power button).
void ui_shutdown_ask(void);
// Same with a named cancel button and its own action.
void ui_ask(const char *text, const char *cancel, void (*on_cancel)(void), const char *ok, void (*on_ok)(void));
// With a third button between them (NULL mid: as ui_ask).
// Four buttons: Cancel, two middle ones, OK (Home valve).
void ui_ask4(const char *text, const char *cancel, void (*on_cancel)(void), const char *mid,
             void (*on_mid)(void), const char *mid2, void (*on_mid2)(void), const char *ok, void (*on_ok)(void));
void ui_ask3(const char *text, const char *cancel, void (*on_cancel)(void), const char *mid,
             void (*on_mid)(void), const char *ok, void (*on_ok)(void));
// A list to pick one of; sel -1 for none.
// Tapping outside closes it.
void ui_pick(const char *title, const char *const *items, int n, int sel, void (*on_pick)(int i));
// Scrolling text over the screen (Help); header Back closes it.
void ui_text_view(const char *title, const char *text);
// The same, for a dialog with buttons on the bottom row (left of STOP): the
// status strip is hidden while it is open, as for the keypad.
lv_obj_t *ui_overlay_full(void);
// The closed-switch question opens itself on every screen (ui.c).

bool ui_overlay_open(void);
void ui_overlay_close(void);

// Home and Menu (ui_home.c).
extern const ui_screen_t ui_scr_home, ui_scr_menu;
void ui_status_line(void);     // mode/move line, in a screen's refresh()
void ui_reset_fault_event(lv_event_t *e);   // the strip's Reset fault (ui_home.c)
// Resets what can be reset; true with the text when a fault stays (ui_home.c).
bool ui_reset_faults(char *t, size_t n);
// Home and Set position (ui_home.c): ui_home_ask the Home confirm,
// ui_set_position_ask the Set position as keypad, ui_position_ask both
// (Home valve on Home's Valve tile while not homed).
void ui_home_ask(void);
void ui_set_position_ask(void);
void ui_position_ask(void);
int32_t ui_pct_shown(int32_t milli);   // valve % as shown (2 dp), in 0.01 %, 0 to 10000
// Trend and the Home sparklines (ui_trend.c). A sparkline redraws when
// invalidated (Home does it once a second); a tap opens Trend.
extern const ui_screen_t ui_scr_trend;
lv_obj_t *ui_spark(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, bool valve);
// Events, Diagnostics (ui_valve.c).
extern const ui_screen_t ui_scr_faults, ui_scr_diag;
// The settings pages on the Menu (ui_settings.c): System and its IP page,
// Valve, Motor, PID, Pressure sensor.
extern const ui_screen_t ui_scr_namenet, ui_scr_netaddr, ui_scr_valve_set, ui_scr_motor, ui_scr_tune, ui_scr_sensor_cal;
// The header's unsaved icon: asks to save what is in use and not saved.
void ui_unsaved_ask(void);
// The Menu's Reset to factory tile: asks, then resets (cfg_reset_factory).
void ui_factory_reset_ask(void);

#endif // _UI_H_
