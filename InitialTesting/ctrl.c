#include <math.h>
#include <string.h>

#include "ctrl.h"

/* --------------------------------------------------------------------------
 * Ported from the simulator v63 (toAuto, setSP, ctrlTick, regulation). The
 * names follow it: I, posCmd, lastSent, posF, winT/winPos/winPV, noresp,
 * wrong/wrongWay, convStart, f02Since, inBandSince.
 * ------------------------------------------------------------------------ */

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static int sgnf(float v) { return v > 0 ? 1 : v < 0 ? -1 : 0; }
static uint32_t since(uint32_t now, uint32_t then) { return now - then; }   // wraps

static float kp_steps(const ctrl_tune_t *k) { return k->kp / 100.0f * (float)k->pos_max; }

void ctrl_init(ctrl_t *c)
{
    memset(c, 0, sizeof *c);
    c->sp = 5.0f;   // not shown until a target is set (sp_set)
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
    if (!c->sp_set)                 return "Set a target first";
    if (c->sp < 0 || c->sp > fs)    return "Target outside the sensor range";
    if (pv > fs)                    return "Pressure above the sensor range";
    if (!k->kdir)                   return "Pressure response not set";
    if (!(k->kp > 0) || k->ti < 0)  return "Tuning not set";
    return NULL;
}

void ctrl_start(ctrl_t *c, const ctrl_tune_t *k, uint32_t t_ms, float pv, int32_t now_pos)
{
    float e = (float)k->kdir * (c->sp - pv);
    c->i = (float)now_pos - kp_steps(k) * e;   // bumpless: first output is the position in force
    c->pos_cmd = c->last_sent = now_pos;
    c->prev_pv = pv;
    c->win_t = 0;
    c->win_pos = c->pos_f;
    c->win_pv = pv;
    c->noresp = c->wrong = 0;
    c->conv_ms = t_ms;
    c->f02_on = false;
    c->move = false;
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

    // F-02, armed in AUTO only.
    if (pv > in->fs) {
        if (!c->f02_on) {
            c->f02_on = true;
            c->f02_ms = in->t_ms;
        }
        if (since(in->t_ms, c->f02_ms) >= CTRL_F02_S * 1000u)
            return CTRL_F02;
    } else {
        c->f02_on = false;
    }

    const float e = (float)k->kdir * (c->sp - pv);
    const float kp = kp_steps(k), ki = k->ti > 0 ? kp / (60.0f * k->ti) : 0;
    const float d = -(CTRL_KD / 100.0f * pos_max) * (float)k->kdir * (pv - c->prev_pv) / CTRL_TS_S;
    c->prev_pv = pv;
    const float i_new = c->i + ki * e * CTRL_TS_S;
    const float raw = kp * e + i_new + d;
    const float lo = k->outmin / 100.0f * pos_max;   // Auto min
    bool integrate = true;
    if (raw > pos_max && e > 0) integrate = false;
    if (raw < lo && e < 0) integrate = false;
    if (in->limit && raw < (float)c->pos_cmd) integrate = false;   // closing blocked by the switch
    const float lead = (float)(c->pos_cmd - in->pos);   // valve still travelling to the last command
    if (CTRL_LEAD_MAX > 0 && fabsf(lead) > CTRL_LEAD_MAX && sgnf(ki * e) == sgnf(lead))
        integrate = false;   // travel anti-windup
    if (integrate)
        c->i = i_new;
    float cmd = clampf(kp * e + c->i + d, lo, pos_max);
    if (in->limit && cmd < (float)c->pos_cmd)
        cmd = (float)c->pos_cmd;
    c->pos_cmd = (int32_t)lroundf(cmd);
    int32_t db = k->db > 1 ? k->db : 1;
    int32_t step = c->pos_cmd - c->last_sent;
    if (step >= db || step <= -db) {
        c->move = true;
        c->move_to = c->pos_cmd;
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
        const float judge = (float)(k->db > CTRL_CHECKMIN ? k->db : CTRL_CHECKMIN);
        const bool judged = fabsf(moved) >= judge;
        const int way = judged ? sgnf(moved) : !in->moving && pin_open ? 1 : !in->moving && pin_min ? -1 : 0;
        if (way) {
            const int exp = way * k->kdir;
            if (dp * (float)exp < -CTRL_DPMIN) {
                c->wrong = (c->wrong_way == way ? c->wrong : 0) + 1;
                c->wrong_way = way;
                c->noresp = 0;
                if (c->wrong >= 2)
                    return way > 0 ? CTRL_F16 : CTRL_F17;
            } else if (judged && fabsf(dp) < CTRL_DPMIN) {
                c->noresp++;
                c->wrong = 0;
                if (c->noresp >= CTRL_NRESP)
                    return CTRL_F10;
            } else {
                c->noresp = 0;
                c->wrong = 0;
            }
        } else {
            c->noresp = 0;   // a window not judged resets both counts
            c->wrong = 0;
        }
        c->win_t = 0;
        c->win_pos = c->pos_f;
        c->win_pv = pv;
    }

    // W-02 convergence timeout.
    if (fabsf(c->sp - pv) <= k->band)
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
