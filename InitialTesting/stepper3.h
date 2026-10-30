#ifndef _STEPPER3_H_
#define _STEPPER3_H_

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Needle-valve stepper on the Stepper 3 Click (S2, ULN2003, unipolar).
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
 * closing step the limit input is read (NC contact on PD5, open = stop):
 * it is the closing stop (limit.c, PMC_LIMIT_STOP).
 *
 * Commands over the link, one per line, replies end with OK or ERR:
 *   MOT?                  state, position and settings
 *   MOT MOVE <steps>      relative move, + open, - close (1 to 400000)
 *   MOT GOTO <pos> [sps]  absolute move, optional rate; retargets a running move
 *   MOT STOP              stop now (coils as per MOT HOLD)
 *   MOT STOP RAMP         ramp down, then stop
 *   MOT OFF               stop now and switch all coils off
 *   MOT ZERO              set the position count to 0
 *   MOT OVERRIDE          let the next move close past an open limit
 *                         (one move only, RAM only); MOT OVERRIDE OFF cancels
 *   MOT RATE <sps>        run rate, steps/s
 *   MOT START <sps>       start/stop rate of the ramp, steps/s
 *   MOT ACCEL <sps/s>     ramp acceleration
 *   MOT MODE WAVE|FULL|HALF
 *   MOT HOLD 0|1          keep the coils on when idle
 *   MOT DIR 0|1           1 reverses the direction
 *   MOT ORDER <abcd>      coil order on the outputs, e.g. 0123 or 0213
 * Settings are kept by CFG SAVE (cfg.c) and loaded at start-up.
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
    STEPPER3_STOP_LIMIT,    // limit input open during a closing move
    STEPPER3_STOP_TIMEOUT   // F-5: still moving after |distance| / rate + 2 s
} stepper3_stop_t;

// Pins low (coils off). Does not move the motor.
void stepper3_init(void);

// Moves to an absolute position (steps of the current mode) at sps steps/s
// (0 = the MOT RATE setting). During a move this retargets it: a target
// further on extends it; a target behind ramps down to the start rate, then
// turns. Returns false for a rate out of range, a target more than
// STEPPER3_MAX_MOVE from here, or (from rest) a closing move with the limit
// open. A target equal to the position returns true without moving.
#define STEPPER3_MAX_MOVE 700000   // > 1.1 x the largest POS_MAX (300000 full steps) in half steps
#define STEPPER3_TIMEOUT_MARGIN_MS 2000
bool stepper3_goto_rate(int32_t target, uint32_t sps);
bool stepper3_goto(int32_t target);
// Relative move; during a move, relative to where that move ends.
bool stepper3_move(int32_t steps);
// Where the running move ends, or the position when idle.
int32_t stepper3_target(void);

// Stops at once. Coils stay on only if hold is set.
void stepper3_stop(void);
// Ramps down at the normal deceleration, then stops (last stop "stopped").
void stepper3_stop_ramped(void);
// From the main loop: F-5 motion timeout, stops at once.
void stepper3_poll(void);
// Goes up by one each time a move ends (done, stopped, limit, timeout);
// stepper3_last_stop() then says why.
uint32_t stepper3_move_seq(void);
// Sets the position count (refused while moving; fine while settling).
bool stepper3_set_position(int32_t p);
// While set (non-NULL), MOT MOVE/GOTO/OVERRIDE/ZERO and the motor screen
// moves are refused with this text. MOT STOP and MOT OFF always work. The
// valve layer uses it while calibrating or with a fault latched.
void stepper3_lock(const char *why);
const char *stepper3_locked(void);
// Disarms the override; a closing move using it is stopped at once (true).
bool stepper3_override_cancel(void);
// Stops at once and switches every coil off.
void stepper3_off(void);

bool stepper3_busy(void);
int32_t stepper3_position(void);
void stepper3_zero(void);
// Motor counts per full step: 2 in HALF, else 1.
int32_t stepper3_scale(void);
#define STEPPER3_STEPS_PER_TURN 16512   // full steps per output turn: 48 x 344 (bench, 22 Oct)
// Called when the step count stops matching the valve's homing: MOT ZERO,
// a change of direction or coil order (from the link, the motor
// screen or a loaded record). The valve layer clears its homing.
void stepper3_on_reference_change(void (*cb)(const char *why));
// Called just before a move starts from rest, before the coils are
// energised (main loop only). The saved position is withdrawn there; if
// the callback returns false with *why, the move is refused.
void stepper3_on_move_start(bool (*cb)(const char **why));
// Why the last refused goto or move was refused.
const char *stepper3_refusal(void);
stepper3_stop_t stepper3_last_stop(void);
// The last move started: where from and how long it took (so far, while
// it runs). False if none since power-up.
bool stepper3_last_move(int32_t *from, uint32_t *ms);

// Limit input state: true if fitted and the NC contact is open.
bool stepper3_limit_open(void);
// Closing override armed or in use (MOT OVERRIDE).
bool stepper3_override(void);
void stepper3_override_arm(void);
bool stepper3_limit_fitted(void);

// Settings (saved by cfg.c). Setters return false while a move is running or
// for a value out of range.
#define STEPPER3_RATE_MIN  16      // steps/s, 1 MHz / 65536, the 16-bit ARR limit
#define STEPPER3_RATE_MAX  1000
#define STEPPER3_ACCEL_MIN 10      // steps/s^2
#define STEPPER3_ACCEL_MAX 20000
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
