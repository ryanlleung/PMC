#include <string.h>

#include "main_screen.h"
#include "link.h"
#include "clicks.h"
#include "cal.h"
#include "sysinfo.h"
#include "i2c_sdk_test.h"
#include "pmc_config.h"

lvgl_main_screen_ui_t lvgl_main_screen_ui;

/*
 * Screen layout, 480 x 272:
 *   y   0-30   header: title, Power Monitor / Boost 10 / USB status
 *   y  38-150  pressure card: value (48 px), unit, calibration / fault line
 *   y 158-208  three readouts: signal, excitation, ratio
 *   y 216-262  excitation setpoint slider
 * Full per-board status lines go to COM3 only, when they change.
 */
#define COL_HEADER   lv_color_make(31, 41, 51)
#define COL_CARD     lv_color_white()
#define COL_TEXT     lv_color_make(31, 41, 51)
#define COL_MUTED    lv_color_make(110, 120, 130)
#define COL_OK       lv_color_make(46, 160, 67)
#define COL_FAULT    lv_color_make(214, 48, 49)
#define COL_IDLE     lv_color_make(150, 158, 166)

static lv_obj_t *chip_pm, *chip_boost, *chip_usb;
static lv_obj_t *pressure_label;
static lv_obj_t *pressure_sub;
static lv_obj_t *val_signal, *val_exc, *val_ratio;
static lv_obj_t *exc_slider;
static lv_obj_t *exc_label;

static const char *last_line[6];
static char line_copy[6][192];

static lv_obj_t *make_card(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, COL_CARD, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 6, 0);
    lv_obj_set_style_pad_all(c, 6, 0);
    return c;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, lv_color_t col)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, col, 0);
    return l;
}

// One small readout: caption above, value below.
static lv_obj_t *make_readout(lv_obj_t *parent, int32_t x, const char *caption)
{
    lv_obj_t *c = make_card(parent, x, 158, 148, 50);
    lv_obj_t *cap = make_label(c, caption, COL_MUTED);
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 0, -2);
    lv_obj_t *v = make_label(c, "--", COL_TEXT);
    lv_obj_align(v, LV_ALIGN_BOTTOM_LEFT, 0, 2);
    return v;
}

static void set_chip(lv_obj_t *chip, const char *text, lv_color_t col)
{
    lv_label_set_text(chip, text);
    lv_obj_set_style_text_color(chip, col, 0);
}

/*
 * Once a second, one CSV line on COM3 for logging (tools/logger):
 *   DATA,t_ms,p_mbar,signal_uV,exc_mV,ratio_mV_per_V,die_C,set_mV,ok
 * p_mbar and ratio are empty when there is no valid reading (ok = 0).
 * Lines not starting with DATA are status text.
 */
// DATA lines once a second; DATA OFF pauses them and the live status lines.
static bool data_on = true;

bool main_screen_data_command(const char *line)
{
    if (strcmp(line, "DATA OFF") == 0)
        data_on = false;
    else if (strcmp(line, "DATA ON") == 0)
        data_on = true;
    else
        return false;
    link_printf("OK data %s\r\n", data_on ? "on" : "off");
    return true;
}

static void print_data_line(const clicks_state_t *s)
{
    char p[16] = "", r[16] = "";
    int32_t sig = s->shunt_nv / 100;                 // 0.1 uV

    if (s->reading_ok) {
        int32_t pa = LV_ABS(s->p_mmbar);
        lv_snprintf(p, sizeof p, "%s%ld.%03ld", s->p_mmbar < 0 ? "-" : "",
                    (long)(pa / 1000), (long)(pa % 1000));
        int32_t ra = LV_ABS(s->r_ppb);
        lv_snprintf(r, sizeof r, "%s%ld.%06ld", s->r_ppb < 0 ? "-" : "",
                    (long)(ra / 1000000), (long)(ra % 1000000));
    }
    int32_t da = LV_ABS(s->die_mc);
    link_printf("DATA,%lu,%s,%s%ld.%ld,%ld,%s,%s%ld.%03ld,%ld,%d\r\n",
                      (unsigned long)lv_tick_get(), p,
                      sig < 0 ? "-" : "", (long)(LV_ABS(sig) / 10), (long)(LV_ABS(sig) % 10),
                      (long)s->bus_mv, r,
                      s->die_mc < 0 ? "-" : "", (long)(da / 1000), (long)(da % 1000),
                      (long)s->boost_set_mv, s->reading_ok ? 1 : 0);
}

static void print_changed_lines(void)
{
    const char *line[6] = { sysinfo_line(), clicks_stepper3_status(), clicks_boost10_status(),
                            clicks_powermonitor_status(), clicks_druck_status(),
                            i2c_sdk_test_result() };

    // A PC that connects later still gets every line once.
    bool resend = link_take_new_client();

    // The Power Monitor and Druck lines carry live readings and change every
    // second, so DATA OFF holds them back too.
    const bool live[6] = { false, false, false, true, true, false };

    for (int i = 0; i < 6; i++) {
        if (!line[i][0] || (live[i] && !data_on))
            continue;
        if (resend || last_line[i] == NULL || strcmp(line_copy[i], line[i]) != 0) {
            lv_strlcpy(line_copy[i], line[i], sizeof line_copy[i]);
            last_line[i] = line_copy[i];
            link_printf("%s\r\n", line[i]);
        }
    }
}

/**
 * @brief Every second: re-read the boards, refresh the screen, and print any
 * changed status line to COM3.
 */
static void status_timer_cb(lv_timer_t *t)
{
    (void)t;
    clicks_poll();
    print_changed_lines();

    const clicks_state_t *s = clicks_state();
    if (s->pm_found && data_on)
        print_data_line(s);

    // Header status.
    set_chip(chip_pm, "PM", s->pm_found ? COL_OK : COL_FAULT);
    if (s->boost_tripped)
        set_chip(chip_boost, "BOOST TRIP", COL_FAULT);
    else
        set_chip(chip_boost, "BOOST", s->boost_pg ? COL_OK : COL_FAULT);
    set_chip(chip_usb, link_chip_text(), link_chip_ok() ? COL_OK : COL_IDLE);

    // Pressure.
    lv_label_set_text(pressure_label, clicks_druck_value());
    if (!s->pm_found)
        lv_label_set_text(pressure_sub, "Power Monitor not responding");
    else if (s->boost_tripped)
        lv_label_set_text_fmt(pressure_sub, "Boost tripped at %ld mV, reset to clear", (long)s->boost_trip_mv);
    else if (!s->reading_ok)
        lv_label_set_text(pressure_sub, "Excitation below 7 V, check VBUS wiring");
    else if (s->cal_nominal)
        lv_label_set_text(pressure_sub, "Druck 15 psia, nominal cal");
    else
        lv_label_set_text_fmt(pressure_sub, "Druck 15 psia, cal %s", cal_id());

    // Readouts.
    if (s->pm_found) {
        int32_t sig_uv = s->shunt_nv / 1000;          // whole uV
        int32_t sig_mv = sig_uv / 1000;
        int32_t sig_frac = LV_ABS(sig_uv % 1000);
        lv_label_set_text_fmt(val_signal, "%s%ld.%03ld mV", sig_uv < 0 ? "-" : "",
                              (long)LV_ABS(sig_mv), (long)sig_frac);
        lv_label_set_text_fmt(val_exc, "%ld.%03ld V", (long)(s->bus_mv / 1000), (long)(s->bus_mv % 1000));
    } else {
        lv_label_set_text(val_signal, "--");
        lv_label_set_text(val_exc, "--");
    }
    if (s->reading_ok) {
        int32_t r10 = s->r_ppb / 10;   // mV/V to 5 decimals
        lv_label_set_text_fmt(val_ratio, "%s%ld.%05ld mV/V", r10 < 0 ? "-" : "",
                              (long)(LV_ABS(r10) / 100000), (long)(LV_ABS(r10) % 100000));
    } else {
        lv_label_set_text(val_ratio, "--");
    }

    if (s->boost_tripped)
        lv_obj_add_state(exc_slider, LV_STATE_DISABLED);
}

/**
 * @brief Excitation slider: 90-110 = 9.0-11.0 V. Sets the Boost 10 and shows
 * the nominal value actually set (the digipot steps are ~50 mV).
 */
static void exc_slider_event_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target_obj(e);
    int32_t mv = clicks_boost10_set_mv(lv_slider_get_value(sl) * 100);

    if (mv < 0) {
        lv_label_set_text(exc_label, "tripped");
        return;
    }
    lv_label_set_text_fmt(exc_label, "%ld.%02ld V", (long)(mv / 1000), (long)((mv % 1000 + 5) / 10));
    if (lv_event_get_code(e) == LV_EVENT_RELEASED)
        link_printf("Excitation set to %ld mV nominal\r\n", (long)mv);
}

void init_main_screen()
{
    init_main_screen_ui(&lvgl_main_screen_ui);
    lv_obj_t *scr = lvgl_main_screen_ui.main_screen;

    // The designer's test switch is no longer used.
    lv_obj_add_flag(lvgl_main_screen_ui.switch_0, LV_OBJ_FLAG_HIDDEN);

    // Header.
    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_size(hdr, 480, 30);
    lv_obj_set_style_bg_color(hdr, COL_HEADER, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_hor(hdr, 10, 0);
    lv_obj_set_style_pad_ver(hdr, 0, 0);

    lv_obj_t *title = make_label(hdr, "PMC  Druck pressure", lv_color_white());
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);
    // Status words, right-aligned row; green = ok, red = fault, grey = idle.
    lv_obj_t *chips = lv_obj_create(hdr);
    lv_obj_remove_flag(chips, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(chips, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(chips, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chips, 0, 0);
    lv_obj_set_style_pad_all(chips, 0, 0);
    lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(chips, 14, 0);
    lv_obj_align(chips, LV_ALIGN_RIGHT_MID, 0, 0);
    chip_pm = make_label(chips, "PM", COL_IDLE);
    chip_boost = make_label(chips, "BOOST", COL_IDLE);
    chip_usb = make_label(chips, "USB", COL_IDLE);

    // Pressure card.
    lv_obj_t *pc = make_card(scr, 10, 38, 460, 112);
    pressure_label = make_label(pc, "----", COL_TEXT);
    lv_obj_set_style_text_font(pressure_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_align(pressure_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(pressure_label, 300);
    lv_obj_align(pressure_label, LV_ALIGN_TOP_LEFT, 0, 4);
    lv_obj_t *unit = make_label(pc, "mbar abs", COL_MUTED);
    lv_obj_align_to(unit, pressure_label, LV_ALIGN_OUT_RIGHT_BOTTOM, 12, -10);
    pressure_sub = make_label(pc, "", COL_MUTED);
    lv_obj_align(pressure_sub, LV_ALIGN_BOTTOM_LEFT, 4, 0);

    // Firmware version; after a watchdog or brown-out reset, also the cause
    // in red. Normal reset causes are only on the link (VER?).
    lv_obj_t *ver = make_label(pc, "", sysinfo_reset_abnormal() ? COL_FAULT : COL_MUTED);
    if (sysinfo_reset_abnormal())
        lv_label_set_text_fmt(ver, "v%s, reset: %s", PMC_FW_VERSION, sysinfo_reset_reason());
    else
        lv_label_set_text_fmt(ver, "v%s", PMC_FW_VERSION);
    lv_obj_align(ver, LV_ALIGN_BOTTOM_RIGHT, -4, 0);

    // Readouts.
    val_signal = make_readout(scr, 10, "Signal");
    val_exc = make_readout(scr, 166, "Excitation");
    val_ratio = make_readout(scr, 322, "Ratio");

    // Excitation setpoint, 9.0-11.0 V in 0.1 V steps, starts at 10.0 V to
    // match the start-up wiper.
    lv_obj_t *sc = make_card(scr, 10, 216, 460, 46);
    lv_obj_t *cap = make_label(sc, "Excitation set", COL_MUTED);
    lv_obj_align(cap, LV_ALIGN_LEFT_MID, 0, 0);
    exc_slider = lv_slider_create(sc);
    lv_slider_set_range(exc_slider, 90, 110);
    lv_slider_set_value(exc_slider, 100, LV_ANIM_OFF);
    lv_obj_set_size(exc_slider, 220, 10);
    lv_obj_align(exc_slider, LV_ALIGN_LEFT_MID, 120, 0);
    lv_obj_add_event_cb(exc_slider, exc_slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(exc_slider, exc_slider_event_cb, LV_EVENT_RELEASED, NULL);
    exc_label = make_label(sc, "10.00 V", COL_TEXT);
    lv_obj_align(exc_label, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_timer_create(status_timer_cb, 1000, NULL);
}

void show_main_screen()
{
    lv_screen_load(lvgl_main_screen_ui.main_screen);
}
