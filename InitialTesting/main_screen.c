#include <string.h>

#include "main_screen.h"
#include "usb_serial.h"
#include "clicks.h"

lvgl_main_screen_ui_t lvgl_main_screen_ui;

static lv_obj_t *switch_label;
static lv_obj_t *usb_label;
static lv_obj_t *click_label[3];

/**
 * @brief Every second: re-check the Click boards, show their status and
 * the USB link state on screen, and print any board line that changed.
 */
static void status_timer_cb(lv_timer_t *t)
{
    (void)t;
    const char *status[3];

    clicks_poll();
    status[0] = clicks_stepper3_status();
    status[1] = clicks_boost10_status();
    status[2] = clicks_powermonitor_status();

    for (int i = 0; i < 3; i++) {
        if (strcmp(lv_label_get_text(click_label[i]), status[i]) != 0) {
            lv_label_set_text(click_label[i], status[i]);
            usb_serial_printf("%s\r\n", status[i]);
        }
    }

    lv_label_set_text(usb_label, usb_serial_status());
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

    switch_label = lv_label_create(lvgl_main_screen_ui.main_screen);
    lv_label_set_text(switch_label, "Switch: OFF");
    lv_obj_align_to(switch_label, lvgl_main_screen_ui.switch_0, LV_ALIGN_OUT_BOTTOM_MID, 0, 20);

    usb_label = lv_label_create(lvgl_main_screen_ui.main_screen);
    lv_label_set_text(usb_label, "USB: starting");
    lv_obj_align(usb_label, LV_ALIGN_BOTTOM_MID, 0, -10);

    // Click board status, top left, one block per socket.
    for (int i = 0; i < 3; i++) {
        click_label[i] = lv_label_create(lvgl_main_screen_ui.main_screen);
        lv_label_set_text(click_label[i], "");
        lv_obj_set_width(click_label[i], 470);
        lv_label_set_long_mode(click_label[i], LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_pos(click_label[i], 5, 5 + 20 * i);
    }

    lv_timer_create(status_timer_cb, 1000, NULL);

    lv_obj_add_event_cb(lvgl_main_screen_ui.switch_0, switch_0_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

void show_main_screen()
{
    lv_screen_load(lvgl_main_screen_ui.main_screen);
}
