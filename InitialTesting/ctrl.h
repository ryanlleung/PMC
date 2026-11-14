#ifndef _CTRL_H_
#define _CTRL_H_

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief AUTO control step (stage 8 of the porting plan), ported from the
 * simulator's toAuto() and ctrlTick() (v63). Plain C with no hardware calls,
 * so tools/ctrl-test runs it on the PC against the simulator's flow-line
 * model. valve.c owns the mode, the motor and the faults; this file only
 * works out the next valve command and the checks.
 *
 * Once per Ts (1 s), straight after the pressure read:
 *   - PID in position form, D on the measurement (KD, 0 is off; a setting
 *     from 0.14.21), output in steps.
 *     Bumpless entry: the first output is the position in force. A KP
 *     change in AUTO re-bases the integral the same way (0.11.38). Opening
 *     the valve raises the pressure: the Pressure response setting (KDIR)
 *     went in 0.16.30.
 *   - Anti-windup: no integration past POS_MAX or AUTO min in the direction
 *     of the error, while the closed switch blocks closing, or while the valve
 *     is still more than CTRL_LEAD_MAX steps short of the last command.
 *   - Commands within the deadband of the last one sent are held: DB steps
 *     opening, DB close closing (0.15.15). A larger DB opens in one move
 *     past the needle's stick instead of creeping (5 Nov bench: 0.3 to
 *     0.44 % to break free opening; closing answers at once).
 *   - Gain schedule (0.15.15, Ryan 5 Nov "build like industry ones"): three
 *     PID sets, picked by the pressure (PV) or the setpoint (SP) against two
 *     boundaries, as industrial controllers do (Eurotherm EPC3000 gain
 *     scheduling). Below B1 set 1, B1 to B2 set 2, B2 up set 3. On the
 *     pressure a set changes only once the pressure is past the boundary
 *     by the hysteresis (H %), so noise at a boundary cannot flip it. The
 *     table is always on (0.16.20; the Off branches went in 0.16.30).
 *     A change of set re-bases the integral, as a KP change
 *     does, so the valve does not jump. Since 0.16.9 (Ryan 10 Nov, from the
 *     bench model thread) the set follows the LOWER of the pressure and the
 *     setpoint: going up nothing changes; going down the setpoint's set is
 *     in charge from the start, so a big step down no longer starts on the
 *     high set's KP and shuts the valve (bench model, today's gains: 12 to
 *     8 mbar 91 % under before, 7 % after).
 *   - Response monitor: every CTRL_TCHECK s, the counted position through a
 *     CTRL_TLAG s lag against the pressure change. W-2 (the fault F-6 to
 *     0.16.32) after CTRL_NRESP windows with no response: AUTO ends,
 *     MANUAL, valve idle, nothing latched. A window with the valve pinned
 *     fully open, or at AUTO min, while the loop asks for more is judged
 *     too. A wrong-way change counts as a response (no fault since 0.11.20).
 *   - The convergence timeout (W-2 to 0.16.32: outside the target band for
 *     30 min ended AUTO) is removed in 0.16.33 (Ryan 14 Nov): a cool-down
 *     or a setpoint out of reach looks the same, and the loop recovers by
 *     itself if it stays in AUTO. Regulating / In band / Stable still say
 *     where the pressure is.
 *   - The wrong-way warning (0.11.45 to 0.15.10, pinned at the wrong end
 *     for 60 s) is removed: it tripped at low pressure, where the valve
 *     rightly rests near closed (Ryan 5 Nov).
 *   - Less hunting (0.12.12, Ryan 26 Oct): a move the opposite
 *     way to the last one is only sent once the command is CTRL_REV_PCT of
 *     travel from the valve. Before, the reading's noise turned the motor
 *     round about every 2 s, inside the drive's backlash. (The pressure
 *     filter of 0.12.12 was 0 s, off, and went in 0.16.30.) Since 0.16.9 the
 *     hold is CTRL_REV_MBAR of error at the set's KP, at most CTRL_REV_PCT:
 *     0.10 % at KP 0.5 and up, 0.012 % at KP 0.06. At the low KPs of the
 *     low pressures a fixed 0.10 % held the valve far longer than the
 *     error took to change sign (bench model: 2 mbar step down 24 % under
 *     before, 6 % after). AUTO moves run at
 *     MOT RATE (the distance-based speed of 0.12.5 was taken out, Ryan 26 Oct).
 */

// Fixed for now (the simulator's DEF_P); the tuning in cfg.c is KP, TI,
// OUTMIN, DB, BAND and KD.
#define CTRL_TS_S       1       // control step, s
#define CTRL_TCHECK_S   30      // response window, s
#define CTRL_NRESP      5       // windows with no response before W-2
#define CTRL_DPMIN      0.05f   // smallest pressure change that counts, mbar
#define CTRL_CHECKMIN   1000    // smallest lagged move that is judged, steps
#define CTRL_LEAD_MAX   3000    // travel anti-windup, steps
#define CTRL_TLAG_S     40      // process lag for the response monitor, s
#define CTRL_STABLE_S   60      // in band this long reads Stable, s
#ifndef CTRL_REV_PCT
#define CTRL_REV_PCT    0.10f   // a reversal waits until the command is this far away, % of travel (rev_pct): the most
#endif
#define CTRL_REV_MBAR   0.2f    // the reversal hold is this much error at the set's KP (0.16.9), up to rev_pct

typedef struct {
    float kp, ti, kd;  // as set 1 below
} ctrl_pid_t;

typedef struct {
    float kp;          // set 1: gain, % of POS_MAX per mbar of error
    float ti;          // integral time, min (0 = no integral)
    int32_t db;        // deadband opening, steps
    float band;        // target band, mbar
    float outmin;      // AUTO min, % open
    int32_t pos_max;   // steps for 100 %
    float kd;          // derivative, % of POS_MAX per mbar/s of pressure change (0 = off)
    // The PID table (always on, by the pressure).
    ctrl_pid_t sets[3];    // sets 2, 3 and 4 (set 4 from 0.16.14)
    float b1, b2;          // boundaries, mbar: set 1 below b1, set 2 to b2, set 3 from b2
    float b3;              // boundary set 3 / set 4, mbar (0.16.14); not above b2 (0: initialisers
                           // without it): three sets, as before
    float hyst;            // hysteresis on the pressure, fraction of the boundary (0.15)
} ctrl_tune_t;

typedef struct {
    uint32_t t_ms;     // now (wraps)
    float pv;          // pressure, mbar
    int32_t pos;       // step count now
    bool moving;       // a move is running
    bool limit;        // closed switch pressed (closing blocked)
    float fs;          // sensor range, mbar
} ctrl_in_t;

typedef enum {
    CTRL_RUN,          // carry on (a move may be requested)
    CTRL_NORESP,       // W-2 no pressure response: to MANUAL
} ctrl_result_t;

typedef enum { CTRL_REG_NONE, CTRL_REG_REGULATING, CTRL_REG_BAND, CTRL_REG_STABLE } ctrl_reg_t;

typedef struct {
    float sp;          // target, mbar
    bool sp_set;
    float i;           // integral term, steps
    int32_t pos_cmd;   // last command worked out, steps
    int32_t last_sent; // last command sent to the motor
    float prev_pv;
    float pos_f;       // counted position through the lag
    float win_t, win_pos, win_pv;
    int noresp;
    bool in_band;
    uint32_t in_band_ms;
    bool move;         // ctrl_step asks for a move to move_to
    int32_t move_to;
    float kp_used;     // gain (steps per mbar) of the last step: a change in AUTO re-bases the integral
    int last_dir;      // way of the last move sent: +1 opening, -1 closing, 0 none yet
    int move_dir;      // way of the move asked for
    float rev_pct;     // CTRL_REV_PCT from ctrl_init, the most the hold is; 0 off
    int set;           // PID set in use, 1 to 4 (0 before AUTO first starts)
    float kd_used;     // derivative of the last step: a change re-bases too
    float e_last;      // setpoint less pressure at the last step (before a setpoint change), mbar: the re-base keeps the output there (0.16.13)
    float sp_used;     // the setpoint the integral is based on: a change moves the integral by KP x the change, so P acts on the pressure only (0.16.15)
} ctrl_t;

void ctrl_init(ctrl_t *c);
// The PID set the schedule gives for x (in AUTO the lower of the pressure
// and the setpoint), from set now (1 to 4; 0 picks with no hysteresis).
int ctrl_pick_set(const ctrl_tune_t *k, int now, float x);
// That set's gains.
ctrl_pid_t ctrl_set_gains(const ctrl_tune_t *k, int set);
// How many sets the PID table has: 4 when boundary 3 is above boundary 2
// (0.16.14), else 3.
int ctrl_sets(const ctrl_tune_t *k);
// Every Ts in every mode: the lagged position follows the count (only
// through the lag once the valve is homed and not homing).
void ctrl_track(ctrl_t *c, int32_t pos, bool lagged);
// Why AUTO cannot start, with the simulator's wording, or NULL. pv_ok false
// when there is no valid reading.
const char *ctrl_check(const ctrl_t *c, const ctrl_tune_t *k, bool homed, bool fault,
                       bool pv_ok, float fs);
// Enters AUTO (or resumes it) with now_pos as the first output.
void ctrl_start(ctrl_t *c, const ctrl_tune_t *k, uint32_t t_ms, float pv, int32_t now_pos);
// Target, rounded to 0.1 mbar; false outside 0 to fs.
bool ctrl_set_sp(ctrl_t *c, float sp, float fs, uint32_t t_ms);
// One step in AUTO. On CTRL_RUN, c->move says whether to send c->move_to;
// call ctrl_sent() once the motor has taken it.
ctrl_result_t ctrl_step(ctrl_t *c, const ctrl_tune_t *k, const ctrl_in_t *in);
void ctrl_sent(ctrl_t *c);
// Regulation words for the status line, in AUTO.
ctrl_reg_t ctrl_regulation(const ctrl_t *c, const ctrl_tune_t *k, uint32_t t_ms, bool pv_ok, float pv);
// Leaves the band timing; called outside AUTO.
void ctrl_idle(ctrl_t *c);

#endif // _CTRL_H_
