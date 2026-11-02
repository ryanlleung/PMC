#include "pmc_config.h"
#include <string.h>

#include "lv_port_indev.h"
#include "lvgl_common.h"
#include "link.h"

/* LVGL v9: read_cb signature changed, and lv_indev_drv_t is removed */
static void touchpad_init(void);
static void touchpad_read(lv_indev_t * indev, lv_indev_data_t * data);

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
 *
 * One finger only (0.12.18). The controller reports up to five touches and
 * LVGL has one pointer. To 0.12.17 the pointer was the controller's first
 * point, which can be either finger and swaps as fingers lift, so a second
 * finger (or a palm) made the press jump between buttons, and the buttons
 * act on press. Now the finger that started the press is followed by
 * position (the nearest point within TOUCH_SAME_PX of where it was); other
 * touches are ignored. When that finger lifts the press ends, and the next
 * press waits until no finger is on the glass.
 *
 * A second finger is kept apart for the Limit override (0.13.7, Ryan 28
 * Oct): while the followed finger is down, touch_second() gives the other
 * touch nearest it. It never reaches LVGL; Motor override reads it itself.
 */
#define TOUCH_SAME_PX 60

static bool touch_now, touch_latched, touch_wait_up;
static lv_coord_t touch_x, touch_y;
static uint32_t touch_multi;    // polls that saw more than one touch, for TOUCH?

static bool touch2_now;
static lv_coord_t touch2_x, touch2_y;

// STOP for any finger (touch_watch): a contact other than the one LVGL
// follows, landing in the box, is latched at once (a quick tap counts).
static int32_t wbox[4];
static bool wbox_set, wbox_in, wbox_hit;
static uint8_t wbox_n;   // contacts at the last read

static bool touch_follow(const tp_touch_item_t *t, uint8_t n, lv_coord_t *x, lv_coord_t *y, int32_t *idx)
{
    int32_t best = -1, best_d = 0;
    for (uint8_t i = 0; i < n; i++) {
        int32_t dx = (int32_t)t->point[i].coord_x - touch_x;
        int32_t dy = (int32_t)t->point[i].coord_y - touch_y;
        int32_t d = dx * dx + dy * dy;
        if (best < 0 || d < best_d) {
            best = i;
            best_d = d;
        }
    }
    if (best < 0 || best_d > TOUCH_SAME_PX * TOUCH_SAME_PX)
        return false;
    *x = (lv_coord_t)t->point[best].coord_x;
    *y = (lv_coord_t)t->point[best].coord_y;
    *idx = best;
    return true;
}

// The touch nearest the followed one, other than it.
static void touch_other(const tp_touch_item_t *t, uint8_t n, int32_t skip)
{
    int32_t best = -1, best_d = 0;
    for (uint8_t i = 0; i < n; i++) {
        if ((int32_t)i == skip)
            continue;
        int32_t dx = (int32_t)t->point[i].coord_x - touch_x;
        int32_t dy = (int32_t)t->point[i].coord_y - touch_y;
        int32_t d = dx * dx + dy * dy;
        if (best < 0 || d < best_d) {
            best = i;
            best_d = d;
        }
    }
    touch2_now = best >= 0;
    if (touch2_now) {
        touch2_x = (lv_coord_t)t->point[best].coord_x;
        touch2_y = (lv_coord_t)t->point[best].coord_y;
    }
}

void touch_watch(int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    wbox[0] = x1; wbox[1] = y1; wbox[2] = x2; wbox[3] = y2;
    wbox_set = true;
}

bool touch_watch_take(void)
{
    bool h = wbox_hit;
    wbox_hit = false;
    return h;
}

// Any contact but the followed one (skip, -1 none) in the watched box.
static bool watch_check(const tp_touch_item_t *t, uint8_t n, int32_t skip)
{
    if (!wbox_set)
        return false;
    for (int32_t i = 0; i < n; i++) {
        int32_t x = t->point[i].coord_x, y = t->point[i].coord_y;
        if (i != skip && x >= wbox[0] && x <= wbox[2] && y >= wbox[1] && y <= wbox[3])
            return true;
    }
    return false;
}

bool touch_second(int32_t *x, int32_t *y)
{
    if (touch2_now && x && y) {
        *x = touch2_x;
        *y = touch2_y;
    }
    return touch2_now;
}

void touch_poll(void)
{
    static uint32_t last;
    if(lv_tick_elaps(last) < 5)
        return;
    last = lv_tick_get();

    process_tp();
    touch_reads++;
    // The points as the SDK gives them to LVGL (get_touch_coordinates).
    tp_touch_item_t t;
    memset(&t, 0, sizeof t);
    uint8_t n = touch_last_err == TP_OK ? tp.touch.n_touches : 0;
    if(n)
        tp_press_coordinates(&tp, &t);
    if(n > TP_N_TOUCHES_MAX)
        n = TP_N_TOUCHES_MAX;
    if(n > 1)
        touch_multi++;

    int32_t idx = 0;
    if(touch_last_err != TP_OK) {
        // A failed read says nothing about the finger: tp.touch may still
        // hold the last good read. End any press, so a hold (Motor
        // override, the Limit override close) cannot outlive the controller,
        // and start the next one only after a good read with no finger down.
        touch_wait_up = touch_wait_up || touch_now;
        touch_now = false;
    } else if(n == 0) {
        touch_now = false;
        touch_wait_up = false;
    } else if(touch_now) {
        // Follow the finger that started the press; if it lifted, end the
        // press even though another finger is still down.
        if(!touch_follow(&t, n, &touch_x, &touch_y, &idx)) {
            touch_now = false;
            touch_wait_up = true;
        }
    } else if(!touch_wait_up) {
        touch_x = (lv_coord_t)t.point[0].coord_x;
        touch_y = (lv_coord_t)t.point[0].coord_y;
        touch_now = true;
        touch_presses++;
        touch_latched = true;
    }
    if(touch_now)
        touch_other(&t, n, idx);
    else
        touch2_now = false;
    // A contact that has just landed there: the count went up this read (a
    // finger sliding in, or the followed one lost by a fast move, is not).
    bool in = watch_check(&t, n, touch_now ? idx : -1);
    if(in && !wbox_in && n > wbox_n)
        wbox_hit = true;   // latched until taken
    wbox_in = in;
    wbox_n = n;
    if(touch_now)
        touch_latched = true;
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

#if PMC_ENG_COMMANDS
bool touch_command(const char *line)
{
    if(strcmp(line, "TOUCH?") != 0)
        return false;
    link_printf("TOUCH reads=%lu errors=%lu last_err=%d presses=%lu multi=%lu now=%s last=%d,%d\r\nOK\r\n",
                (unsigned long)touch_reads, (unsigned long)touch_errors, (int)touch_last_err,
                (unsigned long)touch_presses, (unsigned long)touch_multi,
                touch_was_pressed ? "pressed" : "released",
                (int)tp.touch.point[0].coord_x, (int)tp.touch.point[0].coord_y);
    return true;
}
#endif

