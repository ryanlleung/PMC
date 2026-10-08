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
 *   y   0-30   header: title, Druck ADC (Power Monitor) / excitation (Boost 10) / link
 *   y  38-134  pressure card: calibrated value (48 px), unit, ratio, sensor / fault line
 *   y 140-198  calibration (12 px): Druck s/n, cert zero and span, ATM correction, stored
 *   y 204-262  raw chip readings (12 px): INA228 shunt, bus, die temperature; Boost 10 PG
 * The top two are the useful values; the bottom card is what the chips report.
 * Excitation is fixed at 10.00 V nominal (clicks_init); the screen only reads it.
 * Full per-board status lines go to COM3 only, when they change.
 */
#define COL_HEADER   lv_color_make(31, 41, 51)
#define COL_CARD     lv_color_white()
#define COL_TEXT     lv_color_make(31, 41, 51)
#define COL_MUTED    lv_color_make(110, 120, 130)
#define COL_OK       lv_color_make(46, 160, 67)
#define COL_FAULT    lv_color_make(214, 48, 49)
#define COL_IDLE     lv_color_make(150, 158, 166)
#define COL_WARN     lv_color_make(214, 130, 0)

static lv_obj_t *chip_pm, *chip_boost, *chip_usb;
static lv_obj_t *pressure_label;
static lv_obj_t *pressure_sub;
static lv_obj_t *val_signal, *val_ratio, *val_exc, *val_die, *val_pg;
static lv_obj_t *cal_table, *cal_zero, *cal_span, *cal_atm, *cal_store;

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

// Section card, 12 px text, with a title line; fields go below the title.
static lv_obj_t *make_section(lv_obj_t *parent, int32_t y, const char *title)
{
    lv_obj_t *c = make_card(parent, 10, y, 460, 58);
    lv_obj_set_style_text_font(c, &lv_font_montserrat_12, 0);
    lv_obj_t *t = make_label(c, title, COL_HEADER);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, -2);
    return c;
}

// Caption above, value below, in a section card.
static lv_obj_t *make_field(lv_obj_t *parent, int32_t x, const char *caption)
{
    lv_obj_t *cap = make_label(parent, caption, COL_MUTED);
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, x, 14);
    lv_obj_t *v = make_label(parent, "--", COL_TEXT);
    lv_obj_align(v, LV_ALIGN_TOP_LEFT, x, 30);
    return v;
}

// Fixed point with 2 decimals, rounded: v in units of 10^-dec.
static void set_fixed2(lv_obj_t *l, int64_t v, int dec, const char *unit)
{
    int64_t scale = 1;
    for (int k = 2; k < dec; k++) scale *= 10;
    int64_t a = v < 0 ? -v : v;
    a = (a + scale / 2) / scale;                   // now hundredths
    lv_label_set_text_fmt(l, "%s%ld.%02ld %s", (v < 0 && a) ? "-" : "",
                          (long)(a / 100), (long)(a % 100), unit);
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
    if (s->pm_found && data_on && !s->pm_diag)
        print_data_line(s);

    // Header status.
    set_chip(chip_pm, "Druck ADC", s->pm_found ? COL_OK : COL_FAULT);
    if (s->boost_tripped)
        set_chip(chip_boost, "10 V TRIP", COL_FAULT);
    else
        set_chip(chip_boost, "10 V supply", s->boost_pg ? COL_OK : COL_FAULT);
    set_chip(chip_usb, link_chip_text(), link_chip_ok() ? COL_OK : COL_IDLE);

    // Pressure.
    lv_label_set_text(pressure_label, clicks_druck_value());
    if (!s->pm_found)
        lv_label_set_text(pressure_sub, "Druck ADC (Power Monitor) not responding");
    else if (s->boost_tripped)
        lv_label_set_text_fmt(pressure_sub, "10 V supply tripped at %ld mV, reset to clear", (long)s->boost_trip_mv);
    else if (!s->reading_ok)
        lv_label_set_text(pressure_sub, "Excitation below 7 V, check VBUS wiring");
    else if (s->cal_nominal)
        lv_label_set_text(pressure_sub, "Druck 15 psia, NOT calibrated (nominal)");
    else
        lv_label_set_text(pressure_sub, "Druck 15 psia, calibrated");
    lv_obj_set_style_text_color(pressure_sub,
                                (!s->pm_found || s->boost_tripped || !s->reading_ok) ? COL_FAULT :
                                s->cal_nominal ? COL_WARN : COL_MUTED, 0);

    // Readouts, 2 decimals (the DATA lines keep full resolution).
    if (s->pm_found) {
        set_fixed2(val_signal, s->shunt_nv, 6, "mV");
        set_fixed2(val_exc, s->bus_mv, 3, "V");
        set_fixed2(val_die, s->die_mc, 3, "C");
    } else {
        lv_label_set_text(val_signal, "--");
        lv_label_set_text(val_exc, "--");
        lv_label_set_text(val_die, "--");
    }
    if (s->reading_ok)
        set_fixed2(val_ratio, s->r_ppb, 6, "mV/V");
    else
        lv_label_set_text(val_ratio, "-- mV/V");

    if (s->boost_tripped) {
        lv_label_set_text(val_pg, "tripped");
        lv_obj_set_style_text_color(val_pg, COL_FAULT, 0);
    } else {
        lv_label_set_text(val_pg, s->boost_pg ? "regulating" : "not regulating");
        lv_obj_set_style_text_color(val_pg, s->boost_pg ? COL_OK : COL_FAULT, 0);
    }

    // Calibration. Zero and span are shown as the cert gives them, mV at
    // 10 V excitation, with any ATM correction taken back out.
    bool nominal = cal_is_default();
    if (nominal) {
        lv_label_set_text(cal_table, "none (nominal)");
        lv_obj_set_style_text_color(cal_table, COL_WARN, 0);
    } else {
        // The "+atm" suffix is shown in the ATM column instead.
        char id[CAL_ID_LEN];
        lv_strlcpy(id, cal_id(), sizeof id);
        size_t n = strlen(id);
        if (n >= 4 && strcmp(id + n - 4, "+atm") == 0)
            id[n - 4] = '\0';
        lv_label_set_text(cal_table, id);
        lv_obj_set_style_text_color(cal_table, COL_TEXT, 0);
    }
    set_fixed2(cal_zero, (int64_t)cal_zero_ppb() * 10, 6, "mV");
    set_fixed2(cal_span, (int64_t)cal_span_ppb() * 10, 6, "mV");

    int32_t k = cal_atm_ppm();
    if (!cal_atm_applied()) {
        lv_label_set_text(cal_atm, nominal ? "--" : "not applied");
        lv_obj_set_style_text_color(cal_atm, nominal ? COL_MUTED : COL_WARN, 0);
    } else if (k <= 0) {
        lv_label_set_text(cal_atm, "applied");
        lv_obj_set_style_text_color(cal_atm, COL_OK, 0);
    } else {
        // Gain as a percentage change: 0.957300 shows -4.27 %.
        int32_t c = (LV_ABS(k - 1000000) + 50) / 100;   // 0.01 %
        lv_label_set_text_fmt(cal_atm, "%s%ld.%02ld %%", k < 1000000 ? "-" : "+",
                              (long)(c / 100), (long)(c % 100));
        lv_obj_set_style_text_color(cal_atm, COL_OK, 0);
    }

    if (nominal) {
        lv_label_set_text(cal_store, "--");
        lv_obj_set_style_text_color(cal_store, COL_MUTED, 0);
    } else if (cal_saved()) {
        lv_label_set_text(cal_store, "in flash");
        lv_obj_set_style_text_color(cal_store, COL_OK, 0);
    } else {
        lv_label_set_text(cal_store, "not saved");
        lv_obj_set_style_text_color(cal_store, COL_WARN, 0);
    }
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
    lv_obj_t *pc = make_card(scr, 10, 38, 460, 96);
    pressure_label = make_label(pc, "----", COL_TEXT);
    lv_obj_set_style_text_font(pressure_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_align(pressure_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(pressure_label, 300);
    lv_obj_align(pressure_label, LV_ALIGN_TOP_LEFT, 0, -2);
    lv_obj_t *unit = make_label(pc, "mbar abs", COL_MUTED);
    lv_obj_align_to(unit, pressure_label, LV_ALIGN_OUT_RIGHT_TOP, 12, 10);
    val_ratio = make_label(pc, "-- mV/V", COL_MUTED);
    lv_obj_align_to(val_ratio, unit, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 4);
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

    // Calibration.
    lv_obj_t *cc = make_section(scr, 140, "Calibration");
    cal_table = make_field(cc, 0, "Druck s/n");
    cal_zero = make_field(cc, 110, "Zero (10 V)");
    cal_span = make_field(cc, 195, "Span (10 V)");
    cal_atm = make_field(cc, 280, "ATM correction");
    cal_store = make_field(cc, 380, "Stored");

    // Raw chip readings. Excitation is fixed at 10 V; bus is what it measures.
    lv_obj_t *rc = make_section(scr, 204, "Raw chip readings: INA228 (Power Monitor), Boost 10");
    val_signal = make_field(rc, 0, "Shunt (Druck out)");
    val_exc = make_field(rc, 125, "Bus (excitation)");
    val_die = make_field(rc, 250, "Die temp");
    val_pg = make_field(rc, 350, "Boost PG");

    lv_timer_create(status_timer_cb, 1000, NULL);
}

void show_main_screen()
{
    lv_screen_load(lvgl_main_screen_ui.main_screen);
}
