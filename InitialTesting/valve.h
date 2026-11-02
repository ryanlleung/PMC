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
 * Modes: NOT CALIBRATED (boot, watchdog reset, after F-3/F-4 or an
 * aborted calibration), CALIBRATING, MANUAL and AUTO (stage 8,
 * ctrl.c). Nothing moves at boot.
 *
 * AUTO (from MANUAL) after the entry checks (ctrl_check), with
 * the target set; bumpless from where the valve is going. Every second,
 * after the pressure read, ctrl_step works out the valve command and the
 * checks. A fault stops the valve and drops to MANUAL, holding where it
 * stopped (sensor faults too). W-2 (not settling) stops it and drops to
 * MANUAL too, the valve idle (no PAUSED state since 0.11.25, Ryan 22 Oct);
 * the strip shows W-2 until OK or the next Start AUTO. Leaving AUTO ramps the valve down and
 * MANUAL holds where it stops. MANUAL moves, homing and the valve settings
 * are refused in AUTO ("Switch to MANUAL first").
 *
 * Calibration (Control Logic 4): if the switch is pressed, an N_clear
 * clearance move off it first (F-4 if still pressed); a fast close at
 * f_home for up to 1.1 x POS_MAX (F-3 if the switch is not found); an
 * N_backoff back-off (F-4 if still pressed); a creep close at f_creep
 * (F-3 if not found); the switch point is N_offset (0 %).
 *
 * The switch pressed during any other move, once calibrated: the move stops
 * (stepper3.c), a W-1 event, and a question waits on the screen (this point
 * is 0 %, or keep the count).
 *
 * Faults latch, stop the motor and lock out moves until ALARM RESET (or
 * Reset on the screen), which clears only the latches whose cause has gone:
 *   F-1 no pressure signal, F-2 excitation outside 9.5-10.5 V or tripped,
 *   F-3 switch not found while calibrating, F-4 switch still pressed
 *   after moving off it, F-5 motion timeout; in AUTO only: F-6 no
 *   pressure response. F-1 and F-2 do not block manual moves. STOP
 *   latches nothing; in AUTO it drops to MANUAL.
 * Outside AUTO, STOP stops and holds the valve without a latch (STOP event).
 *
 * The remote commands are in remote.c (PRES SP, MODE, VALV SET, STOP, ALRM,
 * EVNT?); they call the functions below. Engineering commands
 * (PMC_ENG_COMMANDS): VALVE?, CTRL?, VALVE CAL, VALVE CLOSED, VALVE SETZERO |
 * SKIP, VALVE POSMAX|FHOME|FCREEP|BACKOFF|RETURN|CLEAR <n>, TUNE.
 * Events go to the log only; nothing is sent unasked.
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
bool valve_motion_blocked(void); // a latched fault that stops the motor (not F-1, F-2)
const char *valve_status(void);  // one status line

// AUTO (stage 8).
bool valve_auto(void);
// The strip text of a warning not yet acknowledged (W-2, W-4, W-5, W-6), or
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
int32_t valve_band_mmbar(void);                  // target band (BAND)
ctrl_reg_t valve_regulation(void);               // status words, in AUTO

// Settings, in the order POSMAX FHOME FCREEP BACKOFF CLEAR (unused, was
// OFFSET: always 0) RETURN
// (cfg.c; RETURN from 0.13.6).
#define VALVE_N_SETTINGS 7
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
int valve_cal_step(void);    // -1 not calibrating, 0 moving off the switch, 1-4 step
bool valve_goto_pct10(int32_t pct10, const char **why);   // VALVE SET, 0.1 %
bool valve_goto_pct100(int32_t pct100, const char **why); // the same in 0.01 % (keypad)
// A move by steps from where the valve is (or is going), calibrated or not:
// the Valve page's Close, Open, -5 % and +5 % before a calibration. A
// closing move still stops at the switch (no question without a zero).
bool valve_move_steps(int32_t steps, const char **why);
int32_t valve_target_pct10(void);                // where the valve is going
int32_t valve_pct_milli(void);                   // position in 0.001 % (for 2 dp on screen)
int32_t valve_target_pct_milli(void);
void valve_stop(void);                           // STOP (in AUTO: MANUAL, nothing latched)
// STOP on the screen: valve_stop, and the network's moves (VALV SET, MODE
// AUTO, Modbus valve % and AUTO coil) are held until valve_remote_allow
// (Allow on the screen), so a master writing every scan cannot undo it.
void valve_stop_local(void);
bool valve_remote_held(void);
void valve_remote_allow(void);
#define VALVE_REMOTE_HELD_WHY "Stopped on the screen: Allow remote moves there"
bool valve_stop_latches(void);                   // STOP would end AUTO now (red STOP button)
// ALARM RESET; returns the number latched before, left = those kept.
int valve_reset(char *left, size_t n);
int valve_fault_count(void);
bool valve_fault_first(const char **tag, const char **text);   // highest priority
bool valve_fault_is(const char *tag);            // e.g. "F-5" latched
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

// Adds an event to the log (tag up to 5 characters).
void valve_event(const char *tag, const char *fmt, ...);
// EVNT? <n>: "EVNT <n>" now, then n lines "<age s>,<tag>,<text>", newest
// first, sent from valve_task as the link has room.
void valve_events_send(int n);
// An EVNT? dump is still going out: main.c holds the next command line
// (up to 0.5 s), so no reply lands among its lines. _stop drops the rest
// (a new client).
bool valve_events_sending(void);
void valve_events_stop(void);

#endif // _VALVE_H_
