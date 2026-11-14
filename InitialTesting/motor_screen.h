#ifndef _MOTOR_SCREEN_H_
#define _MOTOR_SCREEN_H_

#include <stddef.h>
#include "ui.h"

/**
 * @brief Motor > Jog (0.17.0; Motor override to 0.16.33): Close, Limit
 * override (two-finger deadman) and Open, hold to run. The Motor page
 * (ui_settings.c, ui_scr_motor) makes the row in its Jog tab.
 */
void motor_jog_make(lv_obj_t *parent);   // the row at y 104, x 10 to 353; goes with parent
void motor_jog_fast(void);               // 10 Hz while the row is up
void motor_jog_strip(void);              // the strip while the row is up
// "2 min 45 s": a full open or close at the rates in use.
const char *motor_travel_text(char *buf, size_t n);

#endif // _MOTOR_SCREEN_H_
