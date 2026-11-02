
/**
 * @file lv_port_indev_templ.h
 *
 */

#ifndef LV_PORT_INDEV_TEMPL_H
#define LV_PORT_INDEV_TEMPL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

void lv_port_indev_init(void);
void process_tp();

// Reads the touch controller at most every 5 ms; call every main-loop pass.
void touch_poll(void);

// TOUCH?: touch reads, presses seen and the last point, on the link.
// Returns false if the line is not TOUCH?.
bool touch_command(const char *line);

// While a press is down: a second finger on the glass, and where (x, y may
// be NULL). Not given to LVGL; Motor override's Limit override reads it.
bool touch_second(int32_t *x, int32_t *y);
// STOP for any finger: with a thumb or a drop on the glass LVGL follows
// that contact, and a tap elsewhere never reaches a button. touch_watch
// sets a box (screen pixels); a new contact other than the followed one
// landing in it is latched, and touch_watch_take returns and clears it.
void touch_watch(int32_t x1, int32_t y1, int32_t x2, int32_t y2);
bool touch_watch_take(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*LV_PORT_INDEV_TEMPL_H*/