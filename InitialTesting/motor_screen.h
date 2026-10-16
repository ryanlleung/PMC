#ifndef _MOTOR_SCREEN_H_
#define _MOTOR_SCREEN_H_

#include "lvgl.h"

/**
 * @brief Touchscreen motor control: jog by a chosen step size, STOP, and every
 * MOT setting (mode, rates, accel, coil order, direction, hold, zero).
 * Opened from Menu > Motor service (ui_service()); Back calls back(),
 * which returns to Home.
 */
void motor_screen_init(void (*back)(void));   // builds it
void motor_screen_show(void);
// Frees it (after another screen is loaded); init() builds it again.
void motor_screen_close(void);

#endif // _MOTOR_SCREEN_H_
