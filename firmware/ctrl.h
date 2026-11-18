#ifndef _CTRL_H_
#define _CTRL_H_

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief AUTO control step. Plain C with no hardware calls, so
 * tools/ctrl-test runs it on the PC against a model of the flow line.
 * valve.c owns the mode, the motor and the faults; this file only works out
 * the next valve command and the checks.
 *
 * Once per Ts (1 s), straight after the pressure read:
 *   - PID in position form, D on the measurement (KD, 0 is off), output in
 *     steps. Bumpless entry: the first output is the position in force. A
 *     KP change in AUTO re-bases the integral the same way. Opening the
 *     valve raises the pressure.
 *   - The proportional part acts on the pressure only: a setpoint change
 *     moves the integral by KP x the change the other way, so the output
 *     does not jump with the setpoint.
 *   - Anti-windup: no integration past POS_MAX or AUTO min in the direction
 *     of the error, while the closed switch blocks closing, or while the valve
 *     is still more than CTRL_LEAD_MAX steps short of the last command.
 *   - Commands within the deadband (DB steps, either way) of the last one
 *     sent are held. A larger DB opens in one move past the needle's stick
 *     instead of creeping (on the bench the needle needs 0.3 to 0.44 % to
 *     break free when opening; closing answers at once).
 *   - PID table: three or four zones of KP, TI and KD, picked by the
 *     pressure against the boundaries, as industrial controllers schedule
 *     their gains. Below B1 zone 1, B1 to B2 zone 2, B2 to B3 zone 3, above
 *     B3 zone 4 (B3 0: three zones). A zone changes only once the pressure
 *     is past the boundary by the hysteresis (H %), so noise at a boundary
 *     cannot flip it. A change of zone re-bases the integral, as a KP
 *     change does, so the valve does not jump. The zone follows the LOWER
 *     of the pressure and the setpoint: going up nothing changes; going down
 *     the setpoint's zone is in charge from the start, so a big step down
 *     does not start on the high zone's KP and shut the valve.
 *   - Response monitor: every CTRL_TCHECK s, the counted position through a
 *     CTRL_TLAG s lag against the pressure change. W-2 after CTRL_NRESP
 *     windows with no response: AUTO ends, MANUAL, valve idle, nothing
 *     latched. A window with the valve pinned fully open, or at AUTO min,
 *     while the loop asks for more is judged too. A wrong-way change counts
 *     as a response.
 *   - No convergence timeout: a cool-down or a setpoint out of reach looks
 *     the same, and the loop recovers by itself if it stays in AUTO.
 *     Regulating / In band / Stable say where the pressure is.
 *   - Less hunting: a move the opposite way to the last one is only sent
 *     once the command is CTRL_REV_MBAR of error at the zone's KP from the
 *     valve, at most CTRL_REV_PCT of travel: 0.10 % at KP 0.5 and up, 0.012 %
 *     at KP 0.06. Otherwise the reading's noise would turn the motor round
 *     about every 2 s, inside the drive's backlash. AUTO moves run at the
 *     Speed setting.
 */

// Fixed; the tuning in cfg.c is the PID table, OUTMIN, DB and BAND.
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
#define CTRL_REV_MBAR   0.2f    // the reversal hold is this much error at the zone's KP, up to rev_pct

typedef struct {
    float kp, ti, kd;  // as zone 1 below
} ctrl_pid_t;

typedef struct {
    float kp;          // zone 1: gain, % of POS_MAX per mbar of error
    float ti;          // integral time, min (0 = no integral)
    int32_t db;        // deadband opening, steps
    float band;        // target band, mbar
    float outmin;      // AUTO min, % open
    int32_t pos_max;   // steps for 100 %
    float kd;          // derivative, % of POS_MAX per mbar/s of pressure change (0 = off)
    // The PID table (always on, by the pressure).
    ctrl_pid_t sets[3];    // zones 2, 3 and 4
    float b1, b2;          // boundaries, mbar: zone 1 below b1, zone 2 to b2, zone 3 from b2
    float b3;              // boundary zone 3 / zone 4, mbar; not above b2 (0 included): three zones
    float hyst;            // hysteresis on the pressure, fraction of the boundary
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
    int set;           // PID zone in use, 1 to 4 (0 before AUTO first starts)
    float kd_used;     // derivative of the last step: a change re-bases too
    float e_last;      // setpoint less pressure at the last step (before a setpoint change), mbar: the re-base keeps the output there
    float sp_used;     // the setpoint the integral is based on: a change moves the integral by KP x the change, so P acts on the pressure only
} ctrl_t;

void ctrl_init(ctrl_t *c);
// The PID zone the table gives for x (in AUTO the lower of the pressure
// and the setpoint), from zone now (1 to 4; 0 picks with no hysteresis).
int ctrl_pick_set(const ctrl_tune_t *k, int now, float x);
// That zone's gains.
ctrl_pid_t ctrl_set_gains(const ctrl_tune_t *k, int set);
// How many zones the PID table has: 4 when boundary 3 is above boundary
// 2, else 3.
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
