#ifndef _STEPPER3_H_
#define _STEPPER3_H_

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Needle-valve stepper on the Stepper 3 Click (S1, ULN2003, unipolar).
 *
 * The ULN2003 only switches the four coil ends to ground; the step sequence
 * comes from here. Steps are timed by TIM7 (interrupt per step), so the
 * main loop and screen keep running during a move. Linear ramp from the
 * start rate up to the run rate and back down.
 *
 * Nothing moves at start-up: all coils off until a MOT MOVE command.
 * Coils are switched off after each move unless MOT HOLD 1.
 *
 * Direction convention: positive steps open the valve, negative close it.
 * MOT DIR flips the sequence if the wiring turns the other way. Before every
 * closing step the limit input is read (NC contact, open = stop). The input
 * (DIGI IN, IN1) is not fitted yet, so closing moves are not stopped by it.
 *
 * Commands over the link, one per line, replies end with OK or ERR:
 *   MOT?                  state, position and settings
 *   MOT MOVE <steps>      relative move, + open, - close
 *   MOT STOP              stop now (coils as per MOT HOLD)
 *   MOT OFF               stop now and switch all coils off
 *   MOT ZERO              set the position count to 0
 *   MOT RATE <sps>        run rate, steps/s
 *   MOT START <sps>       start/stop rate of the ramp, steps/s
 *   MOT ACCEL <sps/s>     ramp acceleration
 *   MOT MODE WAVE|FULL|HALF
 *   MOT HOLD 0|1          keep the coils on when idle
 *   MOT DIR 0|1           1 reverses the direction
 *   MOT ORDER <abcd>      coil order on the outputs, e.g. 0123 or 0213
 * Settings are RAM only and return to the defaults at reset.
 */

typedef enum {
    STEPPER3_WAVE,   // one coil on at a time
    STEPPER3_FULL,   // two coils on, full torque
    STEPPER3_HALF    // alternates one and two coils, half-size steps
} stepper3_mode_t;

typedef enum {
    STEPPER3_STOP_NONE,     // no move yet
    STEPPER3_STOP_DONE,     // reached the target
    STEPPER3_STOP_CMD,      // MOT STOP / MOT OFF / stepper3_stop()
    STEPPER3_STOP_LIMIT     // limit input open during a closing move
} stepper3_stop_t;

// Pins low (coils off). Does not move the motor.
void stepper3_init(void);

// Starts a relative move in steps of the current mode. Returns false if a
// move is running, steps is 0, or |steps| is over STEPPER3_MAX_MOVE.
#define STEPPER3_MAX_MOVE 100000
bool stepper3_move(int32_t steps);

// Stops at once. Coils stay on only if hold is set.
void stepper3_stop(void);
// Stops at once and switches every coil off.
void stepper3_off(void);

bool stepper3_busy(void);
int32_t stepper3_position(void);
void stepper3_zero(void);
stepper3_stop_t stepper3_last_stop(void);

// Limit input state: true if fitted and the NC contact is open.
bool stepper3_limit_open(void);
bool stepper3_limit_fitted(void);

// Settings (RAM only). Setters return false while a move is running or for
// a value out of range (rates 16-1000 steps/s, accel 10-20000 steps/s^2).
bool stepper3_set_rate(uint32_t sps);
bool stepper3_set_start(uint32_t sps);
bool stepper3_set_accel(uint32_t sps2);
bool stepper3_set_mode(stepper3_mode_t mode);   // converts the position count
bool stepper3_set_hold(bool on);
bool stepper3_set_reverse(bool on);
bool stepper3_set_order(const uint8_t order[4]); // each of 0-3 once

uint32_t stepper3_rate(void);
uint32_t stepper3_start_rate(void);
uint32_t stepper3_accel(void);
stepper3_mode_t stepper3_mode(void);
bool stepper3_hold(void);
bool stepper3_reverse(void);
bool stepper3_energised(void);
int8_t stepper3_direction(void);      // +1 opening, -1 closing, 0 idle
void stepper3_order(uint8_t order[4]);
const char *stepper3_mode_name(stepper3_mode_t mode);
const char *stepper3_stop_name(stepper3_stop_t stop);

// Short status line for the screen and the link.
const char *stepper3_status(void);

// Handles one MOT command line. Returns false if it is not a MOT command.
bool stepper3_command(const char *line);

#endif // _STEPPER3_H_
