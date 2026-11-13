#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"   // lv_tick_get, lv_snprintf
#include "link.h"
#include "rtclock.h"
#include "limit.h"
#include "stepper3.h"
#include "pmc_config.h"
#include "fixed.h"
#include "cfg.h"
#include "ctrl.h"
#include "cal.h"
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
static int32_t n_backoff = 400;    // steps off the switch after the fast close, before the creep
static int32_t n_return = 400;     // steps off the switch after the creep: 0 % (0.13.6, Ryan 28 Oct)
static int32_t n_clear  = 800;     // steps off the switch if pressed at the start
static int32_t n_unused;           // settings slot 5, the zero offset until 0.13.31: always 0
static valve_ref_t ref_how;        // how the position was last set (valve_reference)

static const setting_t settings[] = {
    { "POSMAX",  &pos_max,   1000, 300000 },
    { "FHOME",   &f_home,    16,   1000 },
    { "FCREEP",  &f_creep,   16,   1000 },
    { "BACKOFF", &n_backoff, 10,   5000 },
    { "CLEAR",   &n_clear,   10,   10000 },
    { "",        &n_unused,  0,    0 },      // was OFFSET (removed in 0.13.31): 0 % is always count 0
    { "RETURN",  &n_return,  10,   5000 },
};
#define N_SETTINGS (sizeof settings / sizeof settings[0])
_Static_assert(N_SETTINGS == VALVE_N_SETTINGS, "valve.h VALVE_N_SETTINGS");

/* The settings above are in full steps (steps/s for the speeds), whatever
 * the step mode; the motor counts half steps in HALF (stepper3_scale()).
 * These give them in motor counts, so a mode change keeps the homing
 * (Ryan, 23 Oct: "just recalculate"). */
static int32_t sc(int32_t full) { return full * stepper3_scale(); }
static int32_t pm(void) { return sc(pos_max); }
// The count of the seat (the switch point): 0 % is the return above it
// (Ryan, 26 Oct; the back-off until 0.13.6), so a seated valve reads about
// -0.6 % with the defaults.
static int32_t seat_count(void) { return sc(-n_return); }
static int32_t f_rate(int32_t full)
{
    int32_t r = sc(full);
    return r > STEPPER3_RATE_MAX ? STEPPER3_RATE_MAX : r;
}

#define SETTLE_MS     150     // after a move, before the switch is read
#define CREEP_EXTRA   200     // creep goes this far past the back-off
#define EXC_MIN_MV    9500    // F-2 window
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

#define EVT_LINE 128   // link room wanted before an EVENT? line goes out (and its size)
static struct {
    bool active;
    uint32_t next, end, since;
} edump;
static void evt_dump_task(void);

static void evt_print(const event_t *e)
{
    char l[EVT_LINE];
    uint32_t age = lv_tick_elaps(e->t_ms);
    lv_snprintf(l, sizeof l, "%lu.%lu,%s,%s\r\n", (unsigned long)(age / 1000), (unsigned long)(age % 1000 / 100),
                e->tag, e->text);
    link_write(l);
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
}
#define ev valve_event

/* --------------------------------------------------------------------------
 * Faults, in the simulator's FAULT_ORDER (highest priority first).
 * ------------------------------------------------------------------------ */
typedef enum { F01, F02, F03, F04, F05, F06, F_N } fault_t;   // priority order (Ryan, 22 Oct)

static const char *const fault_tag[F_N]   = { "F-1", "F-2", "F-3", "F-4", "F-5", "F-6" };
static const char *const fault_short[F_N] = {
    "No pressure signal", "Excitation out of window", "Limit not found while calibrating",
    "Limit pressed when it should be free", "Motion timeout", "No pressure response" };

static bool latched[F_N];
static uint32_t latched_ms[F_N];    // when each latched
static valve_mode_t latched_mode[F_N];
static bool cause_f01, cause_f02;   // from the last sensor read
static bool sensed;                 // at least one sensor read since boot

/* --------------------------------------------------------------------------
 * Mode, calibration, limit question
 * ------------------------------------------------------------------------ */
typedef enum { CAL_NONE, CAL_CLEAR, CAL_FAST, CAL_BACK, CAL_CREEP, CAL_PARK } cal_stage_t;

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
static bool hold_on;              // a screen button drives the valve while pressed (valve_hold)
static const char *auto_said;     // the AUTO move refusal last logged (once per reason, per AUTO run)
static bool ref_unproven;         // Direction or coil order changed since the last homing
static bool running;              // the main loop has started (not the settings load at start-up)
static int32_t question_at;       // where the switch was pressed, full steps (a mode change rescales the count)
static int32_t q_count(void) { return question_at * stepper3_scale(); }   // in motor counts now

static uint32_t cal_done_ms, cal_took_s;   // last good calibration
static bool cal_done;
static char cal_done_iso[20];              // its wall time, "" if the clock is not set

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

// The pressure faults (F-1 no Druck, F-2 excitation) do not block the motor: manual moves and calibrating use the step
// count and the limit switch, not the pressure (Ryan 16 Oct). In AUTO they
// stop the valve and drop to MANUAL like any other fault.
static bool sensor_fault(fault_t f) { return f == F01 || f == F02; }

static bool is_auto(void) { return mode == VALVE_AUTO; }

// The warning that ended AUTO or needs a look (W-2, W-4, W-5; W-6 removed in 0.15.11), shown on
// the status strip until OK, the next Start AUTO or homing.
static char warn[72];
static uint32_t sw_far_ms; // W-4: when the switch was first read pressed away from the seat, 0 = not
static bool sw_far_said;  // W-4 raised for this occurrence
// W-4 once the switch has read pressed away from the seat this long (0.16.4,
// Ryan PR #43: the warning "should update as quickly as the limit hitting
// it"; to 0.16.3 two 1 s reads, so up to 2 s after Home's limit edge).
// Long enough that one glitch on the wire does not end AUTO.
#define SW_FAR_MS 200
// Pass band (0.16.5, Ryan 9 Nov): asked for on Motor settings > Override
// > Disable switch (Ignore switch in 0.16.21; Motor override > Switch to 0.16.18); in force
// (stepper3_limit_band) only while homed. RAM only, off at power-up. Its
// edges are saved settings from 0.16.21 (valve_pass_lo, valve_pass_hi).
static bool band_req;
static int over_n;        // W-5: consecutive reads above the sensor range in AUTO

// Shut down (valve.h): the move to 0 % running, and where it started.
#define OFF_WHY  "Controller is off: turn it on at the screen"
#define SHUT_WHY "Shutting down: STOP to cancel"
static bool shut_moving;
static int32_t shut_from;

static bool any_latched(void)
{
    for (int i = 0; i < F_N; i++)
        if (latched[i] && !sensor_fault((fault_t)i))
            return true;
    return false;
}

// OFF is not a lock here (0.15.1): the controller starts OFF, before
// cfg_init() sets the saved step mode, which a lock would refuse. Nothing
// can move the valve while OFF anyway: the OFF screen covers the screens
// and valve_off_why() refuses the rest.
static void update_lock(void)
{
    stepper3_lock(mode == VALVE_CALIBRATING ? "Homing: STOP to cancel"
                  : mode == VALVE_SHUTDOWN ? SHUT_WHY
                  : any_latched() ? "Reset the fault first"
                  : is_auto() ? "Stop AUTO first" : NULL);
}

static void cal_end(valve_mode_t m)
{
    mode = m;
    cal_stage = CAL_NONE;
    cal_moving = false;
    cal_settling = false;
    update_lock();
}

// Ends the homing. kept: the step count still holds, so an earlier homing
// stays: an operator STOP (the count kept counting through the cancelled
// moves) or a first move refused (nothing moved).
static void cal_stop(const char *why, bool kept)
{
    if (mode != VALVE_CALIBRATING)
        return;
    stepper3_stop();
    seq_seen = stepper3_move_seq();
    bool keep = cal_was && kept;
    ev("REF", "Homing aborted (%s)%s", why, keep ? ", earlier homing kept" : "");
    calibrated = keep;
    cal_end(keep ? VALVE_MANUAL : VALVE_NOT_CAL);
}

static void cal_abort(const char *why) { cal_stop(why, !strcmp(why, "STOP")); }

// A shut down ends without parking: MANUAL (NOT HOMED if the count was
// lost), the valve where it stopped; strip: the warning to show, or NULL.
static void shut_cancel(const char *why, const char *strip)
{
    if (mode != VALVE_SHUTDOWN)
        return;
    stepper3_stop();
    seq_seen = stepper3_move_seq();
    shut_moving = false;
    mode = calibrated ? VALVE_MANUAL : VALVE_NOT_CAL;
    if (strip)
        lv_snprintf(warn, sizeof warn, "%s", strip);
    ev("OFF", "Shut down stopped (%s), %s", why, mode == VALVE_MANUAL ? "MANUAL" : "not homed");
    update_lock();
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
        shut_cancel(fault_tag[f], NULL);
    }
    if (uncalibrate) {
        calibrated = false;
        if (mode != VALVE_OFF)
            mode = VALVE_NOT_CAL;
        question = false;   // its count belongs to the homing just lost
    }
    update_lock();
}

static bool cal_move(int32_t to, int32_t rate)
{
    cal_moving = stepper3_goto_rate(to, (uint32_t)rate);
    seq_seen = stepper3_move_seq();
    return cal_moving;
}

// A homing move refused (the saved position not written): the homing ends with the reason in the log, as for STOP, and no
// fault (F-3 is the switch not found). first: nothing has moved yet.
static bool cal_go(int32_t to, int32_t rate, bool first)
{
    if (cal_move(to, rate))
        return true;
    cal_stop(stepper3_refusal(), first);
    return false;
}

static bool cal_fast(bool first)
{
    int32_t n = (int32_t)(((int64_t)pm() * 11 + 5) / 10);
    cal_stage = CAL_FAST;
    return cal_go(stepper3_position() - n, f_rate(f_home), first);
}

static void cal_creep(void)
{
    cal_stage = CAL_CREEP;
    cal_go(stepper3_position() - sc(n_backoff + CREEP_EXTRA), f_rate(f_creep), false);
}

static bool cal_start(const char **err)
{
    if (mode == VALVE_CALIBRATING) { *err = "Homing: STOP to cancel"; return false; }
    if (valve_off_why())           { *err = valve_off_why(); return false; }
    if (is_auto())                 { *err = "Stop AUTO first"; return false; }
    if (any_latched())             { *err = "Reset the fault first"; return false; }
    stepper3_stop();
    stepper3_override_cancel();
    cal_settling = false;
    cal_was = calibrated;
    calibrated = false;
    mode = VALVE_CALIBRATING;
    cal_t0 = lv_tick_get();
    update_lock();
    ev("REF", "Homing started at count %ld", (long)stepper3_position());
    bool ok;
    if (limit_open_now()) {
        cal_stage = CAL_CLEAR;
        ok = cal_go(stepper3_position() + sc(n_clear), f_rate(f_home), true);
    } else {
        ok = cal_fast(true);
    }
    if (!ok) {
        *err = stepper3_refusal();   // the homing has ended (cal_go), nothing moved
        return false;
    }
    // Homing answers the switch question and the warning, once it is under
    // way: a refused start leaves them for the operator.
    question = false;
    warn[0] = 0;
    return true;
}

// A calibration move has ended.
static void cal_move_ended(stepper3_stop_t why)
{
    cal_moving = false;
    switch (cal_stage) {
    case CAL_CLEAR:
    case CAL_BACK:
    case CAL_PARK:
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
            cal_go(stepper3_position() + sc(n_backoff), f_rate(f_home), false);
        } else if (why == STEPPER3_STOP_DONE) {
            raise(F03, true, "Homing: switch not found within %ld steps",
                  (long)(((int64_t)pm() * 11 + 5) / 10));
        } else {
            cal_abort(stepper3_stop_name(why));
        }
        break;
    case CAL_CREEP:
        if (why == STEPPER3_STOP_LIMIT) {
            if (!stepper3_set_position(seat_count())) {
                cal_abort("count not set");
                break;
            }
            // Then off the switch by the return, so it is not left
            // pressed against the seat; that point is 0 % (Ryan, 26 Oct).
            cal_stage = CAL_PARK;
            cal_go(0, f_rate(f_home), false);
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

static void cal_homed(void)
{
    calibrated = true;
    ref_unproven = false;
    ref_how = VALVE_REF_HOMED;
    cal_done_ms = lv_tick_get();
    cal_done = true;
    cal_took_s = lv_tick_elaps(cal_t0) / 1000;
    lv_strlcpy(cal_done_iso, rtclock_iso(), sizeof cal_done_iso);
    ev("REF", "Homed in %lu s, switch at %ld, 0 %% at %ld", (unsigned long)cal_took_s, (long)seat_count(),
       (long)stepper3_position());
    cal_end(VALVE_MANUAL);
}

static void cal_settled(void)
{
    cal_settling = false;
    int32_t moved = sc(cal_stage == CAL_CLEAR ? n_clear : cal_stage == CAL_PARK ? n_return : n_backoff);
    if (limit_open_now()) {
        raise(F04, true, "Switch still active after %ld-step %s", (long)moved,
              cal_stage == CAL_CLEAR ? "clearance move" : cal_stage == CAL_PARK ? "return" : "back-off");
    } else if (cal_stage == CAL_CLEAR) {
        cal_fast(false);
    } else if (cal_stage == CAL_BACK) {
        cal_creep();
    } else {
        cal_homed();
    }
}

static void limit_hit(void)
{
    if (!calibrated || question)
        return;
    question_at = stepper3_position() / stepper3_scale();
    question = true;
    char p[16];
    ev("W-1", "Limit switch pressed at count %ld (%s %%): Set as seat or Skip",
       (long)q_count(), pct_text(p, sizeof p, q_count()));

}

/* --------------------------------------------------------------------------
 * Shut down (0.15.0, Ryan 4 Nov: operators forget to close the valve before
 * switching off; to 0 %, not onto the switch, from 0.15.1). valve.h has the
 * rules; valvepos.c keeps the count while OFF.
 * ------------------------------------------------------------------------ */
static void seq_check(void);
static void far_check(void);

static void go_off(void)
{
    stepper3_off();   // coils off, whatever the hold setting
    seq_seen = stepper3_move_seq();
    shut_moving = false;
    question = false;
    mode = VALVE_OFF;
    update_lock();
}

static void shut_move_ended(stepper3_stop_t why)
{
    shut_moving = false;
    if (why == STEPPER3_STOP_DONE) {
        ev("OFF", "Valve at 0 %%, count %ld; coils off, OFF", (long)stepper3_position());
        go_off();
    } else if (why == STEPPER3_STOP_LIMIT) {
        // The switch before 0 %: ice, a switch tripping early or a count
        // that slipped. Only a look can tell: MANUAL and the switch question,
        // as for any other move.
        shut_cancel("limit switch pressed", "Shut down stopped: limit switch pressed before 0 %");
        limit_hit();
    } else {
        shut_cancel(stepper3_stop_name(why), NULL);
    }
}

bool valve_shutdown(bool close, const char **why)
{
    if (mode == VALVE_SHUTDOWN || mode == VALVE_OFF) return true;
    if (mode == VALVE_CALIBRATING) { *why = "Homing: STOP to cancel"; return false; }
    close = close && calibrated;   // not homed: no 0 % to go to
    if (close && any_latched())    { *why = "Reset the fault first"; return false; }
    seq_check();   // a move that ended just before is handled first
    bool was_auto = is_auto();
    stepper3_override_cancel();
    if (stepper3_busy()) {
        stepper3_stop();
        seq_seen = stepper3_move_seq();
    }
    question = false;
    warn[0] = 0;
    int32_t pos = stepper3_position();
    char p[16];
    if (!calibrated) {
        // Not homed: where 0 % is, is not known, so the valve stays put.
        ev("OFF", "Shut down, not homed: valve not moved; coils off, OFF");
        go_off();
        return true;
    }
    if (!close) {
        // The operator chose not to close it (0.15.7): it stays where it
        // is, and valvepos.c keeps the count as at 0 %.
        ev("OFF", "Shut down%s, valve left at %s %%: coils off, OFF", was_auto ? ", AUTO ended" : "",
           pct_text(p, sizeof p, pos));
        go_off();
        return true;
    }
    if (pos == 0) {
        ev("OFF", "Shut down%s at 0 %%: coils off, OFF", was_auto ? ", AUTO ended," : "");
        go_off();
        return true;
    }
    shut_from = pos;
    mode = VALVE_SHUTDOWN;
    update_lock();
    ev("OFF", "Shut down%s: to 0 %% from %s %%", was_auto ? ", AUTO ended" : "", pct_text(p, sizeof p, pos));
    shut_moving = stepper3_goto(0);
    seq_seen = stepper3_move_seq();
    if (!shut_moving) {
        *why = stepper3_refusal();
        shut_cancel(*why, NULL);
        return false;
    }
    return true;
}

bool valve_turn_on(const char **why)
{
    if (mode != VALVE_OFF) { *why = "Not off"; return false; }
    mode = calibrated ? VALVE_MANUAL : VALVE_NOT_CAL;
    update_lock();
    ev("MODE", "Turned on at the screen: %s", valve_mode_name());
    return true;
}

bool valve_restore_parked(int32_t count, bool homed, const char **why)
{
    if (calibrated || (mode != VALVE_OFF && mode != VALVE_NOT_CAL)) { *why = "Position already set"; return false; }
    if (ref_unproven)               { *why = "Direction or coil order changed: home the valve first"; return false; }
    if (any_latched())              { *why = "Reset the fault first"; return false; }
    if (stepper3_busy() || !stepper3_set_position(count)) { *why = "Wait for the valve to stop"; return false; }
    calibrated = true;
    ref_how = homed ? VALVE_REF_PARKED : VALVE_REF_SAVED;
    char p[16];
    ev("REF", "Kept from the last shut down: count %ld (%s %%)%s", (long)count, pct_text(p, sizeof p, count),
       homed ? "" : ", not homed");
    if (mode == VALVE_NOT_CAL) {
        mode = VALVE_MANUAL;   // turned on already
        update_lock();
    }
    return true;
}

bool valve_shutdown_progress(int *left_pct, int *secs)
{
    if (mode != VALVE_SHUTDOWN)
        return false;
    int32_t span = shut_from < 0 ? -shut_from : shut_from, left = stepper3_position();
    if (left < 0) left = -left;
    if (left > span) left = span;
    *left_pct = span > 0 ? (int)((int64_t)left * 100 / span) : 0;
    int32_t r = (int32_t)stepper3_rate();
    *secs = r > 0 ? (int)((left + r - 1) / r) : -1;
    return true;
}

const char *valve_off_why(void)
{
    return mode == VALVE_OFF ? OFF_WHY : mode == VALVE_SHUTDOWN ? SHUT_WHY : NULL;
}

/* --------------------------------------------------------------------------
 * API
 * ------------------------------------------------------------------------ */
void valve_init(void)
{
    ctrl_init(&ctl);
    seq_seen = stepper3_move_seq();
    stepper3_on_reference_change(valve_uncalibrate);
    // Every start is OFF (0.15.1, Ryan 4 Nov): a double tap on the screen
    // turns it on. valvepos.c may then set the count kept at Shut down.
    mode = VALVE_OFF;
    ev("BOOT", "Power-up (event only), OFF, valve not homed");
    update_lock();
}

// A move has ended since the last look: why (F-5, homing, the limit).
static void seq_check(void)
{
    if (stepper3_take_reversed())
        raise(F04, true, "Switch pressed during an opening move at count %ld: Direction, coil order or switch",
              (long)stepper3_position());
    if (stepper3_take_stall()) {
        ev("SYS", "Move stopped at count %ld: the main loop stalled for over 1 s", (long)stepper3_position());
        cal_abort("main loop stalled");   // a homing move cut short: no settle follows
        shut_cancel("main loop stalled", NULL);
    }
    uint32_t seq = stepper3_move_seq();
    if (seq != seq_seen) {
        seq_seen = seq;
        stepper3_stop_t why = stepper3_last_stop();
        if (why == STEPPER3_STOP_TIMEOUT)
            raise(F05, false, "Motion timeout at count %ld", (long)stepper3_position());
        else if (why == STEPPER3_STOP_REVERSED || why == STEPPER3_STOP_STALL)
            ;   // handled above, from the latches
        else if (mode == VALVE_CALIBRATING && cal_moving)
            cal_move_ended(why);
        else if (mode == VALVE_SHUTDOWN && shut_moving)
            shut_move_ended(why);
        else if (why == STEPPER3_STOP_LIMIT)
            limit_hit();
    }
}

void valve_task(void)
{
    running = true;
    evt_dump_task();
    seq_check();
    // The pass band in counts from 0 %, again each pass (Motor settings can
    // change the travel); not while not homed or homing: the count is not
    // the valve's then.
    stepper3_limit_band(band_req && calibrated && mode != VALVE_CALIBRATING && mode != VALVE_OFF,
                        pm() * valve_pass_lo() / 100, pm() * valve_pass_hi() / 100);
    far_check();
    if (hold_on && !stepper3_busy())
        hold_on = false;   // the held move has ended (released, the switch, its end)
    if (mode == VALVE_CALIBRATING && cal_settling && lv_tick_elaps(cal_settle_from) >= SETTLE_MS)
        cal_settled();
}

/* --------------------------------------------------------------------------
 * AUTO (stage 8): ctrl.c works out the command, this runs it.
 * ------------------------------------------------------------------------ */
// The tuning from cfg.c (CFG TUNE units: 0.01 %/mbar, 0.01 min, 0.1 %,
// steps, +-1, 0.01 mbar, 0.01 % per mbar/s).
static ctrl_tune_t tune_now(void)
{
    ctrl_tune_t k;
    k.kp = (float)cfg_tune(CFG_KP) / 100.0f;
    k.ti = (float)cfg_tune(CFG_TI) / 100.0f;
    k.outmin = (float)cfg_tune(CFG_OUTMIN) / 10.0f;
    k.db = sc(cfg_tune(CFG_DB));   // full steps, in motor counts (HALF doubles)
    k.kdir = (int)cfg_tune(CFG_KDIR);
    k.band = (float)cfg_tune(CFG_BAND) / 100.0f;
    k.kd = (float)cfg_tune(CFG_KD) / 100.0f;
    k.pos_max = pm();
    k.steps = stepper3_scale();
    // Sets 2 to 4, the schedule (0.1 mbar boundaries, % hysteresis) and
    // the closing deadband (0 = none: one step).
    for (int s = 2; s <= 4; s++) {
        k.sets[s - 2].kp = (float)cfg_tune(cfg_pid_key(s, 0)) / 100.0f;
        k.sets[s - 2].ti = (float)cfg_tune(cfg_pid_key(s, 1)) / 100.0f;
        k.sets[s - 2].kd = (float)cfg_tune(cfg_pid_key(s, 2)) / 100.0f;
    }
    k.sched = (int)cfg_tune(CFG_SCHED);
    k.b1 = (float)cfg_tune(CFG_B1) / 10.0f;
    k.b2 = (float)cfg_tune(CFG_B2) / 10.0f;
    k.b3 = (float)cfg_tune(CFG_B3) / 10.0f;
    k.hyst = (float)cfg_tune(CFG_HYST) / 100.0f;
    k.db_close = cfg_tune(CFG_DBC) > 0 ? sc(cfg_tune(CFG_DBC)) : 1;
    return k;
}

// The PID set in use: in AUTO the one the schedule has switched to; else
// the one AUTO would start with now (the lower of the pressure and the
// setpoint, as ctrl_start; the schedule is always on from 0.16.20).
int valve_pid_set(void)
{
    ctrl_tune_t k = tune_now();
    if (k.sched == CTRL_SCHED_OFF)
        return 1;
    if (mode == VALVE_AUTO && ctl.set >= 1 && ctl.set <= 4)
        return ctl.set;
    return pv_ok ? ctrl_pick_set(&k, 0, ctl.sp_set ? fminf(pv_mbar, ctl.sp) : pv_mbar) : 1;
}

int32_t valve_range_mmbar(void)
{
    return cfg_sensor_range_mmbar();   // the 15 psia Druck's (one sensor per board, 0.16.2)
}

static float range_mbar(void) { return (float)valve_range_mmbar() / 1000.0f; }

static const char *mbar_text(char *buf, size_t n, float mbar, int dec)
{
    int64_t scale = dec == 2 ? 100 : 10;
    return fixed_fmt(buf, n, (int64_t)(mbar * (float)scale + (mbar >= 0 ? 0.5f : -0.5f)), dec);
}

// A warning ends AUTO: the valve stops where it is, MANUAL.
static void auto_end(const char *strip)
{
    stepper3_stop();
    mode = VALVE_MANUAL;
    lv_snprintf(warn, sizeof warn, "%s", strip);
    update_lock();
}

// The switch reads pressed with the count well off the seat: past the
// clearing move and the back-off of homing, where it has always released.
static bool switch_far(void)
{
    return calibrated && mode != VALVE_CALIBRATING && limit_open_now() && !stepper3_limit_ignored() &&
           stepper3_position() > sc(n_clear + (n_backoff > n_return ? n_backoff : n_return));
}

// W-4, every pass of the main loop: the switch pressed away from the seat
// for SW_FAR_MS.
static void far_check(void)
{
    char pc[16], v[16];
    if (!switch_far()) {
        sw_far_ms = 0;
        sw_far_said = false;
    } else if (!sw_far_ms) {
        sw_far_ms = lv_tick_get() | 1;   // 0 means not pressed
    } else if (lv_tick_elaps(sw_far_ms) >= SW_FAR_MS && !sw_far_said) {
        sw_far_said = true;
        int32_t pos = stepper3_position();
        bool was_auto = is_auto();
        fixed_fmt(v, sizeof v, fixed_round(milli_of(pos), 1), 2);
        char t[72];
        lv_snprintf(t, sizeof t, "W-4 Switch pressed at %s %%: check the switch", v);
        if (was_auto)
            auto_end(t);
        else
            lv_snprintf(warn, sizeof warn, "%s", t);
        ev("W-4", "Switch pressed at count %ld (%s %%), off the seat: check switch, wiring, position",
           (long)pos, pct_text(pc, sizeof pc, pos));
        if (was_auto)
            ev("MODE", "Manual after W-4, valve idle at %s %%", pct_text(pc, sizeof pc, pos));
    }
}

// W-5, once a second; two reads in a row, so one glitch does not end AUTO.
static void warn_checks(void)
{
    char pc[16], v[16];
    if (!is_auto() || !pv_ok || pv_mbar <= range_mbar()) {
        over_n = 0;
    } else if (++over_n >= 2) {
        over_n = 0;
        auto_end("W-5 Pressure above the sensor range: switched to MANUAL");
        ev("W-5", "Pressure %s mbar above the sensor range (%s mbar), AUTO ended, MANUAL, valve idle",
           mbar_text(pc, sizeof pc, pv_mbar, 1), mbar_text(v, sizeof v, range_mbar(), 1));
    }
}

// Once a second, after the faults of this read.
static void auto_step(void)
{
    warn_checks();
    ctrl_track(&ctl, stepper3_position(), calibrated && mode != VALVE_CALIBRATING);
    if (mode != VALVE_AUTO || !pv_ok) {
        ctrl_idle(&ctl);
        return;
    }
    ctrl_tune_t k = tune_now();
    ctrl_in_t in = { lv_tick_get(), pv_mbar, stepper3_position(), stepper3_busy(),
                     limit_open_now() && !stepper3_limit_ignored(), range_mbar() };
    const int set0 = ctl.set;
    const ctrl_result_t res = ctrl_step(&ctl, &k, &in);
    if (ctl.set != set0 && set0 >= 1) {
        char a[16];
        ev("MODE", "AUTO PID set %d to %d, pressure %s mbar", set0, ctl.set, mbar_text(a, sizeof a, ctl.pv_f, 1));
    }
    switch (res) {
    case CTRL_RUN:
        // The closed switch blocks closing from rest (stepper3 refuses it);
        // ctrl.c holds the integral meanwhile.
        if (ctl.move) {
            if (stepper3_goto(ctl.move_to)) {
                ctrl_sent(&ctl);
                auto_said = NULL;
            } else if (!(ctl.move_to < stepper3_position() && stepper3_limit_open()) &&
                       stepper3_refusal() != auto_said) {
                auto_said = stepper3_refusal();
                ev("MODE", "AUTO move refused: %s", auto_said);
            }
        }
        break;
    case CTRL_W02:
        auto_end("W-2 Not settling: switched to MANUAL, valve idle");
        ev("W-2", "Convergence timeout after %d s, AUTO ended, MANUAL, valve idle", CTRL_TCONV_S);
        break;
    case CTRL_F06:
        raise(F06, false, "No pressure response in %d windows: drive slipping, motor unplugged or needle seized",
              CTRL_NRESP);
        break;
    }
}

bool valve_auto(void) { return is_auto(); }
const char *valve_warning(void) { return warn[0] ? warn : NULL; }
void valve_warning_clear(void) { warn[0] = 0; }

// A screen button is driving the valve while pressed (Close or Open held
// on Motor override): its move has no real end, so AUTO
// and remote moves wait for the release. Only while that move runs.
void valve_hold(bool on) { hold_on = on; }
static bool holding(void) { return hold_on && stepper3_busy(); }

// Why AUTO cannot start now (with the target as set), or NULL.
static const char *auto_why(void)
{
    if (mode == VALVE_CALIBRATING) return "Homing: STOP to cancel";
    if (valve_off_why()) return valve_off_why();
    ctrl_tune_t k = tune_now();
    const char *why = ctrl_check(&ctl, &k, calibrated, valve_fault_count() > 0, pv_ok, pv_mbar, range_mbar());
    if (!why && switch_far())
        why = "Limit switch pressed away from the seat";
    // A bench cal point being taken would change the reading AUTO uses.
    if (!why && cal_capturing())
        why = "Sensor calibration point being taken";
    // The switch question puts the count in doubt; the screen asks it first.
    if (!why && question)
        why = "Limit switch question waiting at the screen";
    if (!why && holding())
        why = "Valve being moved on the screen";
    return why;
}

bool valve_start_auto(const char **why)
{
    if (mode == VALVE_AUTO) return true;
    if ((*why = auto_why()) != NULL)
        return false;
    ctrl_tune_t k = tune_now();
    auto_said = NULL;
    // Bumpless from where the valve is going (a manual move still running
    // carries on to its end under AUTO).
    int32_t now = stepper3_target();
    ctrl_track(&ctl, stepper3_position(), false);
    ctrl_start(&ctl, &k, lv_tick_get(), pv_mbar, now);
    mode = VALVE_AUTO;
    warn[0] = 0;
    over_n = 0;
    update_lock();
    char a[16], p[16];
    char st[16] = "";
    if (k.sched != CTRL_SCHED_OFF)
        lv_snprintf(st, sizeof st, ", PID set %d", ctl.set);
    ev("MODE", "%s at setpoint %s mbar, valve %s %%%s", "Auto",
       mbar_text(a, sizeof a, ctl.sp, 1), pct_text(p, sizeof p, now), st);
    return true;
}

bool valve_start_auto_at(int32_t mmbar, const char **why)
{
    // In AUTO a new target goes through the setpoint keypad.
    if (mode == VALVE_AUTO) { *why = "Already in AUTO: change the setpoint"; return false; }
    // Checked with the new target in place, then put back: it is only set
    // (and logged) if AUTO will start.
    int32_t fs = valve_range_mmbar();
    if (mmbar < 0 || mmbar > fs) { *why = "Setpoint outside the sensor range"; return false; }
    ctrl_t was = ctl;
    ctrl_set_sp(&ctl, (float)fixed_round(mmbar, 2) / 10.0f, range_mbar(), lv_tick_get());
    *why = auto_why();
    ctl = was;
    if (*why)
        return false;
    return valve_set_target(mmbar, why) && valve_start_auto(why);
}

bool valve_to_manual(const char **why)
{
    if (mode == VALVE_MANUAL) return true;
    if (valve_off_why()) { *why = valve_off_why(); return false; }
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
        ev("SP", "Setpoint %s rejected, allowed 0 to %s mbar", fixed_fmt(a, sizeof a, fixed_round(mmbar, 2), 1),
           fixed_fmt(b, sizeof b, fixed_round(fs, 2), 1));
        *why = "Setpoint outside the sensor range";
        return false;
    }
    int32_t v = (int32_t)fixed_round(mmbar, 2);   // 0.1 mbar
    // The same target again (a master writing it every scan, SETPOINT
    // repeated) changes nothing: no event, and the W-2 and Stable timers
    // are not restarted.
    if (ctl.sp_set && lroundf(ctl.sp * 10.0f) == v)
        return true;
    ctrl_set_sp(&ctl, (float)v / 10.0f, range_mbar(), lv_tick_get());
    ev("SP", "Setpoint %s mbar", fixed_fmt(a, sizeof a, v, 1));
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
    // After a Direction or coil-order change the valve may turn the other
    // way: only homing (which finds the switch) proves it, not a typed or
    // saved position (opening would then drive into the seat).
    if (running && (!strcmp(why, "motor direction changed") || !strcmp(why, "coil order changed")))
        ref_unproven = true;   // not the saved settings applied at start-up
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
    if (mode != VALVE_OFF)
        mode = VALVE_NOT_CAL;
    ev("REF", "Homing cleared (%s)", why);
    update_lock();
}

bool valve_restore(int32_t count, const char **why)
{
    if (mode == VALVE_CALIBRATING) { *why = "Homing: STOP to cancel"; return false; }
    if (valve_off_why())            { *why = valve_off_why(); return false; }
    if (is_auto())                  { *why = "Stop AUTO first"; return false; }
    if (calibrated)                 { *why = "Position already set"; return false; }
    if (ref_unproven)               { *why = "Direction or coil order changed: home the valve first"; return false; }
    if (any_latched())              { *why = "Reset the fault first"; return false; }
    if (stepper3_busy() || !stepper3_set_position(count)) { *why = "Wait for the valve to stop"; return false; }
    calibrated = true;
    ref_how = VALVE_REF_SAVED;
    mode = VALVE_MANUAL;
    char p[16];
    ev("REF", "Last saved position restored: count %ld (%s %%), not verified by homing",
       (long)count, pct_text(p, sizeof p, count));
    update_lock();
    return true;
}

// The operator's position: 0 % where it stands (Set as 0 %), or a typed %.
// Set as 0 % reads 0.00 % (0.14.14, Ryan 4 Nov), not the seat below it.
static bool set_by_hand(bool zero, int32_t pct100, const char **why)
{
    if (pct100 < 0 || pct100 > 10000) { *why = "Outside 0 to 100 %"; return false; }
    if (mode == VALVE_CALIBRATING) { *why = "Homing: STOP to cancel"; return false; }
    if (valve_off_why())            { *why = valve_off_why(); return false; }
    if (is_auto())                  { *why = "Stop AUTO first"; return false; }
    if (any_latched())              { *why = "Reset the fault first"; return false; }
    if (ref_unproven)               { *why = "Direction or coil order changed: home the valve first"; return false; }
    int32_t c = zero ? 0 : (int32_t)(((int64_t)pct100 * pm() + 5000) / 10000);
    if (stepper3_busy() || !stepper3_set_position(c)) { *why = "Wait for the valve to stop"; return false; }
    calibrated = true;
    ref_how = VALVE_REF_HAND;
    mode = VALVE_MANUAL;
    char p[16];
    if (zero)
        ev("REF", "Set as 0 %% by the operator: count %ld (%s %%), not verified by homing", (long)c,
           pct_text(p, sizeof p, c));
    else
        ev("REF", "Position set by the operator: count %ld (%ld.%02ld %%), not verified by homing",
           (long)c, (long)(pct100 / 100), (long)(pct100 % 100));
    update_lock();
    return true;
}

bool valve_set_closed(const char **why) { return set_by_hand(true, 0, why); }
bool valve_set_position(int32_t pct100, const char **why) { return set_by_hand(false, pct100, why); }

valve_mode_t valve_mode(void)   { return mode; }
bool valve_calibrated(void)     { return calibrated; }
valve_ref_t valve_reference(void) { return calibrated ? ref_how : VALVE_REF_NONE; }
int32_t valve_pos_max(void)     { return pm(); }   // in motor counts

bool valve_limit_band(void) { return band_req; }
int valve_pass_lo(void)      { return (int)cfg_tune(CFG_PASS_LO); }
int valve_pass_hi(void)      { return (int)cfg_tune(CFG_PASS_HI); }

void valve_limit_band_set(bool on)
{
    if (on == band_req)
        return;
    band_req = on;
    valve_event("LIM", on ? "Limit switch disabled from %d to %d %% open: on (screen)"
                          : "Limit switch disabled from %d to %d %% open: off (screen)", valve_pass_lo(), valve_pass_hi());
}
int32_t valve_pct10(void)       { return pct10_of(stepper3_position()); }
bool valve_fault_latched(void)  { return any_latched(); }
bool valve_motion_blocked(void) { return any_latched(); }

const char *valve_mode_name(void)
{
    switch (mode) {
    case VALVE_CALIBRATING: return "HOMING";
    case VALVE_MANUAL:      return "MANUAL";
    case VALVE_AUTO:        return "AUTO";
    case VALVE_SHUTDOWN:    return "SHUTDOWN";
    case VALVE_OFF:         return "OFF";
    default:                return "NOT HOMED";
    }
}

static const char *cal_stage_name(void)
{
    static const char *const names[] = { "", "moving off the switch", "1 of 4 closing to the switch",
                                         "2 of 4 backing off", "3 of 4 slow close",
                                         "4 of 4 return" };
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
        // No position here: it changes every second in AUTO, and the DATA
        // line carries it; this one prints on a change of state.
        char a[16];
        lv_snprintf(status_text, sizeof status_text, "Valve: %s at %s mbar%s%s", valve_mode_name(),
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
    seq_check();   // a move that ended just before (at the switch: W-1) is not this stop's
    bool was_shut = mode == VALVE_SHUTDOWN;   // not if 0 % has just been reached (OFF)
    stepper3_stop();
    seq_seen = stepper3_move_seq();
    cal_abort("STOP");
    if (was_auto) {
        mode = VALVE_MANUAL;
        update_lock();
    }
    ev("STOP", "Operator stop at count %ld%s", (long)stepper3_position(),
       was_cal ? ", calibrating cancelled" : was_auto ? ", AUTO ended, MANUAL"
       : was_shut ? ", shut down cancelled" : "");
    shut_cancel("STOP", NULL);
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


// EVENT? <n> (remote.c): "EVENT <count>" now, then the newest count events,
// newest first, as "<age s>,<tag>,<text>", a line at a time from
// valve_task() as the link has room, so a slow reader does not hold up the
// main loop (as TREND DUMP).
bool valve_events_sending(void) { return edump.active; }
void valve_events_stop(void) { edump.active = false; }

void valve_events_send(int n)
{
    uint32_t have = evt_count < EVT_N ? evt_count : EVT_N;
    if ((uint32_t)n > have)
        n = (int)have;
    char c[24];
    lv_snprintf(c, sizeof c, "EVENT %d\r\n", n);
    link_write(c);
    edump.next = evt_count;
    edump.end = evt_count - (uint32_t)n;
    edump.since = lv_tick_get();
    edump.active = n > 0;
}

static void evt_dump_task(void)
{
    if (!edump.active)
        return;
    for (int lines = 0; lines < 8; lines++) {      // a few per loop pass
        if (edump.next <= edump.end) {
            edump.active = false;
            return;
        }
        if (link_tx_room() < EVT_LINE) {
            if (lv_tick_elaps(edump.since) >= 2000)
                edump.active = false;               // nobody reading: drop the rest
            return;
        }
        edump.since = lv_tick_get();
        uint32_t i = --edump.next;
        if (evt_count - i <= EVT_N)                 // not yet written over
            evt_print(&events[i % EVT_N]);
        else
            link_write("0.0,LOST,written over\r\n");   // keeps the count right
    }
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
    if (valve_settings_check(v) || mode == VALVE_CALIBRATING || mode == VALVE_SHUTDOWN || is_auto() ||
        stepper3_busy())
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
    if (mode == VALVE_CALIBRATING)  { *why = "Homing: STOP to cancel"; return false; }
    if (valve_off_why())            { *why = valve_off_why(); return false; }
    if (any_latched())              { *why = "Reset the fault first"; return false; }
    if (is_auto())                  { *why = "Stop AUTO first"; return false; }
    if (!calibrated)                { *why = "Home the valve first"; return false; }
    if (holding())                  { *why = "Valve being moved on the screen"; return false; }
    int32_t to = (int32_t)(((int64_t)pct100 * pm() + 5000) / 10000);
    if (to < 0) to = 0;
    if (to > pm()) to = pm();
    if (!stepper3_goto(to))         { *why = stepper3_refusal(); return false; }
    return true;
}

bool valve_calibrate(const char **why) { return cal_start(why); }

uint32_t valve_cal_elapsed_s(void) { return mode == VALVE_CALIBRATING ? lv_tick_elaps(cal_t0) / 1000 : 0; }

uint32_t valve_cal_budget_s(void)
{
    return (uint32_t)(((int64_t)pm() * 11 / 10 + sc(n_clear + 2 * n_backoff + n_return)) / f_rate(f_home)
                      + sc(n_backoff + CREEP_EXTRA) / f_rate(f_creep) + 1);
}

int valve_cal_step(void)
{
    switch (mode == VALVE_CALIBRATING ? cal_stage : CAL_NONE) {
    case CAL_CLEAR: return 0;
    case CAL_FAST:  return 1;
    case CAL_BACK:  return 2;
    case CAL_CREEP: return 3;
    case CAL_PARK:  return 4;
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
        *pct10 = pct10_of(q_count());
    return question;
}

bool valve_answer(bool set_zero, const char **why)
{
    if (!question)          { *why = "No switch question waiting"; return false; }
    if (set_zero && is_auto())       { *why = "Stop AUTO first"; return false; }
    if (set_zero && valve_off_why()) { *why = valve_off_why(); return false; }
    if (set_zero && stepper3_busy()) { *why = "Wait for the valve to stop"; return false; }
    question = false;
    if (set_zero) {
        int32_t d = stepper3_position() - q_count();
        stepper3_set_position(seat_count() + d);
        ev("W-1", "Point set as the seat, count %ld (0 %% is the return above it)", (long)seat_count());
        if (!strncmp(warn, "W-4", 3))
            warn[0] = 0;   // the switch was right, the count was not
    } else {
        ev("W-1", "Zero skipped, count kept at %ld", (long)q_count());
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


