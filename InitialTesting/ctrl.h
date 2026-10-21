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
 *   - PI in position form, D on the measurement (Kd 0, off), output in steps.
 *     Bumpless entry: the first output is the position in force.
 *   - Anti-windup: no integration past POS_MAX or Auto min in the direction
 *     of the error, while the closed switch blocks closing, or while the valve
 *     is still more than CTRL_LEAD_MAX steps short of the last command.
 *   - Commands within the deadband (DB steps) of the last one sent are held.
 *   - Response monitor: every CTRL_TCHECK s, the counted position through a
 *     CTRL_TLAG s lag against the pressure change. F-10 after CTRL_NRESP
 *     windows with no response, F-16 (opening, pressure falling) and F-17
 *     (closing, pressure rising) after two windows in a row. A window with
 *     the valve pinned fully open, or at Auto min, while the loop asks for
 *     more is judged too.
 *   - F-02: pressure above the sensor range for 5 s.
 *   - W-02: outside the target band for CTRL_TCONV s; AUTO pauses.
 */

// Fixed for now (the simulator's DEF_P); the tuning in cfg.c is KP, TI,
// OUTMIN, DB, KDIR and BAND.
#define CTRL_TS_S       1       // control step, s
#define CTRL_KD         0.0f    // derivative, % per mbar/s (off)
#define CTRL_TCHECK_S   30      // response window, s
#define CTRL_NRESP      5       // windows with no response before F-10
#define CTRL_DPMIN      0.05f   // smallest pressure change that counts, mbar
#define CTRL_CHECKMIN   1000    // smallest lagged move that is judged, steps
#define CTRL_TCONV_S    1800    // W-02 after this long outside the band, s
#define CTRL_LEAD_MAX   3000    // travel anti-windup, steps
#define CTRL_TLAG_S     40      // process lag for the response monitor, s
#define CTRL_STABLE_S   60      // in band this long reads Stable, s
#define CTRL_F02_S      5       // above the sensor range this long, s
#define CTRL_SP_CONFIRM 5.0f    // target changes larger than this ask first, mbar

typedef struct {
    float kp;          // gain, % of POS_MAX per mbar of error
    float ti;          // integral time, min (0 = no integral)
    int32_t db;        // deadband, steps
    int kdir;          // +1 opening raises the pressure, -1 lowers it, 0 not set
    float band;        // target band, mbar
    float outmin;      // Auto min, % open
    int32_t pos_max;   // steps for 100 %
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
    CTRL_F02,          // above the sensor range for 5 s
    CTRL_F10,          // no pressure response
    CTRL_F16,          // opening while the pressure kept falling
    CTRL_F17,          // closing while the pressure kept rising
    CTRL_W02           // not settling: pause
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
    int noresp, wrong, wrong_way;
    uint32_t conv_ms;  // last time in band (or target set, or AUTO started)
    bool f02_on;
    uint32_t f02_ms;
    bool in_band;
    uint32_t in_band_ms;
    bool move;         // ctrl_step asks for a move to move_to
    int32_t move_to;
} ctrl_t;

void ctrl_init(ctrl_t *c);
// Every Ts in every mode: the lagged position follows the count (only
// through the lag once the valve is homed and not homing).
void ctrl_track(ctrl_t *c, int32_t pos, bool lagged);
// Why AUTO cannot start, with the simulator's wording, or NULL. pv_ok false
// when there is no valid reading.
const char *ctrl_check(const ctrl_t *c, const ctrl_tune_t *k, bool homed, bool fault,
                       bool pv_ok, float pv, float fs);
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
