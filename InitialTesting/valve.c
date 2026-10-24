#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"   // lv_tick_get, lv_snprintf
#include "link.h"
#include "rtclock.h"
#include "limit.h"
#include "stepper3.h"
#include "supply.h"
#include "fixed.h"
#include "cfg.h"
#include "ctrl.h"
#include "valve.h"

/* --------------------------------------------------------------------------
 * Settings (CFG SAVE keeps them, cfg.c). Defaults and ranges from the simulator
 * (DEF_P / RANGE), except POS_MAX: 4 turns of the 26M motor through the PK22
 * 344:1 gearbox, 66,048 full steps (Ryan, 19 Oct; was 6 turns).
 * ------------------------------------------------------------------------ */
typedef struct {
    const char *name;
    int32_t *v;
    int32_t lo, hi;
} setting_t;

static int32_t pos_max  = 66048;   // steps, 0 % to 100 %
static int32_t f_home   = 400;     // steps/s, clearance, fast close, back-off
static int32_t f_creep  = 25;      // steps/s, slow final close
static int32_t n_backoff = 400;    // steps off the switch before the creep
static int32_t n_clear  = 800;     // steps off the switch if pressed at the start
static int32_t n_offset = 0;       // count given to the switch point
static valve_ref_t ref_how;        // how the position was last set (valve_reference)

static const setting_t settings[] = {
    { "POSMAX",  &pos_max,   1000, 300000 },
    { "FHOME",   &f_home,    16,   1000 },
    { "FCREEP",  &f_creep,   16,   1000 },
    { "BACKOFF", &n_backoff, 10,   5000 },
    { "CLEAR",   &n_clear,   10,   10000 },
    { "OFFSET",  &n_offset,  0,    5000 },
};
#define N_SETTINGS (sizeof settings / sizeof settings[0])
_Static_assert(N_SETTINGS == VALVE_N_SETTINGS, "valve.h VALVE_N_SETTINGS");

/* The settings above are in full steps (steps/s for the speeds), whatever
 * the step mode; the motor counts half steps in HALF (stepper3_scale()).
 * These give them in motor counts, so a mode change keeps the homing
 * (Ryan, 23 Oct: "just recalculate"). */
static int32_t sc(int32_t full) { return full * stepper3_scale(); }
static int32_t pm(void) { return sc(pos_max); }
static int32_t f_rate(int32_t full)
{
    int32_t r = sc(full);
    return r > STEPPER3_RATE_MAX ? STEPPER3_RATE_MAX : r;
}

#define SETTLE_MS     150     // after a move, before the switch is read
#define CREEP_EXTRA   200     // creep goes this far past the back-off
#define EXC_MIN_MV    9500    // F-02 window
#define EXC_MAX_MV    10500

/* --------------------------------------------------------------------------
 * Event log: 100 in RAM, each also sent as an EVT line (200 to 0.11.17;
 * the 10 KB went to the draw buffer, display_lvgl.c).
 * ------------------------------------------------------------------------ */
#define EVT_N 100
typedef struct {
    uint32_t t_ms;
    char tag[6];
    char text[90];
} event_t;

static event_t events[EVT_N];
static uint32_t evt_count;     // total logged since boot

#define EVT_LINE 120   // link room wanted before an EVT? line goes out
static struct {
    bool active;
    uint32_t next, end, since;
} edump;
static void evt_dump_task(void);

static void evt_print(const event_t *e)
{
    link_printf("EVT %lu.%lu %s %s\r\n", (unsigned long)(e->t_ms / 1000),
                (unsigned long)(e->t_ms % 1000 / 100), e->tag, e->text);
}

void valve_event(const char *tag, const char *fmt, ...)
{
    event_t *e = &events[evt_count % EVT_N];
    va_list args;
    e->t_ms = lv_tick_get();
    lv_strlcpy(e->tag, tag, sizeof e->tag);
    va_start(args, fmt);
    lv_vsnprintf(e->text, sizeof e->text, fmt, args);
    va_end(args);
    evt_count++;
    evt_print(e);
}
#define ev valve_event

/* --------------------------------------------------------------------------
 * Faults, in the simulator's FAULT_ORDER (highest priority first).
 * ------------------------------------------------------------------------ */
typedef enum { F01, F02, F03, F04, F05, F06, F_N } fault_t;   // priority order (Ryan, 22 Oct)

static const char *const fault_tag[F_N]   = { "F-01", "F-02", "F-03", "F-04", "F-05", "F-06" };
static const char *const fault_short[F_N] = {
    "No pressure signal", "Excitation out of window", "Limit not found while calibrating",
    "Limit still active after clearance", "Motion timeout", "No pressure response" };

static bool latched[F_N];
static uint32_t latched_ms[F_N];    // when each latched
static valve_mode_t latched_mode[F_N];
static bool cause_f01, cause_f02;   // from the last sensor read
static bool sensed;                 // at least one sensor read since boot

/* --------------------------------------------------------------------------
 * Mode, calibration, limit question
 * ------------------------------------------------------------------------ */
typedef enum { CAL_NONE, CAL_CLEAR, CAL_FAST, CAL_BACK, CAL_CREEP } cal_stage_t;

static valve_mode_t mode = VALVE_NOT_CAL;
static bool calibrated;
static bool cal_was;     // calibrated before this calibration started
static cal_stage_t cal_stage;
static bool cal_moving;           // a calibration move is running
static uint32_t cal_settle_from;  // when the wait started
static bool cal_settling;         // waiting SETTLE_MS before the next stage
static uint32_t cal_t0;
static uint32_t seq_seen;         // stepper3_move_seq() last handled

static bool question;             // switch-pressed question waiting
static int32_t question_at;       // count where the switch was pressed

static uint32_t cal_done_ms, cal_took_s;   // last good calibration
static bool cal_done;
static char cal_done_iso[20];              // its wall time, "" if the clock is not set
static int32_t switch_at;         // count where the switch last opened
static bool switch_was_open;

static char status_text[160];

// AUTO (ctrl.c): the loop's state and the last reading it used.
static ctrl_t ctl;
static bool pv_ok;
static float pv_mbar;

static int32_t pct10_of(int32_t pos)
{
    const int32_t m = pm();
    return (int32_t)(((int64_t)pos * 1000 + (pos >= 0 ? m / 2 : -m / 2)) / m);
}

static int32_t milli_of(int32_t pos);

static const char *pct_text(char *buf, size_t n, int32_t pos)
{
    return fixed_fmt(buf, n, pct10_of(pos), 1);
}

// The pressure faults (F-01 no Druck, F-02 excitation) do not block the motor: manual moves and calibrating use the step
// count and the limit switch, not the pressure (Ryan 16 Oct). In AUTO they
// stop the valve and drop to MANUAL like any other fault.
static bool sensor_fault(fault_t f) { return f == F01 || f == F02; }

static bool is_auto(void) { return mode == VALVE_AUTO; }

static bool w02;   // W-02 shown until OK or the next Start AUTO

static bool any_latched(void)
{
    for (int i = 0; i < F_N; i++)
        if (latched[i] && !sensor_fault((fault_t)i))
            return true;
    return false;
}

static void update_lock(void)
{
    stepper3_lock(mode == VALVE_CALIBRATING ? "calibrating, CTRL STOP to cancel"
                  : any_latched() ? "fault latched, CTRL RESET first"
                  : is_auto() ? "in AUTO, CTRL MAN first" : NULL);
}

static void cal_end(valve_mode_t m)
{
    mode = m;
    cal_stage = CAL_NONE;
    cal_moving = false;
    cal_settling = false;
    update_lock();
}

static void cal_abort(const char *why)
{
    if (mode != VALVE_CALIBRATING)
        return;
    stepper3_stop();
    seq_seen = stepper3_move_seq();
    // An operator STOP keeps the earlier calibration: the step count kept
    // counting through the cancelled moves, so the position is still known.
    bool keep = cal_was && !strcmp(why, "STOP");
    ev("REF", "Homing aborted (%s)%s", why, keep ? ", earlier homing kept" : "");
    calibrated = keep;
    cal_end(keep ? VALVE_MANUAL : VALVE_NOT_CAL);
}

static void raise(fault_t f, bool uncalibrate, const char *fmt, ...)
{
    char text[90];
    va_list args;
    if (latched[f])
        return;
    va_start(args, fmt);
    lv_vsnprintf(text, sizeof text, fmt, args);
    va_end(args);
    latched[f] = true;
    latched_ms[f] = lv_tick_get();
    latched_mode[f] = mode;
    ev(fault_tag[f], "%s", text);
    if (is_auto()) {
        // Any fault in AUTO: stop, MANUAL holds where the valve stopped.
        stepper3_stop();
        mode = VALVE_MANUAL;
        char p[16];
        ev("MODE", "Manual after %s, valve idle at %s %%", fault_tag[f],
           pct_text(p, sizeof p, stepper3_position()));
    } else if (!sensor_fault(f)) {
        stepper3_stop();
        cal_abort(fault_tag[f]);
    }
    if (uncalibrate) {
        calibrated = false;
        mode = VALVE_NOT_CAL;
    }
    update_lock();
}

static bool cal_move(int32_t to, int32_t rate)
{
    cal_moving = stepper3_goto_rate(to, (uint32_t)rate);
    seq_seen = stepper3_move_seq();
    return cal_moving;
}

static void cal_fast(void)
{
    int32_t n = (int32_t)(((int64_t)pm() * 11 + 5) / 10);
    cal_stage = CAL_FAST;
    if (!cal_move(stepper3_position() - n, f_rate(f_home)))
        raise(F03, true, "Homing: closing move refused at count %ld (%s)", (long)stepper3_position(),
              stepper3_refusal());
}

static void cal_creep(void)
{
    cal_stage = CAL_CREEP;
    if (!cal_move(stepper3_position() - sc(n_backoff + CREEP_EXTRA), f_rate(f_creep)))
        raise(F03, true, "Homing: creep move refused at count %ld", (long)stepper3_position());
}

static bool cal_start(const char **err)
{
    if (mode == VALVE_CALIBRATING) { *err = "already calibrating"; return false; }
    if (is_auto())                 { *err = "Switch to MANUAL first"; return false; }
    if (any_latched())             { *err = "fault latched, CTRL RESET first"; return false; }
    stepper3_stop();
    stepper3_override_cancel();
    question = false;
    cal_settling = false;
    cal_was = calibrated;
    calibrated = false;
    mode = VALVE_CALIBRATING;
    cal_t0 = lv_tick_get();
    update_lock();
    ev("REF", "Homing started at count %ld", (long)stepper3_position());
    if (limit_open_now()) {
        cal_stage = CAL_CLEAR;
        cal_move(stepper3_position() + sc(n_clear), f_rate(f_home));
    } else {
        cal_fast();
    }
    return true;
}

// A calibration move has ended.
static void cal_move_ended(stepper3_stop_t why)
{
    cal_moving = false;
    switch (cal_stage) {
    case CAL_CLEAR:
    case CAL_BACK:
        if (why != STEPPER3_STOP_DONE)
            cal_abort(stepper3_stop_name(why));
        else {
            cal_settle_from = lv_tick_get();   // then check the switch
            cal_settling = true;
        }
        break;
    case CAL_FAST:
        if (why == STEPPER3_STOP_LIMIT) {
            cal_stage = CAL_BACK;
            cal_move(stepper3_position() + sc(n_backoff), f_rate(f_home));
        } else if (why == STEPPER3_STOP_DONE) {
            raise(F03, true, "Homing: switch not found within %ld steps",
                  (long)(((int64_t)pm() * 11 + 5) / 10));
        } else {
            cal_abort(stepper3_stop_name(why));
        }
        break;
    case CAL_CREEP:
        if (why == STEPPER3_STOP_LIMIT) {
            if (!stepper3_set_position(sc(n_offset))) {
                cal_abort("count not set");
                break;
            }
            calibrated = true;
            ref_how = VALVE_REF_HOMED;
            cal_done_ms = lv_tick_get();
            cal_done = true;
            cal_took_s = lv_tick_elaps(cal_t0) / 1000;
            lv_strlcpy(cal_done_iso, rtclock_iso(), sizeof cal_done_iso);
            ev("REF", "Homed in %lu s, position %ld", (unsigned long)cal_took_s, (long)sc(n_offset));
            cal_end(VALVE_MANUAL);
        } else if (why == STEPPER3_STOP_DONE) {
            raise(F03, true, "Homing: switch not found on the creep close");
        } else {
            cal_abort(stepper3_stop_name(why));
        }
        break;
    default:
        break;
    }
}

static void cal_settled(void)
{
    cal_settling = false;
    int32_t moved = sc(cal_stage == CAL_CLEAR ? n_clear : n_backoff);
    if (limit_open_now()) {
        raise(F04, true, "Switch still active after %ld-step %s", (long)moved,
              cal_stage == CAL_CLEAR ? "clearance move" : "back-off");
    } else if (cal_stage == CAL_CLEAR) {
        cal_fast();
    } else {
        cal_creep();
    }
}

static void limit_hit(void)
{
    if (!calibrated || question)
        return;
    question_at = stepper3_position();
    question = true;
    char p[16];
    ev("W-01", "Limit switch pressed at count %ld (%s %%): Set as 0 %% or Skip",
       (long)question_at, pct_text(p, sizeof p, question_at));
}

/* --------------------------------------------------------------------------
 * API
 * ------------------------------------------------------------------------ */
void valve_init(void)
{
    ctrl_init(&ctl);
    seq_seen = stepper3_move_seq();
    stepper3_on_reference_change(valve_uncalibrate);
    ev("BOOT", "Power-up (event only), valve not homed");
    update_lock();
}

/* Motor supply (supply.h). Lost during a move: the count ran on without
 * the valve, so the move stops and the homing is cleared. Off in AUTO,
 * idle: MANUAL (AUTO could not move it); the homing stays. */
static void supply_check(void)
{
    static bool was_off;
    bool off = supply_off();
    if (off && !was_off) {
        bool moving = stepper3_busy() || mode == VALVE_CALIBRATING;
        if (stepper3_busy())
            stepper3_stop();
        ev("SUP", "Motor supply off (%ld mV)%s", (long)supply_mv(), moving ? " during a move" : "");
        if (moving && (calibrated || mode == VALVE_CALIBRATING))
            valve_uncalibrate("motor supply lost during a move");
        if (is_auto()) {
            mode = VALVE_MANUAL;
            update_lock();
            ev("MODE", "Manual: motor supply off");
        }
    } else if (!off && was_off) {
        ev("SUP", "Motor supply on (%ld mV)", (long)supply_mv());
    }
    was_off = off;
}

void valve_task(void)
{
    evt_dump_task();
    uint32_t seq = stepper3_move_seq();
    if (seq != seq_seen) {
        seq_seen = seq;
        stepper3_stop_t why = stepper3_last_stop();
        if (why == STEPPER3_STOP_TIMEOUT)
            raise(F05, false, "Motion timeout at count %ld", (long)stepper3_position());
        else if (mode == VALVE_CALIBRATING && cal_moving)
            cal_move_ended(why);
        else if (why == STEPPER3_STOP_LIMIT)
            limit_hit();
    }
    if (mode == VALVE_CALIBRATING && cal_settling && lv_tick_elaps(cal_settle_from) >= SETTLE_MS)
        cal_settled();
    supply_check();
    bool open = limit_open_now();
    if (open && !switch_was_open)
        switch_at = stepper3_position();
    switch_was_open = open;
}

/* --------------------------------------------------------------------------
 * AUTO (stage 8): ctrl.c works out the command, this runs it.
 * ------------------------------------------------------------------------ */
// The tuning from cfg.c (CFG TUNE units: 0.01 %/mbar, 0.01 min, 0.1 %,
// steps, +-1, 0.01 mbar).
static ctrl_tune_t tune_now(void)
{
    ctrl_tune_t k;
    k.kp = (float)cfg_tune(CFG_KP) / 100.0f;
    k.ti = (float)cfg_tune(CFG_TI) / 100.0f;
    k.outmin = (float)cfg_tune(CFG_OUTMIN) / 10.0f;
    k.db = cfg_tune(CFG_DB);
    k.kdir = (int)cfg_tune(CFG_KDIR);
    k.band = (float)cfg_tune(CFG_BAND) / 100.0f;
    k.pos_max = pm();
    return k;
}

int32_t valve_range_mmbar(void)
{
    cfg_sensor_t sn;
    if (cfg_sensor(cfg_sensor_active(), &sn) && sn.fs_mmbar > 0)
        return sn.fs_mmbar;
    return cfg_psi10_mmbar(150);
}

static float range_mbar(void) { return (float)valve_range_mmbar() / 1000.0f; }

static const char *mbar_text(char *buf, size_t n, float mbar, int dec)
{
    int64_t scale = dec == 2 ? 100 : 10;
    return fixed_fmt(buf, n, (int64_t)(mbar * (float)scale + (mbar >= 0 ? 0.5f : -0.5f)), dec);
}

// Once a second, after the faults of this read.
static void auto_step(void)
{
    ctrl_track(&ctl, stepper3_position(), calibrated && mode != VALVE_CALIBRATING);
    if (mode != VALVE_AUTO || !pv_ok) {
        ctrl_idle(&ctl);
        return;
    }
    ctrl_tune_t k = tune_now();
    ctrl_in_t in = { lv_tick_get(), pv_mbar, stepper3_position(), stepper3_busy(), limit_open_now(), range_mbar() };
    switch (ctrl_step(&ctl, &k, &in)) {
    case CTRL_RUN:
        // The closed switch blocks closing from rest (stepper3 refuses it);
        // ctrl.c holds the integral meanwhile.
        if (ctl.move && stepper3_goto(ctl.move_to))
            ctrl_sent(&ctl);
        break;
    case CTRL_W02:
        stepper3_stop();
        mode = VALVE_MANUAL;
        w02 = true;
        ev("W-02", "Convergence timeout after %d s, AUTO ended, MANUAL, valve idle", CTRL_TCONV_S);
        update_lock();
        break;
    case CTRL_F06:
        raise(F06, false, "No pressure response in %d windows: drive slipping, motor unplugged or needle seized",
              CTRL_NRESP);
        break;
    }
}

bool valve_auto(void) { return is_auto(); }
bool valve_w02(void) { return w02; }
void valve_w02_clear(void) { w02 = false; }

const char *valve_auto_line(void)
{
    static char line[128];
    static const char *const reg[] = { "no reading", "Regulating", "In band", "Stable" };
    char p[16], sp[16], v[16], to[16];
    mbar_text(p, sizeof p, pv_mbar, 2);
    mbar_text(sp, sizeof sp, ctl.sp, 1);
    fixed_fmt(v, sizeof v, fixed_round(milli_of(stepper3_position()), 1), 2);
    fixed_fmt(to, sizeof to, fixed_round(milli_of(stepper3_target()), 1), 2);
    lv_snprintf(line, sizeof line, "%s %s mB, target %s, valve %s %%%s%s%s, %s", valve_mode_name(),
                pv_ok ? p : "----", sp, v, stepper3_busy() ? " to " : "", stepper3_busy() ? to : "",
                stepper3_busy() ? " %" : "",
                reg[valve_regulation()]);
    return line;
}

bool valve_start_auto(const char **why)
{
    if (mode == VALVE_AUTO) return true;
    if (mode == VALVE_CALIBRATING) { *why = "Homing: STOP to cancel"; return false; }
    ctrl_tune_t k = tune_now();
    *why = ctrl_check(&ctl, &k, calibrated, valve_fault_count() > 0, pv_ok, pv_mbar, range_mbar());
    if (!*why && supply_off())
        *why = "Motor supply off";
    if (*why)
        return false;
    // Bumpless from where the valve is going (a manual move still running
    // carries on to its end under AUTO).
    int32_t now = stepper3_target();
    ctrl_start(&ctl, &k, lv_tick_get(), pv_mbar, now);
    mode = VALVE_AUTO;
    w02 = false;
    update_lock();
    char a[16], p[16];
    ev("MODE", "%s at target %s mB, valve %s %%", "Auto",
       mbar_text(a, sizeof a, ctl.sp, 1), pct_text(p, sizeof p, now));
    return true;
}

bool valve_to_manual(const char **why)
{
    if (mode == VALVE_MANUAL) return true;
    if (!is_auto()) { *why = "Not in AUTO"; return false; }
    // A move under way ramps down at the normal deceleration; MANUAL then
    // holds where it stops.
    if (stepper3_busy())
        stepper3_stop_ramped();
    mode = VALVE_MANUAL;
    update_lock();
    char p[16];
    ev("MODE", "Manual, valve stops at its present position (%s %%)", pct_text(p, sizeof p, stepper3_position()));
    return true;
}

bool valve_set_target(int32_t mmbar, const char **why)
{
    int32_t fs = valve_range_mmbar();
    char a[16], b[16];
    if (mmbar < 0 || mmbar > fs) {
        ev("TARGET", "Setpoint %s rejected, allowed 0 to %s mB", fixed_fmt(a, sizeof a, fixed_round(mmbar, 2), 1),
           fixed_fmt(b, sizeof b, fixed_round(fs, 2), 1));
        *why = "Target outside the sensor range";
        return false;
    }
    int32_t v = (int32_t)fixed_round(mmbar, 2);   // 0.1 mbar
    ctrl_set_sp(&ctl, (float)v / 10.0f, range_mbar(), lv_tick_get());
    ev("TARGET", "Set to %s mB", fixed_fmt(a, sizeof a, v, 1));
    return true;
}

bool valve_target(int32_t *mmbar)
{
    if (mmbar)
        *mmbar = (int32_t)lroundf(ctl.sp * 10.0f) * 100;
    return ctl.sp_set;
}

int32_t valve_band_mmbar(void) { return cfg_tune(CFG_BAND) * 10; }

ctrl_reg_t valve_regulation(void)
{
    if (mode != VALVE_AUTO)
        return CTRL_REG_NONE;
    ctrl_tune_t k = tune_now();
    return ctrl_regulation(&ctl, &k, lv_tick_get(), pv_ok, pv_mbar);
}

void valve_sense(const clicks_state_t *s)
{
    bool exc_bad = s->pm_found && (s->boost_tripped || s->bus_mv < EXC_MIN_MV || s->bus_mv > EXC_MAX_MV);
    cause_f02 = exc_bad;
    cause_f01 = !s->pm_found || (!exc_bad && !s->reading_ok);
    sensed = true;
    if (cause_f01)
        raise(F01, false, s->pm_found ? "No pressure signal (%s)" : "No pressure signal (Power Monitor not answering)%s",
              s->pm_found ? (s->druck_absent ? "outside 0.005-11 mV/V" : "no valid reading") : "");
    if (cause_f02) {
        if (s->boost_tripped)
            raise(F02, false, "Excitation supply tripped at %ld mV", (long)s->boost_trip_mv);
        else
            raise(F02, false, "Excitation %ld mV, outside 9500-10500 mV", (long)s->bus_mv);
    }
    pv_ok = s->pm_found && s->reading_ok && !s->boost_tripped && !cause_f01 && !cause_f02;
    pv_mbar = (float)s->p_mmbar / 1000.0f;
    auto_step();
}

void valve_uncalibrate(const char *why)
{
    if (mode == VALVE_CALIBRATING) {
        cal_abort(why);
        cal_was = false;
    }
    if (!calibrated)
        return;
    if (is_auto())
        stepper3_stop();
    calibrated = false;
    question = false;
    mode = VALVE_NOT_CAL;
    ev("REF", "Homing cleared (%s)", why);
    update_lock();
}

bool valve_restore(int32_t count, const char **why)
{
    if (mode == VALVE_CALIBRATING) { *why = "calibrating"; return false; }
    if (is_auto())                  { *why = "Switch to MANUAL first"; return false; }
    if (calibrated)                 { *why = "already calibrated"; return false; }
    if (any_latched())              { *why = "fault latched, CTRL RESET first"; return false; }
    if (stepper3_busy() || !stepper3_set_position(count)) { *why = "motor busy"; return false; }
    calibrated = true;
    ref_how = VALVE_REF_SAVED;
    mode = VALVE_MANUAL;
    char p[16];
    ev("REF", "Last saved position restored: count %ld (%s %%), not verified by homing",
       (long)count, pct_text(p, sizeof p, count));
    update_lock();
    return true;
}

bool valve_set_closed(const char **why)
{
    return valve_set_position(0, why);
}

bool valve_set_position(int32_t pct100, const char **why)
{
    if (pct100 < 0 || pct100 > 10000) { *why = "outside 0 to 100 %"; return false; }
    if (mode == VALVE_CALIBRATING) { *why = "homing, STOP to cancel"; return false; }
    if (is_auto())                  { *why = "Switch to MANUAL first"; return false; }
    if (any_latched())              { *why = "fault latched, CTRL RESET first"; return false; }
    int32_t c = (int32_t)(((int64_t)pct100 * pm() + 5000) / 10000);
    if (stepper3_busy() || !stepper3_set_position(c)) { *why = "moving, wait for it to stop"; return false; }
    calibrated = true;
    ref_how = VALVE_REF_HAND;
    mode = VALVE_MANUAL;
    if (c == 0)
        ev("REF", "Set as closed by the operator: count 0 (0 %%), not verified by homing");
    else
        ev("REF", "Position set by the operator: count %ld (%ld.%02ld %%), not verified by homing",
           (long)c, (long)(pct100 / 100), (long)(pct100 % 100));
    update_lock();
    return true;
}

valve_mode_t valve_mode(void)   { return mode; }
bool valve_calibrated(void)     { return calibrated; }
valve_ref_t valve_reference(void) { return calibrated ? ref_how : VALVE_REF_NONE; }
int32_t valve_pos_max(void)     { return pm(); }   // in motor counts
int32_t valve_pct10(void)       { return pct10_of(stepper3_position()); }
bool valve_fault_latched(void)  { return any_latched(); }
bool valve_motion_blocked(void) { return any_latched(); }

const char *valve_mode_name(void)
{
    switch (mode) {
    case VALVE_CALIBRATING: return "HOMING";
    case VALVE_MANUAL:      return "MANUAL";
    case VALVE_AUTO:        return "AUTO";
    default:                return "NOT HOMED";
    }
}

static const char *cal_stage_name(void)
{
    static const char *const names[] = { "", "moving off the switch", "1 of 3 closing to the switch",
                                         "2 of 3 backing off", "3 of 3 slow close" };
    return names[cal_stage];
}

static void faults_text(char *buf, size_t n)
{
    buf[0] = '\0';
    for (int i = 0; i < F_N; i++) {
        if (!latched[i])
            continue;
        size_t len = strlen(buf);
        lv_snprintf(buf + len, n - len, "%s%s", len ? " " : "", fault_tag[i]);
    }
}

const char *valve_status(void)
{
    char p[16], f[40];
    faults_text(f, sizeof f);
    if (is_auto()) {
        // No position here: it changes every second in AUTO, and the AUTO
        // line (valve_auto_line) carries it; this one prints on a change of state.
        char a[16];
        lv_snprintf(status_text, sizeof status_text, "Valve: %s at %s mB%s%s", valve_mode_name(),
                    mbar_text(a, sizeof a, ctl.sp, 1), f[0] ? ", faults " : "", f);
    } else if (calibrated)
        lv_snprintf(status_text, sizeof status_text, "Valve: %s, calibrated, %s %% (count %ld of %ld)%s%s%s",
                    valve_mode_name(), pct_text(p, sizeof p, stepper3_position()),
                    (long)stepper3_position(), (long)pm(), f[0] ? ", faults " : "", f,
                    question ? ", switch question waiting" : "");
    else
        lv_snprintf(status_text, sizeof status_text, "Valve: %s%s%s, count %ld%s%s",
                    valve_mode_name(), cal_stage ? ", " : "", cal_stage_name(),
                    (long)stepper3_position(), f[0] ? ", faults " : "", f);
    return status_text;
}

/* --------------------------------------------------------------------------
 * Commands
 * ------------------------------------------------------------------------ */
static bool can_reset(fault_t f)
{
    if (f == F01) return sensed && !cause_f01;
    if (f == F02) return sensed && !cause_f02;
    return true;
}

bool valve_stop_latches(void)
{
    return is_auto();   // STOP ends AUTO (nothing latched since 0.11.20); outside, a plain stop
}

void valve_stop(void)
{
    bool was_cal = mode == VALVE_CALIBRATING;
    // STOP also disarms a limit override, armed or in use.
    if (stepper3_override()) {
        stepper3_override_cancel();
        ev("LIM", "Limit override off (STOP)");
    }
    // An operating stop: stop and hold, nothing latched. In AUTO it also
    // drops to MANUAL (no fault since 0.11.20, Ryan 22 Oct).
    bool was_auto = is_auto();
    stepper3_stop();
    seq_seen = stepper3_move_seq();
    cal_abort("STOP");
    if (was_auto) {
        mode = VALVE_MANUAL;
        update_lock();
    }
    ev("STOP", "Operator stop at count %ld%s", (long)stepper3_position(),
       was_cal ? ", calibrating cancelled" : was_auto ? ", AUTO ended, MANUAL" : "");
}

static void stop_command(void)
{
    valve_stop();
    link_printf("OK stopped at %ld\r\n", (long)stepper3_position());
}

int valve_reset(char *left, size_t n)
{
    int any = 0;
    if (n)
        left[0] = '\0';
    for (int i = 0; i < F_N; i++) {
        if (!latched[i])
            continue;
        any = 1;
        if (can_reset((fault_t)i)) {
            latched[i] = false;
            ev(fault_tag[i], "Reset by operator");
        } else if (n) {
            size_t len = strlen(left);
            lv_snprintf(left + len, n - len, "%s%s", len ? " " : "", fault_tag[i]);
        }
    }
    update_lock();
    return any;
}

static void reset_command(void)
{
    char left[40];
    bool any = valve_reset(left, sizeof left);
    bool kept = left[0] != '\0';
    if (!any)
        link_printf("OK no fault to reset\r\n");
    else if (kept)
        link_printf("ERR not reset, cause still present: %s\r\n", left);
    else
        link_printf("OK reset%s\r\n", calibrated ? "" : ", valve not calibrated");
}

static void faults_command(void)
{
    bool any = false;
    for (int i = 0; i < F_N; i++) {
        if (!latched[i])
            continue;
        any = true;
        link_printf("CTRL %s %s, %s\r\n", fault_tag[i], fault_short[i],
                    can_reset((fault_t)i) ? "can reset" : "cause still present");
    }
    if (!any)
        link_printf("CTRL no faults\r\n");
    link_printf("OK\r\n");
}

static void events_command(const char *a)
{
    long n = 20;
    while (*a == ' ')
        a++;
    if (*a) {
        char *end;
        n = strtol(a, &end, 10);
        if (end == a || *end || n < 1 || n > EVT_N) {
            link_printf("ERR expected: EVT? [1-%d]\r\n", EVT_N);
            return;
        }
    }
    uint32_t have = evt_count < EVT_N ? evt_count : EVT_N;
    if ((uint32_t)n > have)
        n = (long)have;
    // Sent from valve_task() a line at a time, as the link has room, so a
    // slow reader does not hold up the main loop (as TREND DUMP).
    edump.next = evt_count - (uint32_t)n;
    edump.end = evt_count;
    edump.since = lv_tick_get();
    edump.active = true;
}

static void evt_dump_task(void)
{
    if (!edump.active)
        return;
    for (int lines = 0; lines < 8; lines++) {      // a few per loop pass
        if (link_tx_room() < EVT_LINE) {
            if (lv_tick_elaps(edump.since) >= 2000)
                edump.active = false;               // nobody reading: drop the rest
            return;
        }
        edump.since = lv_tick_get();
        if (edump.next >= edump.end) {
            link_printf("OK %lu events since boot\r\n", (unsigned long)evt_count);
            edump.active = false;
            return;
        }
        uint32_t i = edump.next++;
        if (evt_count - i <= EVT_N)                 // not yet written over
            evt_print(&events[i % EVT_N]);
    }
}

// "12.3" -> 123 (0.1 units), 0 to 1000.
static bool parse_pct10(const char *s, int32_t *out)
{
    long whole = 0, frac = 0;
    int nd = 0;
    bool digits = false;
    while (*s == ' ')
        s++;
    while (*s >= '0' && *s <= '9' && whole <= 1000) {
        whole = whole * 10 + (*s++ - '0');
        digits = true;
    }
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') {
            if (nd++ == 0)
                frac = *s - '0';
            s++;
            digits = true;
        }
    }
    while (*s == ' ')
        s++;
    if (!digits || *s || whole * 10 + frac > 1000)
        return false;
    *out = (int32_t)(whole * 10 + frac);
    return true;
}

static void show(void)
{
    char p[16], f[40];
    faults_text(f, sizeof f);
    link_printf("VALVE mode %s%s%s, calibrated %d, count %ld (%s %%), target %ld\r\n",
                valve_mode_name(), cal_stage ? ", " : "", cal_stage_name(), calibrated ? 1 : 0,
                (long)stepper3_position(), pct_text(p, sizeof p, stepper3_position()),
                (long)stepper3_target());
    link_printf("VALVE POSMAX %ld, FHOME %ld, FCREEP %ld, BACKOFF %ld, CLEAR %ld, OFFSET %ld\r\n",
                (long)pos_max, (long)f_home, (long)f_creep, (long)n_backoff, (long)n_clear, (long)n_offset);
    link_printf("VALVE faults %s, limit %s\r\n", f[0] ? f : "none", limit_text());
    if (ctl.sp_set) {
        static const char *const reg[] = { "", ", regulating", ", in band", ", stable" };
        char a[16], b[16];
        link_printf("VALVE target %s mB, band %s mB%s\r\n", mbar_text(a, sizeof a, ctl.sp, 1),
                    fixed_fmt(b, sizeof b, cfg_tune(CFG_BAND), 2), reg[valve_regulation()]);
    } else {
        link_printf("VALVE target not set (CTRL SP)\r\n");
    }
    if (question)
        link_printf("VALVE question: limit switch pressed at count %ld (%s %%), VALVE SETZERO or VALVE SKIP\r\n",
                    (long)question_at, pct_text(p, sizeof p, question_at));
    link_printf("OK\r\n");
}

static void setting_command(const char *a)
{
    for (size_t i = 0; i < N_SETTINGS; i++) {
        size_t len = strlen(settings[i].name);
        if (strncmp(a, settings[i].name, len) != 0 || (a[len] != ' ' && a[len] != '\0'))
            continue;
        char *end;
        long v = strtol(a + len, &end, 10);
        while (*end == ' ')
            end++;
        if (end == a + len || *end || v < settings[i].lo || v > settings[i].hi)
            link_printf("ERR expected: VALVE %s <%ld-%ld>\r\n", settings[i].name,
                        (long)settings[i].lo, (long)settings[i].hi);
        else if (is_auto())
            link_printf("ERR in AUTO, CTRL MAN first\r\n");
        else if (mode == VALVE_CALIBRATING || stepper3_busy())
            link_printf("ERR busy, CTRL STOP first\r\n");
        else {
            *settings[i].v = (int32_t)v;
            ev("CFG", "VALVE %s set to %ld", settings[i].name, v);
            link_printf("OK\r\n");
        }
        return;
    }
    link_printf("ERR unknown VALVE command\r\n");
}

const char *valve_setting_name(int i)
{
    return i >= 0 && i < (int)N_SETTINGS ? settings[i].name : "?";
}

void valve_settings(int32_t v[VALVE_N_SETTINGS])
{
    for (size_t i = 0; i < N_SETTINGS; i++)
        v[i] = *settings[i].v;
}

const char *valve_settings_check(const int32_t v[VALVE_N_SETTINGS])
{
    for (size_t i = 0; i < N_SETTINGS; i++)
        if (v[i] < settings[i].lo || v[i] > settings[i].hi)
            return settings[i].name;
    return NULL;
}

bool valve_set_settings(const int32_t v[VALVE_N_SETTINGS])
{
    if (valve_settings_check(v) || mode == VALVE_CALIBRATING || is_auto() || stepper3_busy())
        return false;
    for (size_t i = 0; i < N_SETTINGS; i++)
        *settings[i].v = v[i];
    return true;
}

bool valve_goto_pct10(int32_t pct10, const char **why)
{
    return valve_goto_pct100(pct10 * 10, why);
}

bool valve_goto_pct100(int32_t pct100, const char **why)
{
    if (mode == VALVE_CALIBRATING)  { *why = "calibrating, CTRL STOP to cancel"; return false; }
    if (any_latched())              { *why = "fault latched, CTRL RESET first"; return false; }
    if (is_auto())                  { *why = "Switch to MANUAL first"; return false; }
    if (!calibrated)                { *why = "valve not calibrated, VALVE CAL first"; return false; }
    int32_t to = (int32_t)(((int64_t)pct100 * pm() + 5000) / 10000);
    if (to < 0) to = 0;
    if (to > pm()) to = pm();
    if (!stepper3_goto(to))         { *why = stepper3_refusal(); return false; }
    return true;
}

bool valve_move_steps(int32_t steps, const char **why)
{
    if (mode == VALVE_CALIBRATING)  { *why = "calibrating, CTRL STOP to cancel"; return false; }
    if (any_latched())              { *why = "fault latched, CTRL RESET first"; return false; }
    if (is_auto())                  { *why = "Switch to MANUAL first"; return false; }
    int32_t from = stepper3_busy() ? stepper3_target() : stepper3_position();
    if (!stepper3_goto(from + steps)) { *why = stepper3_refusal(); return false; }
    return true;
}

bool valve_calibrate(const char **why) { return cal_start(why); }

uint32_t valve_cal_elapsed_s(void) { return mode == VALVE_CALIBRATING ? lv_tick_elaps(cal_t0) / 1000 : 0; }

uint32_t valve_cal_budget_s(void)
{
    return (uint32_t)(((int64_t)pm() * 11 / 10 + sc(n_clear + 2 * n_backoff)) / f_rate(f_home)
                      + sc(n_backoff + CREEP_EXTRA) / f_rate(f_creep) + 1);
}

int valve_cal_step(void)
{
    switch (mode == VALVE_CALIBRATING ? cal_stage : CAL_NONE) {
    case CAL_CLEAR: return 0;
    case CAL_FAST:  return 1;
    case CAL_BACK:  return 2;
    case CAL_CREEP: return 3;
    default:        return -1;
    }
}

int32_t valve_target_pct10(void) { return pct10_of(stepper3_target()); }

static int32_t milli_of(int32_t pos)
{
    const int32_t m = pm();
    return (int32_t)(((int64_t)pos * 100000 + (pos >= 0 ? m / 2 : -m / 2)) / m);
}
int32_t valve_pct_milli(void)        { return milli_of(stepper3_position()); }
int32_t valve_target_pct_milli(void) { return milli_of(stepper3_target()); }

int valve_fault_count(void)
{
    int n = 0;
    for (int i = 0; i < F_N; i++)
        n += latched[i];
    return n;
}

bool valve_fault_first(const char **tag, const char **text)
{
    for (int i = 0; i < F_N; i++)
        if (latched[i]) {
            *tag = fault_tag[i];
            *text = fault_short[i];
            return true;
        }
    return false;
}

bool valve_fault_is(const char *tag)
{
    for (int i = 0; i < F_N; i++)
        if (latched[i] && strcmp(fault_tag[i], tag) == 0)
            return true;
    return false;
}

bool valve_question(int32_t *pct10)
{
    if (question && pct10)
        *pct10 = pct10_of(question_at);
    return question;
}

bool valve_answer(bool set_zero, const char **why)
{
    if (!question)          { *why = "no switch question waiting"; return false; }
    if (set_zero && is_auto())       { *why = "Switch to MANUAL first"; return false; }
    if (set_zero && stepper3_busy()) { *why = "moving, wait for it to stop"; return false; }
    question = false;
    if (set_zero) {
        int32_t d = stepper3_position() - question_at;
        stepper3_set_position(sc(n_offset) + d);
        switch_at += sc(n_offset) - question_at;
        ev("W-01", "Point set as 0 %%, count %ld", (long)sc(n_offset));
    } else {
        ev("W-01", "Zero skipped, count kept at %ld", (long)question_at);
    }
    return true;
}

bool valve_fault_get(int i, valve_fault_t *f)
{
    int n = 0;
    for (int k = 0; k < F_N; k++) {
        if (!latched[k])
            continue;
        if (n++ != i)
            continue;
        f->tag = fault_tag[k];
        f->text = fault_short[k];
        f->age_ms = lv_tick_elaps(latched_ms[k]);
        f->mode = latched_mode[k];
        f->can_reset = can_reset((fault_t)k);
        return true;
    }
    return false;
}

int valve_event_count(void) { return evt_count < EVT_N ? (int)evt_count : EVT_N; }
uint32_t valve_event_total(void) { return evt_count; }

bool valve_event_get(int i, uint32_t *age_ms, const char **tag, const char **text)
{
    if (i < 0 || i >= valve_event_count())
        return false;
    const event_t *e = &events[(evt_count - 1 - (uint32_t)i) % EVT_N];
    *age_ms = lv_tick_elaps(e->t_ms);
    *tag = e->tag;
    *text = e->text;
    return true;
}

const char *valve_last_cal_iso(void) { return cal_done_iso; }

bool valve_last_cal(uint32_t *age_ms, uint32_t *took_s)
{
    if (!cal_done)
        return false;
    *age_ms = lv_tick_elaps(cal_done_ms);
    *took_s = cal_took_s;
    return true;
}

bool valve_beyond_switch(int32_t *steps)
{
    if (!switch_was_open)
        return false;
    *steps = switch_at - stepper3_position();
    return true;
}

bool valve_command(const char *line)
{
    const char *err;
    char p[16];

    if (strcmp(line, "CTRL STOP") == 0) {
        stop_command();
    } else if (strcmp(line, "CTRL AUTO") == 0) {
        if (valve_start_auto(&err))
            link_printf("OK AUTO at %s mB\r\n", mbar_text(p, sizeof p, ctl.sp, 1));
        else
            link_printf("ERR %s\r\n", err);
    } else if (strncmp(line, "CTRL MAN", 8) == 0 && (line[8] == ' ' || line[8] == '\0')) {
        int32_t pct10 = -1;
        if (line[8] && !parse_pct10(line + 8, &pct10))
            link_printf("ERR expected: CTRL MAN [<0-100>], one decimal\r\n");
        else if (!valve_to_manual(&err))
            link_printf("ERR %s\r\n", err);
        else if (pct10 >= 0 && !valve_goto_pct10(pct10, &err))
            link_printf("ERR MANUAL, %s\r\n", err);
        else
            link_printf("OK MANUAL%s\r\n", pct10 >= 0 ? ", moving" : "");
    } else if (strncmp(line, "CTRL SP ", 8) == 0) {
        const char *s = line + 8;
        int64_t v;
        if (!fixed_parse(&s, 3, &v) || *s || v < INT32_MIN || v > INT32_MAX)
            link_printf("ERR expected: CTRL SP <mbar>\r\n");
        else if (!valve_set_target((int32_t)v, &err))
            link_printf("ERR %s\r\n", err);
        else
            link_printf("OK target %s mB\r\n", mbar_text(p, sizeof p, ctl.sp, 1));
    } else if (strcmp(line, "CTRL RESET") == 0) {
        reset_command();
    } else if (strcmp(line, "CTRL?") == 0) {
        faults_command();
    } else if (strncmp(line, "EVT?", 4) == 0) {
        events_command(line + 4);
    } else if (strcmp(line, "VALVE?") == 0) {
        show();
    } else if (strcmp(line, "VALVE CAL") == 0) {
        if (cal_start(&err))
            link_printf("OK calibrating, up to %lu s\r\n", (unsigned long)valve_cal_budget_s());
        else
            link_printf("ERR %s\r\n", err);
    } else if (strcmp(line, "VALVE CLOSED") == 0) {
        if (valve_set_closed(&err))
            link_printf("OK closed, count 0\r\n");
        else
            link_printf("ERR %s\r\n", err);
    } else if (strncmp(line, "VALVE GOTO", 10) == 0) {
        int32_t pct10;
        if (!parse_pct10(line + 10, &pct10))
            link_printf("ERR expected: VALVE GOTO <0-100>, one decimal\r\n");
        else if (!valve_goto_pct10(pct10, &err))
            link_printf("ERR %s\r\n", err);
        else
            link_printf("OK going to %s %% (count %ld) from %ld\r\n",
                        fixed_fmt(p, sizeof p, pct10, 1), (long)stepper3_target(), (long)stepper3_position());
    } else if (strcmp(line, "VALVE SETZERO") == 0 || strcmp(line, "VALVE SKIP") == 0) {
        bool zero = line[6] == 'S' && line[7] == 'E';
        if (!valve_answer(zero, &err))
            link_printf("ERR %s\r\n", err);
        else if (zero)
            link_printf("OK count %ld\r\n", (long)stepper3_position());
        else
            link_printf("OK count kept\r\n");
    } else if (strncmp(line, "VALVE ", 6) == 0) {
        setting_command(line + 6);
    } else {
        return false;
    }
    return true;
}
