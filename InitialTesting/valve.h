#ifndef _VALVE_H_
#define _VALVE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "clicks.h"
#include "ctrl.h"

/**
 * @brief Valve layer over stepper3.c: calibration to the closed switch,
 * position in % of POS_MAX, fault latches, STOP and the event log.
 * Ported from the simulator (stage 2 of the porting plan). Settings are kept
 * by CFG SAVE (cfg.c, stage 3).
 *
 * Modes: NOT CALIBRATED (boot, watchdog reset, after F-03/F-04 or an
 * aborted calibration), CALIBRATING, MANUAL and AUTO (stage 8,
 * ctrl.c). Nothing moves at boot.
 *
 * AUTO (from MANUAL) after the entry checks (ctrl_check), with
 * the target set; bumpless from where the valve is going. Every second,
 * after the pressure read, ctrl_step works out the valve command and the
 * checks. A fault stops the valve and drops to MANUAL, holding where it
 * stopped (sensor faults too). W-02 (not settling) stops it and drops to
 * MANUAL too, the valve idle (no PAUSED state since 0.11.25, Ryan 22 Oct);
 * the strip shows W-02 until OK or the next Start AUTO. Leaving AUTO ramps the valve down and
 * MANUAL holds where it stops. MANUAL moves, homing and the valve settings
 * are refused in AUTO ("Switch to MANUAL first").
 *
 * Calibration (Control Logic 4): if the switch is pressed, an N_clear
 * clearance move off it first (F-04 if still pressed); a fast close at
 * f_home for up to 1.1 x POS_MAX (F-03 if the switch is not found); an
 * N_backoff back-off (F-04 if still pressed); a creep close at f_creep
 * (F-03 if not found); the switch point is N_offset (0 %).
 *
 * The switch pressed during any other move, once calibrated: the move stops
 * (stepper3.c), a W-01 event, and a question waits for VALVE SETZERO (this
 * point is 0 %) or VALVE SKIP (count kept).
 *
 * Faults latch, stop the motor and lock out MOT moves until CTRL RESET,
 * which clears only the latches whose cause has gone:
 *   F-01 no pressure signal, F-02 excitation outside 9.5-10.5 V or tripped,
 *   F-03 switch not found while calibrating, F-04 switch still pressed
 *   after moving off it, F-05 motion timeout; in AUTO only: F-06 no
 *   pressure response. F-01 and F-02 do not block manual moves. STOP
 *   latches nothing; in AUTO it drops to MANUAL.
 * Outside AUTO, STOP stops and holds the valve without a latch (STOP event).
 *
 * Commands:
 *   VALVE?                       mode, position, settings, pending question
 *   VALVE CAL                    calibrate (MANUAL or NOT CALIBRATED, no fault)
 *   VALVE GOTO <0-100>           go to % open (one decimal), once calibrated
 *   VALVE SETZERO | VALVE SKIP   answer the switch-pressed question (or the screen)
 *   VALVE CLOSED                 take the valve as closed (0 %) where it stands, homed
 *   VALVE POSMAX|FHOME|FCREEP|BACKOFF|CLEAR|OFFSET <n>   settings (idle)
 *   CTRL AUTO                    start AUTO, after the entry checks
 *   CTRL MAN [<0-100>]           to MANUAL (ramped stop), then optionally go to %
 *   CTRL SP <mbar>               target, 0.1 mbar, 0 to the sensor range
 *   CTRL STOP                    stop now, cancel calibration (AUTO ends)
 *   CTRL RESET                   clear latches whose cause has gone
 *   CTRL?                        latched faults, highest priority first
 *   EVT? [n]                     the last n events (default 20, 100 kept)
 * Each event is also sent when it happens as "EVT <s> <tag> <text>".
 */

typedef enum { VALVE_NOT_CAL, VALVE_CALIBRATING, VALVE_MANUAL, VALVE_AUTO } valve_mode_t;

void valve_init(void);           // after stepper3_init(); logs BOOT power-up
void valve_task(void);           // every main-loop pass: calibration, limit hits
void valve_sense(const clicks_state_t *s);   // after each 1 s pressure read
bool valve_command(const char *line);

valve_mode_t valve_mode(void);
const char *valve_mode_name(void);
bool valve_calibrated(void);
// How the position was last set: homed on the switch, or set by the
// operator (closed or a typed %) or from the saved position, neither
// checked against the switch. NONE when not homed.
typedef enum { VALVE_REF_NONE, VALVE_REF_HOMED, VALVE_REF_HAND, VALVE_REF_SAVED } valve_ref_t;
valve_ref_t valve_reference(void);
int32_t valve_pos_max(void);
int32_t valve_pct10(void);       // position in 0.1 % of POS_MAX
bool valve_fault_latched(void);
bool valve_motion_blocked(void); // a latched fault that stops the motor (not F-01, F-02)
const char *valve_status(void);  // one status line

// AUTO (stage 8).
bool valve_auto(void);
// W-02 fired since the last Start AUTO, not yet acknowledged.
bool valve_w02(void);
void valve_w02_clear(void);
bool valve_start_auto(const char **why);         // CTRL AUTO, Start / Resume AUTO
bool valve_to_manual(const char **why);          // CTRL MAN, Switch to MANUAL
// Target in 0.001 mbar, kept to 0.1 mbar; refused outside 0 to the range.
bool valve_set_target(int32_t mmbar, const char **why);
bool valve_target(int32_t *mmbar);               // false: no target set yet
int32_t valve_range_mmbar(void);                 // sensor in use, full scale
int32_t valve_band_mmbar(void);                  // target band (BAND)
ctrl_reg_t valve_regulation(void);               // status words, in AUTO
// "AUTO 4.62 mB, target 4.8, valve 14.20 % to 14.35 %, In band": sent once a
// second in AUTO (sense.c), in place of the motor and valve status lines.
const char *valve_auto_line(void);

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
// The same, taking the present position as pct100 (0.01 %, 0 to 10000).
bool valve_set_position(int32_t pct100, const char **why);
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
void valve_stop(void);                           // CTRL STOP (in AUTO: MANUAL, nothing latched)
bool valve_stop_latches(void);                   // STOP would end AUTO now (red STOP button)
// CTRL RESET; returns the number latched before, left = those kept.
int valve_reset(char *left, size_t n);
int valve_fault_count(void);
bool valve_fault_first(const char **tag, const char **text);   // highest priority
bool valve_fault_is(const char *tag);            // e.g. "F-05" latched
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

// Event log: i = 0 is the newest, below valve_event_count() (up to 100).
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
