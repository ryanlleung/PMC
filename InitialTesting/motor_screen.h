#ifndef _MOTOR_SCREEN_H_
#define _MOTOR_SCREEN_H_

#include "ui.h"

/**
 * @brief Valve position > Motor override: jog by a chosen step size and every MOT
 * setting (mode, rates, accel, coil order, direction, hold, zero). A
 * framework screen (header, strip, STOP) since 0.11.32; ui_service() opens it.
 */
extern const ui_screen_t ui_scr_motor;

#endif // _MOTOR_SCREEN_H_
