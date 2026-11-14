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
 * Modes: OFF (every start, 0.15.1; Shut down below), NOT CALIBRATED
 * (turned on not homed, after F-2/F-3 or an aborted calibration),
 * CALIBRATING, MANUAL and AUTO (stage 8, ctrl.c), SHUTDOWN. Nothing moves
 * at boot.
 *
 * AUTO (from MANUAL) after the entry checks (ctrl_check), with
 * the target set; bumpless from where the valve is going. Every second,
 * after the pressure read, ctrl_step works out the valve command and the
 * checks. A fault stops the valve and drops to MANUAL, holding where it
 * stopped (the sensor fault too). W-2 (no pressure response) stops it and
 * drops to MANUAL too, the valve idle (no PAUSED state since 0.11.25, Ryan
 * 22 Oct); the strip shows W-2 until OK or the next Start AUTO. So does
 * W-1 away from the seat. AUTO does not end for being off the setpoint
 * (no convergence timeout from 0.16.33). Leaving AUTO ramps the valve down and
 * MANUAL holds where it stops. MANUAL moves, homing and the valve settings
 * are refused in AUTO ("Stop AUTO first" on the screen from 0.15.3; ERR 202 "Switch to MANUAL first" over the network).
 *
 * Calibration (Control Logic 4): if the switch is pressed, an N_clear
 * clearance move off it first (F-3 if still pressed); a fast close at
 * f_home for up to 1.1 x POS_MAX (F-2 if the switch is not found); an
 * N_backoff back-off (F-3 if still pressed); a creep close at f_creep
 * (F-2 if not found); the switch point is N_offset (0 %).
 *
 * W-1, the switch pressed outside homing, once calibrated: a closing move
 * stops at it (stepper3.c), and pressed away from the seat, moving or not,
 * counts too and ends AUTO (its own warning W-4 to 0.16.32). A W-1 event,
 * and a question waits on the screen (this point is the seat, or keep the
 * count).
 *
 * Faults latch, stop the motor and lock out moves until ALARM RESET (or
 * Reset on the screen), which clears only the latches whose cause has gone.
 * Three from 0.16.33 (Ryan 14 Nov, renumbered):
 *   F-1 no pressure signal (also the excitation under 7 V or tripped: F-2
 *       to 0.16.32),
 *   F-2 switch not found while homing (F-3 to 0.16.32),
 *   F-3 switch still pressed after moving off it, or pressed during an
 *       opening move (F-4 to 0.16.32).
 *   F-1 does not block manual moves. STOP latches nothing; in AUTO it
 *   drops to MANUAL.
 * Two warnings, nothing latched: W-1 above and W-2 no pressure response in
 * AUTO (the fault F-6 to 0.16.32). Removed in 0.16.33: not settling (the
 * old W-2) and move timed out (W-5; an event only).
 * Outside AUTO, STOP stops and holds the valve without a latch (STOP event).
 *
 * The remote commands are in remote.c (SETPOINT, MODE, VALVE SET, STOP, ALARM,
 * EVENT?); they call the functions below, as the screens do.
 * Events go to the log only; nothing is sent unasked.
 */

// The numbers are the Modbus mode register (docs/modbus-register-map.md).
typedef enum { VALVE_NOT_CAL, VALVE_CALIBRATING, VALVE_MANUAL, VALVE_AUTO, VALVE_SHUTDOWN, VALVE_OFF } valve_mode_t;

void valve_init(void);           // after stepper3_init(); logs BOOT power-up
void valve_task(void);           // every main-loop pass: calibration, limit hits
void valve_sense(const clicks_state_t *s);   // after each 1 s pressure read

valve_mode_t valve_mode(void);
const char *valve_mode_name(void);
bool valve_calibrated(void);
// How the position was last set: homed on the switch, or set by the
// operator (Set as 0 % or a typed %) or from the saved position, neither
// checked against the switch, or kept from the last Shut down of a homed
// valve (PARKED; not homed before it: SAVED). NONE when not homed.
typedef enum { VALVE_REF_NONE, VALVE_REF_HOMED, VALVE_REF_HAND, VALVE_REF_SAVED, VALVE_REF_PARKED } valve_ref_t;
valve_ref_t valve_reference(void);
int32_t valve_pos_max(void);
// Pass band, "Disable switch" on screen from 0.16.22 ("Ignore switch" in
// 0.16.21, Ryan 13 Nov: "change ignore to disable") (0.16.5, Ryan 9 Nov:
// the valve binds about 13 % and the force switch trips there, off the
// seat): while on and homed, between valve_pass_lo() and valve_pass_hi() %
// open the switch neither stops closing nor raises W-1 or F-3. Motor
// settings > Override (0.16.19). RAM only, off at power-up. The edges are
// saved settings from 0.16.21 (Ryan 12 Nov: "make the boundary editable";
// CFG_PASS_LO and CFG_PASS_HI, factory 5 and 50, 0 to 100 % from 0.16.22). Fixed before: 10 to 20 %
// until 0.16.8, 5 to 25 % until 0.16.11, then 5 to 50 %.
bool valve_limit_band(void);
void valve_limit_band_set(bool on);
int valve_pass_lo(void);         // % open
int valve_pass_hi(void);
bool valve_motion_blocked(void); // a latched fault that stops the motor (not F-1)

// AUTO (stage 8).
bool valve_auto(void);
// The strip text of a warning not yet acknowledged (W-2), or
// NULL; cleared by OK, Start AUTO and homing.
const char *valve_warning(void);
void valve_warning_clear(void);
bool valve_start_auto(const char **why);         // MODE AUTO, Start / Resume AUTO
// Start AUTO at a new target (mmbar, 0.001 mbar): the target changes only
// if AUTO starts (Home's Start AUTO keypad). Refused in AUTO: a new target
// there goes through the setpoint keypad and its confirm.
bool valve_start_auto_at(int32_t mmbar, const char **why);
// A screen button drives the valve while pressed (true at its press, false
// at its release): AUTO and remote moves are refused meanwhile.
void valve_hold(bool on);
bool valve_to_manual(const char **why);          // MODE MANUAL, Switch to MANUAL
// Target in 0.001 mbar, kept to 0.1 mbar; refused outside 0 to the range.
bool valve_set_target(int32_t mmbar, const char **why);
bool valve_target(int32_t *mmbar);               // false: no target set yet
int32_t valve_range_mmbar(void);                 // sensor in use, full scale
ctrl_reg_t valve_regulation(void);               // status words, in AUTO
int valve_pid_set(void);                         // PID set in use (AUTO) or AUTO would start with, 1 to 3

// Settings, in the order POSMAX FHOME FCREEP BACKOFF CLEAR RETURN (cfg.c).
#define VALVE_N_SETTINGS 6
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
// Takes the present position as 0 % (count 0, reads 0.00 %), without homing.
bool valve_set_closed(const char **why);
// The same, taking the present position as pct100 (0.01 %, 0 to 10000;
// Set as % on Valve, 0.17.0).
bool valve_set_position(int32_t pct100, const char **why);
uint32_t valve_cal_budget_s(void);               // longest calibration, s
int valve_cal_step(void);    // -1 not calibrating, 0 moving off the switch, 1-4 step
bool valve_goto_pct10(int32_t pct10, const char **why);   // VALVE SET, 0.1 %
bool valve_goto_pct100(int32_t pct100, const char **why); // the same in 0.01 % (keypad)
int32_t valve_pct_milli(void);                   // position in 0.001 % (for 2 dp on screen)
int32_t valve_target_pct_milli(void);
void valve_stop(void);                           // STOP (in AUTO: MANUAL, nothing latched)
bool valve_stop_latches(void);                   // STOP would end AUTO now (red STOP button)
// ALARM RESET; returns the number latched before, left = those kept.
int valve_reset(char *left, size_t n);
int valve_fault_count(void);
bool valve_fault_first(const char **tag, const char **text);   // highest priority
bool valve_fault_is(const char *tag);            // e.g. "F-1" latched
bool valve_question(int32_t *pct10);             // switch question waiting, where
bool valve_answer(bool set_zero, const char **why);   // VALVE SETZERO / VALVE SKIP

/* Shut down (0.15.0, Ryan 4 Nov; to 0 % from 0.15.1, Ryan: "should not
 * home to limit, just go to 0"): AUTO ends, the valve goes to 0 % at the
 * run rate and the coils go off: OFF. Not homed: OFF at once, the valve
 * not moved. Moves, AUTO and homing are refused while OFF (pressure is
 * still read); a double tap on the screen turns it on (MANUAL, or NOT
 * HOMED). STOP cancels a shut down under way (MANUAL, the valve where it
 * stopped); so does the limit switch before 0 % (MANUAL and the switch
 * question). Every start is OFF (0.15.1). close false (0.15.7, Ryan: a
 * choice on the Shut down question): OFF at once, the valve left where it
 * is, its count kept as at 0 %. A latched fault refuses only a close. */
bool valve_shutdown(bool close, const char **why);
bool valve_turn_on(const char **why);            // OFF -> MANUAL (NOT HOMED if not homed)
// valvepos.c at start-up: the count kept at the last Shut down, the limit
// switch released. homed: the valve was homed (PARKED), else SAVED. Stays
// OFF (MANUAL if already turned on).
bool valve_restore_parked(int32_t count, bool homed, const char **why);
// While shutting down: how much of the move is left (100 at the start, 0
// at 0 %) and the seconds left at the run rate.
bool valve_shutdown_progress(int *left_pct, int *secs);
// Why moves and mode changes are refused now: off or shutting down; NULL
// otherwise.
const char *valve_off_why(void);

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

// Adds an event to the log (tag up to 5 characters).
void valve_event(const char *tag, const char *fmt, ...);
// EVENT? <n>: "EVENT <n>" now, then n lines "<age s>,<tag>,<text>", newest
// first, sent from valve_task as the link has room.
void valve_events_send(int n);
// An EVENT? dump is still going out: main.c holds the next command line
// (up to 0.5 s), so no reply lands among its lines. _stop drops the rest
// (a new client).
bool valve_events_sending(void);
void valve_events_stop(void);

#endif // _VALVE_H_
