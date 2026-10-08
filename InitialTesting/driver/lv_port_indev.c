#include <string.h>

#include "lv_port_indev.h"
#include "lvgl_common.h"
#include "link.h"

/* LVGL v9: read_cb signature changed, and lv_indev_drv_t is removed */
static void touchpad_init(void);
static void touchpad_read(lv_indev_t * indev, lv_indev_data_t * data);
static bool touchpad_is_pressed(void);
static void touchpad_get_xy(lv_coord_t * x, lv_coord_t * y);

lv_indev_t * indev_touchpad;

// For TOUCH?: how often the controller is read, and presses seen.
static uint32_t touch_reads, touch_presses;
static bool touch_was_pressed;

static tp_err_t touch_last_err;
static uint32_t touch_errors;

void process_tp(void)
{
    touch_last_err = tp_process(&tp);
    if(touch_last_err != TP_OK)
        touch_errors++;
}

void lv_port_indev_init(void)
{
    touchpad_init();

    /* LVGL v9+: create indev and configure it (no lv_indev_drv_t anymore) */
    indev_touchpad = lv_indev_create();
    lv_indev_set_type(indev_touchpad, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev_touchpad, touchpad_read);
    // LVGL reads every 33 ms by default; 10 ms makes presses register sooner.
    lv_timer_set_period(lv_indev_get_read_timer(indev_touchpad), 10);

    /*
     * If you have multiple displays, ensure the correct display is default
     * before creating the indev, or explicitly bind it:
     * lv_indev_set_display(indev_touchpad, your_display);
     */
}

/* Initialize your touchpad. */
static void touchpad_init(void)
{
    touch_controller_tp_init(&tp, &tp_interface);
}

/*
 * Touch is polled from the main loop (touch_poll, every ~5 ms), not from
 * the SysTick ISR: the SDK I2C driver is not safe to share between an
 * interrupt and the main loop, and the ~1 ms transfer belongs outside an
 * ISR. A press seen by any poll is latched until LVGL next reads, so a quick
 * tap that starts and ends between two LVGL reads still counts.
 */
static bool touch_now, touch_latched;
static lv_coord_t touch_x, touch_y;

void touch_poll(void)
{
    static uint32_t last;
    if(lv_tick_elaps(last) < 5)
        return;
    last = lv_tick_get();

    process_tp();
    touch_reads++;
    touch_now = touchpad_is_pressed();
    if(touch_now) {
        touchpad_get_xy(&touch_x, &touch_y);
        if(!touch_was_pressed)
            touch_presses++;
        touch_latched = true;
    }
    touch_was_pressed = touch_now;
}

/* Will be called by LVGL to read the touchpad. */
static void touchpad_read(lv_indev_t * indev, lv_indev_data_t * data)
{
    LV_UNUSED(indev);

    touch_poll();
    data->state = (touch_now || touch_latched) ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    touch_latched = false;
    data->point.x = touch_x;
    data->point.y = touch_y;
}

bool touch_command(const char *line)
{
    if(strcmp(line, "TOUCH?") != 0)
        return false;
    link_printf("TOUCH reads=%lu errors=%lu last_err=%d presses=%lu now=%s last=%d,%d\r\nOK\r\n",
                (unsigned long)touch_reads, (unsigned long)touch_errors, (int)touch_last_err,
                (unsigned long)touch_presses,
                touch_was_pressed ? "pressed" : "released",
                (int)tp.touch.point[0].coord_x, (int)tp.touch.point[0].coord_y);
    return true;
}

/* Return true if the touchpad is pressed. */
static bool touchpad_is_pressed(void)
{
    /* Your original code missed the return statement. */
    check_touchpad();
}

/* Get the x and y coordinates if the touchpad is pressed. */
static void touchpad_get_xy(lv_coord_t * x, lv_coord_t * y)
{
    get_touch_coordinates(x, y);
}

