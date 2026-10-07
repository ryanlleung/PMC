#include <string.h>

#include "main_screen.h"
#include "usb_serial.h"
#include "clicks.h"

lvgl_main_screen_ui_t lvgl_main_screen_ui;

static lv_obj_t *switch_label;
static lv_obj_t *usb_label;
static lv_obj_t *click_label[4];
static lv_obj_t *pressure_label;
static lv_obj_t *pressure_sub;
static lv_obj_t *exc_label;

/**
 * @brief Every second: re-check the Click boards, show their status and
 * the USB link state on screen, and print any board line that changed.
 */
static void status_timer_cb(lv_timer_t *t)
{
    (void)t;
    const char *status[4];

    clicks_poll();
    status[0] = clicks_stepper3_status();
    status[1] = clicks_boost10_status();
    status[2] = clicks_powermonitor_status();
    status[3] = clicks_druck_status();

    for (int i = 0; i < 4; i++) {
        if (strcmp(lv_label_get_text(click_label[i]), status[i]) != 0) {
            lv_label_set_text(click_label[i], status[i]);
            usb_serial_printf("%s\r\n", status[i]);
        }
    }

    lv_label_set_text(pressure_label, clicks_druck_value());
    lv_label_set_text(usb_label, usb_serial_status());
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
        lv_label_set_text(exc_label, "Exc: tripped");
        return;
    }
    lv_label_set_text_fmt(exc_label, "Exc set %ld.%02ld V", (long)(mv / 1000), (long)((mv % 1000 + 5) / 10));
    if (lv_event_get_code(e) == LV_EVENT_RELEASED)
        usb_serial_printf("Excitation set to %ld mV nominal\r\n", (long)mv);
}

/**
 * @brief Switch toggle handler: prints the new state to the USB COM port
 * and mirrors it on screen.
 */
static void switch_0_event_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target_obj(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);

    usb_serial_printf("Switch toggled: %s\r\n", on ? "ON" : "OFF");
    lv_label_set_text(switch_label, on ? "Switch: ON" : "Switch: OFF");
}

void init_main_screen()
{
    init_main_screen_ui(&lvgl_main_screen_ui);

    // Test switch moved to the bottom left to free the middle for pressure.
    lv_obj_set_pos(lvgl_main_screen_ui.switch_0, 10, 238);
    lv_obj_set_size(lvgl_main_screen_ui.switch_0, 50, 26);

    switch_label = lv_label_create(lvgl_main_screen_ui.main_screen);
    lv_label_set_text(switch_label, "Switch: OFF");
    lv_obj_align_to(switch_label, lvgl_main_screen_ui.switch_0, LV_ALIGN_OUT_RIGHT_MID, 10, 0);

    usb_label = lv_label_create(lvgl_main_screen_ui.main_screen);
    lv_label_set_text(usb_label, "USB: starting");
    lv_obj_align(usb_label, LV_ALIGN_BOTTOM_RIGHT, -10, -10);

    // Druck pressure, large, in the middle.
    pressure_label = lv_label_create(lvgl_main_screen_ui.main_screen);
    lv_obj_set_style_text_font(pressure_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_align(pressure_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(pressure_label, 280);
    lv_label_set_text(pressure_label, "----");
    lv_obj_set_pos(pressure_label, 40, 122);

    lv_obj_t *unit = lv_label_create(lvgl_main_screen_ui.main_screen);
    lv_label_set_text(unit, "mbar abs");
    lv_obj_align_to(unit, pressure_label, LV_ALIGN_OUT_RIGHT_BOTTOM, 10, -8);

    pressure_sub = lv_label_create(lvgl_main_screen_ui.main_screen);
    lv_label_set_text(pressure_sub, clicks_druck_cal_nominal()
                      ? "Druck 15 psia, nominal cal (enter cert values)"
                      : "Druck 15 psia, cert cal");
    lv_obj_align_to(pressure_sub, pressure_label, LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 0);

    // Excitation slider, 9.0-11.0 V in 0.1 V steps, starts at 10.0 V to
    // match the start-up wiper.
    lv_obj_t *exc_slider = lv_slider_create(lvgl_main_screen_ui.main_screen);
    lv_slider_set_range(exc_slider, 90, 110);
    lv_slider_set_value(exc_slider, 100, LV_ANIM_OFF);
    lv_obj_set_size(exc_slider, 290, 10);
    lv_obj_set_pos(exc_slider, 20, 214);
    lv_obj_add_event_cb(exc_slider, exc_slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(exc_slider, exc_slider_event_cb, LV_EVENT_RELEASED, NULL);

    exc_label = lv_label_create(lvgl_main_screen_ui.main_screen);
    lv_label_set_text(exc_label, "Exc set 10.00 V");
    lv_obj_align_to(exc_label, exc_slider, LV_ALIGN_OUT_RIGHT_MID, 18, 0);

    // Click board status, top left, one block per socket.
    for (int i = 0; i < 4; i++) {
        click_label[i] = lv_label_create(lvgl_main_screen_ui.main_screen);
        lv_label_set_text(click_label[i], "");
        lv_obj_set_width(click_label[i], 470);
        lv_label_set_long_mode(click_label[i], LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_pos(click_label[i], 5, 5 + 20 * i + (i == 3 ? 20 : 0));  // S4 is two lines
    }

    lv_timer_create(status_timer_cb, 1000, NULL);

    lv_obj_add_event_cb(lvgl_main_screen_ui.switch_0, switch_0_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

void show_main_screen()
{
    lv_screen_load(lvgl_main_screen_ui.main_screen);
}
