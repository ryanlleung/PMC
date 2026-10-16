#ifndef _VALVE_H_
#define _VALVE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "clicks.h"

/**
 * @brief Valve layer over stepper3.c: calibration to the closed switch,
 * position in % of POS_MAX, fault latches, STOP and the event log.
 * Ported from the simulator (stage 2 of the porting plan). Settings are kept
 * by CFG SAVE (cfg.c, stage 3).
 *
 * Modes: NOT CALIBRATED (boot, watchdog reset, after F-07/F-09 or an
 * aborted calibration), CALIBRATING, MANUAL. Nothing moves at boot.
 *
 * Calibration (Control Logic 4): if the switch is pressed, an N_clear
 * clearance move off it first (F-09 if still pressed); a fast close at
 * f_home for up to 1.1 x POS_MAX (F-07 if the switch is not found); an
 * N_backoff back-off (F-09 if still pressed); a creep close at f_creep
 * (F-07 if not found); the switch point is N_offset (0 %).
 *
 * The switch pressed during any other move, once calibrated: the move stops
 * (stepper3.c), a W-01 event, and a question waits for VALVE SETZERO (this
 * point is 0 %) or VALVE SKIP (count kept).
 *
 * Faults latch, stop the motor and lock out MOT moves until CTRL RESET,
 * which clears only the latches whose cause has gone:
 *   F-01 no Druck signal, F-03 excitation outside 9.5-10.5 V or tripped,
 *   F-07 switch not found while calibrating, F-09 switch still pressed
 *   after moving off it, F-13 motion timeout, F-12 STOP in AUTO (CTRL STOP).
 * Outside AUTO, STOP stops and holds the valve without a latch (STOP event).
 *
 * Commands:
 *   VALVE?                       mode, position, settings, pending question
 *   VALVE CAL                    calibrate (MANUAL or NOT CALIBRATED, no fault)
 *   VALVE GOTO <0-100>           go to % open (one decimal), once calibrated
 *   VALVE SETZERO | VALVE SKIP   answer the switch-pressed question (or the screen)
 *   VALVE CLOSED                 take the valve as closed (0 %) where it stands, homed
 *   VALVE POSMAX|FHOME|FCREEP|BACKOFF|CLEAR|OFFSET <n>   settings (idle)
 *   CTRL STOP                    stop now, cancel calibration (F-12 in AUTO)
 *   CTRL RESET                   clear latches whose cause has gone
 *   CTRL?                        latched faults, highest priority first
 *   EVT? [n]                     the last n events (default 20, 200 kept)
 * Each event is also sent when it happens as "EVT <s> <tag> <text>".
 */

typedef enum { VALVE_NOT_CAL, VALVE_CALIBRATING, VALVE_MANUAL } valve_mode_t;

void valve_init(void);           // after stepper3_init(); logs F-14 power-up
void valve_task(void);           // every main-loop pass: calibration, limit hits
void valve_sense(const clicks_state_t *s);   // after each 1 s pressure read
bool valve_command(const char *line);

valve_mode_t valve_mode(void);
const char *valve_mode_name(void);
bool valve_calibrated(void);
int32_t valve_pos_max(void);
int32_t valve_pct10(void);       // position in 0.1 % of POS_MAX
bool valve_fault_latched(void);
bool valve_motion_blocked(void); // a latched fault that stops the motor (not F-01, F-03)
const char *valve_status(void);  // one status line

// Settings, in the order POSMAX FHOME FCREEP BACKOFF CLEAR OFFSET (cfg.c).
#define VALVE_N_SETTINGS 6
const char *valve_setting_name(int i);
void valve_settings(int32_t v[VALVE_N_SETTINGS]);
// NULL if every value is in range, else the name of the first that is not.
const char *valve_settings_check(const int32_t v[VALVE_N_SETTINGS]);
// Sets all of them; false (nothing changed) if one is out of range or the
// valve is calibrating or moving.
bool valve_set_settings(const int32_t v[VALVE_N_SETTINGS]);
// For the screens (ui_*.c). Each refusal sets *why to a short reason.
bool valve_calibrate(const char **why);          // VALVE CAL
void valve_uncalibrate(const char *why);         // e.g. motor direction changed
// Takes a saved step count as the position (operator's choice at boot).
bool valve_restore(int32_t count, const char **why);
// Takes the valve as fully closed where it stands (0 %, homed without the
// switch): the operator knows it is seated. VALVE CLOSED.
bool valve_set_closed(const char **why);
uint32_t valve_cal_budget_s(void);               // longest calibration, s
uint32_t valve_cal_elapsed_s(void);              // into the calibration running, s; 0 if none
int valve_cal_step(void);    // -1 not calibrating, 0 moving off the switch, 1-3 step
bool valve_goto_pct10(int32_t pct10, const char **why);   // VALVE GOTO, 0.1 %
bool valve_goto_pct100(int32_t pct100, const char **why); // the same in 0.01 % (keypad)
// A move by steps from where the valve is (or is going), calibrated or not:
// the Valve page's Close, Open, -5 % and +5 % before a calibration. A
// closing move still stops at the switch (no question without a zero).
bool valve_move_steps(int32_t steps, const char **why);
int32_t valve_target_pct10(void);                // where the valve is going
int32_t valve_pct_milli(void);                   // position in 0.001 % (for 2 dp on screen)
int32_t valve_target_pct_milli(void);
void valve_stop(void);                           // CTRL STOP (latches F-12 in AUTO only)
bool valve_stop_latches(void);                   // STOP would latch F-12 now
// CTRL RESET; returns the number latched before, left = those kept.
int valve_reset(char *left, size_t n);
int valve_fault_count(void);
bool valve_fault_first(const char **tag, const char **text);   // highest priority
bool valve_fault_is(const char *tag);            // e.g. "F-12" latched
bool valve_question(int32_t *pct10);             // switch question waiting, where
bool valve_answer(bool set_zero, const char **why);   // VALVE SETZERO / VALVE SKIP

// Latched fault i (0 = highest priority, below valve_fault_count()).
typedef struct {
    const char *tag, *text;
    uint32_t age_ms;          // since it latched
    valve_mode_t mode;        // mode when it latched
    bool can_reset;           // its cause has gone
} valve_fault_t;
bool valve_fault_get(int i, valve_fault_t *f);

// Event log: i = 0 is the newest, below valve_event_count() (up to 200).
int valve_event_count(void);
uint32_t valve_event_total(void);                // since boot
bool valve_event_get(int i, uint32_t *age_ms, const char **tag, const char **text);
bool valve_last_cal(uint32_t *age_ms, uint32_t *took_s);   // false: not since boot
const char *valve_last_cal_iso(void);   // "YYYY-MM-DD HH:MM:SS" then, "" if the clock was not set
// While the closed switch is pressed: steps closed past where it opened.
bool valve_beyond_switch(int32_t *steps);

// Adds an event to the log (tag up to 5 characters).
void valve_event(const char *tag, const char *fmt, ...);

#endif // _VALVE_H_
