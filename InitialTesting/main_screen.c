#include "main_screen.h"
#include "usb_serial.h"

lvgl_main_screen_ui_t lvgl_main_screen_ui;

static lv_obj_t *switch_label;

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

    lv_obj_add_event_cb(lvgl_main_screen_ui.switch_0, switch_0_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

void show_main_screen()
{
    lv_screen_load(lvgl_main_screen_ui.main_screen);
}
