#include <math.h>
#include <string.h>

#include "ctrl.h"

/* --------------------------------------------------------------------------
 * Ported from the simulator v63 (toAuto, setSP, ctrlTick, regulation). The
 * names follow it: I, posCmd, lastSent, posF, winT/winPos/winPV, noresp,
 * convStart, inBandSince.
 * ------------------------------------------------------------------------ */

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static int sgnf(float v) { return v > 0 ? 1 : v < 0 ? -1 : 0; }
static uint32_t since(uint32_t now, uint32_t then) { return now - then; }   // wraps

// % of travel per unit in steps.
static float steps_of(float pct, const ctrl_tune_t *k) { return pct / 100.0f * (float)k->pos_max; }

int ctrl_pick_set(const ctrl_tune_t *k, int now, float x)
{
    if (k->sched != CTRL_SCHED_PV)
        return 1;
    // Four sets when boundary 3 is above boundary 2 (0.16.14), else three.
    const int top = ctrl_sets(k);
    // With no set yet, straight from the boundaries.
    if (now < 1 || now > top)
        return x < k->b1 ? 1 : x < k->b2 ? 2 : top == 3 || x < k->b3 ? 3 : 4;
    // On the pressure, a set holds until the pressure is past its boundary
    // by the hysteresis; a large move can pass several in one step.
    const float up = 1.0f + k->hyst, down = 1.0f - k->hyst;
    if (now == 1 && x > k->b1 * up) now = 2;
    if (now == 2 && x > k->b2 * up) now = 3;
    if (now == 3 && top == 4 && x > k->b3 * up) now = 4;
    if (now == 4 && x < k->b3 * down) now = 3;
    if (now == 3 && x < k->b2 * down) now = 2;
    if (now == 2 && x < k->b1 * down) now = 1;
    return now;
}

int ctrl_sets(const ctrl_tune_t *k)
{
    return k->b3 > k->b2 ? 4 : 3;
}

ctrl_pid_t ctrl_set_gains(const ctrl_tune_t *k, int set)
{
    if (set >= 2 && set <= 4)
        return k->sets[set - 2];
    ctrl_pid_t g = { k->kp, k->ti, k->kd };
    return g;
}


void ctrl_init(ctrl_t *c)
{
    memset(c, 0, sizeof *c);
    c->sp = 5.0f;   // not shown until a target is set (sp_set)
    c->pv_tau = CTRL_PV_TAU_S;
    c->rev_pct = CTRL_REV_PCT;
}

void ctrl_track(ctrl_t *c, int32_t pos, bool lagged)
{
    // The valve position through the process time constant: the pressure
    // answers this, not the raw count.
    if (CTRL_TLAG_S > 0 && lagged)
        c->pos_f += ((float)pos - c->pos_f) * CTRL_TS_S / (float)(CTRL_TLAG_S + CTRL_TS_S);
    else
        c->pos_f = (float)pos;
}

const char *ctrl_check(const ctrl_t *c, const ctrl_tune_t *k, bool homed, bool fault,
                       bool pv_ok, float pv, float fs)
{
    if (!homed)                     return "Home the valve first";
    if (fault)                      return "Reset the fault first";
    if (!pv_ok)                     return "No valid pressure";
    if (!c->sp_set)                 return "Set a setpoint first";
    if (c->sp < 0 || c->sp > fs)    return "Setpoint outside the sensor range";
    if (pv > fs)                    return "Pressure above the sensor range";
    if (!k->kdir)                   return "Pressure response not set";
    // Set 1 always; sets 2 to 4 when the schedule can pick them.
    for (int s = 1; s <= (k->sched == CTRL_SCHED_OFF ? 1 : ctrl_sets(k)); s++) {
        const ctrl_pid_t g = ctrl_set_gains(k, s);
        if (!(g.kp > 0) || g.ti < 0)
            return "Tuning not set";
    }
    return NULL;
}

void ctrl_start(ctrl_t *c, const ctrl_tune_t *k, uint32_t t_ms, float pv, int32_t now_pos)
{
    float e = (float)k->kdir * (c->sp - pv);
    c->set = ctrl_pick_set(k, 0, fminf(pv, c->sp));   // the lower of the two (0.16.9, ctrl.h)
    const ctrl_pid_t g = ctrl_set_gains(k, c->set);
    c->i = (float)now_pos - steps_of(g.kp, k) * e;   // bumpless: first output is the position in force
    c->pos_cmd = c->last_sent = now_pos;
    c->prev_pv = pv;
    c->win_t = 0;
    c->win_pos = c->pos_f;
    c->win_pv = pv;
    c->noresp = 0;
    c->conv_ms = t_ms;
    c->move = false;
    c->kp_used = steps_of(g.kp, k);
    c->kd_used = steps_of(g.kd, k);
    c->kdir_used = k->kdir;
    c->pv_f = pv;
    c->last_dir = 0;
    c->e_last = c->sp - pv;
    c->sp_used = c->sp;
}

bool ctrl_set_sp(ctrl_t *c, float sp, float fs, uint32_t t_ms)
{
    sp = roundf(sp * 10.0f) / 10.0f;   // targets are 0.1 mbar everywhere
    if (!(sp >= 0 && sp <= fs))
        return false;
    c->sp = sp;
    c->sp_set = true;
    c->conv_ms = t_ms;
    c->in_band = false;
    return true;
}

void ctrl_sent(ctrl_t *c)
{
    c->last_sent = c->move_to;
    c->last_dir = c->move_dir;
    c->move = false;
}

void ctrl_idle(ctrl_t *c)
{
    c->in_band = false;
}

ctrl_result_t ctrl_step(ctrl_t *c, const ctrl_tune_t *k, const ctrl_in_t *in)
{
    const float pv = in->pv, pos_max = (float)k->pos_max;
    c->move = false;

    if (fabsf(c->sp - pv) > k->band)
        c->in_band = false;
    else if (!c->in_band) {
        c->in_band = true;
        c->in_band_ms = in->t_ms;
    }

    // The PI works on the filtered pressure: the reading's second-to-second
    // noise otherwise moves the command tens of steps each way.
    if (c->pv_tau > 0)
        c->pv_f += (pv - c->pv_f) * CTRL_TS_S / (c->pv_tau + CTRL_TS_S);
    else
        c->pv_f = pv;
    const float pf = c->pv_f;
    const float e = (float)k->kdir * (c->sp - pf);
    c->set = ctrl_pick_set(k, c->set, fminf(pf, c->sp));   // the lower of the two (0.16.9, ctrl.h)
    const ctrl_pid_t g = ctrl_set_gains(k, c->set);
    const float kp = steps_of(g.kp, k), ki = g.ti > 0 ? kp / (60.0f * g.ti) : 0, kd = steps_of(g.kd, k);
    const float d = -kd * (float)k->kdir * (pf - c->prev_pv) / CTRL_TS_S;
    c->prev_pv = pf;
    if (kp != c->kp_used || kd != c->kd_used || k->kdir != c->kdir_used) {
        // Tuning or PID set changed in AUTO: keep the output where it was at
        // the last step's error, as on entry, rather than stepping by the
        // change in KP x e (or in D). The last step's error, not this one's,
        // so a setpoint step that also changes the set (the lower of pressure
        // and setpoint picks it) is handled below like one within a set
        // (0.16.13). The last error is kept without KDIR, so a KDIR change
        // stays bumpless.
        c->i = (float)c->pos_cmd - kp * (float)k->kdir * c->e_last - d;
        c->kp_used = kp;
        c->kd_used = kd;
        c->kdir_used = k->kdir;
    }
    // The proportional part acts on the pressure only (0.16.15, Ryan 12 Nov):
    // a setpoint change moves the integral by KP x the change the other way,
    // so the output does not jump with the setpoint. The integral alone then
    // takes the valve to the new setpoint, and the pressure comes in from
    // one side. Before, the valve jumped by KP x the step and gave most of
    // it back as the pressure arrived: a bounce on the way in (simulator,
    // 16 to 12 mbar on set 3, 11 Nov).
    if (c->sp != c->sp_used) {
        if (!c->sp_kick)
            c->i -= kp * (float)k->kdir * (c->sp - c->sp_used);
        c->sp_used = c->sp;
    }
    c->e_last = c->sp - pf;
    float i_new = c->i + ki * e * CTRL_TS_S;
    const float raw = kp * e + i_new + d;
    const float lo = k->outmin / 100.0f * pos_max;   // AUTO min
    bool integrate = true;
    // Saturation anti-windup: hold the integral where the output just
    // reaches the limit, never past it and never back from it. Skipping the
    // step instead froze i at its start value when one step of ki x e alone
    // overshoots (a large error: CTRL SP 500 from 0.8 mbar).
    if (raw > pos_max && e > 0) i_new = fmaxf(c->i, pos_max - kp * e - d);
    if (raw < lo && e < 0) i_new = fminf(c->i, lo - kp * e - d);
    if (in->limit && raw < (float)c->pos_cmd) integrate = false;   // closing blocked by the switch
    // The step constants are full steps; the counts are half steps in HALF.
    const int32_t fs = k->steps > 1 ? k->steps : 1;
    const float lead = (float)(c->pos_cmd - in->pos);   // valve still travelling to the last command
    if (CTRL_LEAD_MAX > 0 && fabsf(lead) > (float)(CTRL_LEAD_MAX * fs) && sgnf(ki * e) == sgnf(lead))
        integrate = false;   // travel anti-windup
    if (integrate)
        c->i = i_new;
    float cmd = clampf(kp * e + c->i + d, lo, pos_max);
    if (in->limit && cmd < (float)c->pos_cmd)
        cmd = (float)c->pos_cmd;
    c->pos_cmd = (int32_t)lroundf(cmd);
    // Deadbands: opening (counts up) and closing; a closing one of 0 is the
    // opening one (initialisers from before 0.15.15).
    const int32_t db_open = k->db > 1 ? k->db : 1;
    const int32_t db_close = k->db_close > 1 ? k->db_close : k->db_close == 1 ? 1 : db_open;
    int32_t step = c->pos_cmd - c->last_sent;
    if (step >= db_open || step <= -db_close) {
        // Reversal hysteresis: turning round for less than this is lost in
        // the drive's backlash and only makes the motor hunt. CTRL_REV_MBAR
        // of error at this set's KP, at most rev_pct (0.16.9, ctrl.h).
        const int32_t to_go = c->pos_cmd - in->pos;
        const int way = to_go > 0 ? 1 : to_go < 0 ? -1 : 0;
        const float rev = fminf(c->rev_pct, CTRL_REV_MBAR * g.kp) / 100.0f * pos_max;
        if (way && !(c->last_dir && way != c->last_dir && fabsf((float)to_go) < rev)) {
            c->move = true;
            c->move_to = c->pos_cmd;
            c->move_dir = way;
        }
    }

    // Response monitor, with the minimum judged move.
    c->win_t += CTRL_TS_S;
    if (c->win_t >= CTRL_TCHECK_S - 1e-6f) {
        const float moved = c->pos_f - c->win_pos, dp = pv - c->win_pv;
        const float err = (float)k->kdir * (c->sp - pv), tol = 0.01f * pos_max;
        const float p = (float)in->pos;
#define AT(x, v) (fabsf((x) - (v)) <= tol)
        // Pinned: the lagged position sat at the limit for the whole window,
        // so the pressure has caught up with the move there.
        const bool pin_open = err > 0 && AT(p, pos_max) && AT(c->pos_f, pos_max) && AT(c->win_pos, pos_max);
        const bool pin_min = err < 0 && AT(p, lo) && AT(c->pos_f, lo) && AT(c->win_pos, lo);
#undef AT
        const int32_t db_max = db_open > db_close ? db_open : db_close;
        const float judge = (float)(db_max > CTRL_CHECKMIN * fs ? db_max : CTRL_CHECKMIN * fs);
        const bool judged = fabsf(moved) >= judge;
        const int way = judged ? sgnf(moved) : !in->moving && pin_open ? 1 : !in->moving && pin_min ? -1 : 0;
        if (way) {
            // A wrong-way change is a response too: no fault for it (Ryan, 22 Oct;
            // on the model a lost helium supply shows as F-6, a stopped pump as W-2).
            if (judged && fabsf(dp) < CTRL_DPMIN) {
                if (++c->noresp >= CTRL_NRESP)
                    return CTRL_F06;
            } else {
                c->noresp = 0;
            }
        } else {
            c->noresp = 0;   // a window not judged resets the count
        }
        c->win_t = 0;
        c->win_pos = c->pos_f;
        c->win_pv = pv;
    }

    // W-6 (pinned at the wrong end for 60 s) removed in 0.15.11 (Ryan 5 Nov):
    // at low pressure the valve rightly sits within 1 % of closed with the
    // pressure under the band for minutes, and it tripped there. A KDIR the
    // wrong way round still ends AUTO by W-2 (CTRL_TCONV_S).

    // W-2 convergence timeout. The setpoint band can be 0 (0.16.16, Ryan
    // 12 Nov): In band and Stable then never show, but W-2 still judges
    // against 0.10 mbar, the old least band, or a steady hold would end AUTO
    // after CTRL_TCONV_S for not reading the setpoint exactly.
    if (fabsf(c->sp - pv) <= fmaxf(k->band, CTRL_TCONV_BAND))
        c->conv_ms = in->t_ms;
    else if (since(in->t_ms, c->conv_ms) > CTRL_TCONV_S * 1000u)
        return CTRL_W02;
    return CTRL_RUN;
}

ctrl_reg_t ctrl_regulation(const ctrl_t *c, const ctrl_tune_t *k, uint32_t t_ms, bool pv_ok, float pv)
{
    if (!pv_ok)
        return CTRL_REG_NONE;
    if (fabsf(c->sp - pv) > k->band)
        return CTRL_REG_REGULATING;
    return c->in_band && since(t_ms, c->in_band_ms) >= CTRL_STABLE_S * 1000u ? CTRL_REG_STABLE : CTRL_REG_BAND;
}
