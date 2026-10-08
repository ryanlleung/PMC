#ifndef _MOTOR_SCREEN_H_
#define _MOTOR_SCREEN_H_

#include "lvgl.h"

/**
 * @brief Touchscreen motor control: jog by a chosen step size, STOP, and every
 * MOT setting (mode, rates, accel, coil order, direction, hold, zero).
 * Opened from the Motor button on the main screen; Back returns to it.
 */
void motor_screen_init(lv_obj_t *return_screen);
void motor_screen_show(void);

#endif // _MOTOR_SCREEN_H_
