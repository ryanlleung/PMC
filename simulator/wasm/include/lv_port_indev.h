/* Simulator stand-in: touch comes from the page's mouse or finger. */
#ifndef _SIM_LV_PORT_INDEV_H_
#define _SIM_LV_PORT_INDEV_H_
#include <stdbool.h>
void lv_port_indev_init(void);
void touch_poll(void);
bool touch_command(const char *line);

// While a press is down: a second finger on the glass, and where (x, y may
// be NULL). Not given to LVGL; Motor override's Limit override reads it.
bool touch_second(int32_t *x, int32_t *y);
// STOP for any finger (the board's driver/lv_port_indev.h); stubs here.
void touch_watch(int32_t x1, int32_t y1, int32_t x2, int32_t y2);
bool touch_watch_take(void);
#endif
