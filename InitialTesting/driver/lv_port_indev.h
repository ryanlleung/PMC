
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

// TOUCH?: touch reads, presses seen and the last point, on the link.
// Returns false if the line is not TOUCH?.
bool touch_command(const char *line);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*LV_PORT_INDEV_TEMPL_H*/