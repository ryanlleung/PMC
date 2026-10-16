#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"   // lv_tick_get, lv_snprintf
#include "link.h"
#include "rtclock.h"
#include "limit.h"
#include "stepper3.h"
#include "fixed.h"
#include "valve.h"

/* --------------------------------------------------------------------------
 * Settings (CFG SAVE keeps them, cfg.c). Defaults and ranges from the simulator
 * (DEF_P / RANGE): POS_MAX is 6 turns of the 26M motor through the PK22
 * 344:1 gearbox, 99,072 full steps.
 * ------------------------------------------------------------------------ */
typedef struct {
    const char *name;
    int32_t *v;
    int32_t lo, hi;
} setting_t;

static int32_t pos_max  = 99072;   // steps, 0 % to 100 %
static int32_t f_home   = 400;     // steps/s, clearance, fast close, back-off
static int32_t f_creep  = 25;      // steps/s, slow final close
static int32_t n_backoff = 400;    // steps off the switch before the creep
static int32_t n_clear  = 800;     // steps off the switch if pressed at the start
static int32_t n_offset = 0;       // count given to the switch point

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

#define SETTLE_MS     150     // after a move, before the switch is read
#define CREEP_EXTRA   200     // creep goes this far past the back-off
#define EXC_MIN_MV    9500    // F-03 window
#define EXC_MAX_MV    10500

/* --------------------------------------------------------------------------
 * Event log: 200 in RAM, each also sent as an EVT line.
 * ------------------------------------------------------------------------ */
#define EVT_N 200
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
typedef enum { F01, F03, F07, F09, F13, F12, F_N } fault_t;

static const char *const fault_tag[F_N]   = { "F-01", "F-03", "F-07", "F-09", "F-13", "F-12" };
static const char *const fault_short[F_N] = {
    "No Druck signal", "Excitation out of window", "Limit not found while calibrating",
    "Limit still active after clearance", "Motion timeout", "Operator STOP" };

static bool latched[F_N];
static uint32_t latched_ms[F_N];    // when each latched
static valve_mode_t latched_mode[F_N];
static bool cause_f01, cause_f03;   // from the last sensor read
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

static int32_t pct10_of(int32_t pos)
{
    return (int32_t)(((int64_t)pos * 1000 + (pos >= 0 ? pos_max / 2 : -pos_max / 2)) / pos_max);
}

static const char *pct_text(char *buf, size_t n, int32_t pos)
{
    return fixed_fmt(buf, n, pct10_of(pos), 1);
}

// The sensor faults (F-01 no Druck, F-03 excitation) do not stop or block
// the motor: manual moves and calibrating use the step count and the limit
// switch, not the pressure (Ryan 16 Oct). AUTO will need the reading.
static bool sensor_fault(fault_t f) { return f == F01 || f == F03; }

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
                  : any_latched() ? "fault latched, CTRL RESET first" : NULL);
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
    if (!sensor_fault(f)) {
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
    int32_t n = (int32_t)(((int64_t)pos_max * 11 + 5) / 10);
    cal_stage = CAL_FAST;
    if (!cal_move(stepper3_position() - n, f_home))
        raise(F07, true, "Homing: closing move refused at count %ld (%s)", (long)stepper3_position(),
              stepper3_refusal());
}

static void cal_creep(void)
{
    cal_stage = CAL_CREEP;
    if (!cal_move(stepper3_position() - (n_backoff + CREEP_EXTRA), f_creep))
        raise(F07, true, "Homing: creep move refused at count %ld", (long)stepper3_position());
}

static bool cal_start(const char **err)
{
    if (mode == VALVE_CALIBRATING) { *err = "already calibrating"; return false; }
    if (any_latched())             { *err = "fault latched, CTRL RESET first"; return false; }
    stepper3_stop();
    stepper3_override_cancel();
    question = false;
    cal_was = calibrated;
    calibrated = false;
    mode = VALVE_CALIBRATING;
    cal_t0 = lv_tick_get();
    update_lock();
    ev("REF", "Homing started at count %ld", (long)stepper3_position());
    if (limit_open_now()) {
        cal_stage = CAL_CLEAR;
        cal_move(stepper3_position() + n_clear, f_home);
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
        else
            cal_settle_from = lv_tick_get();   // then check the switch
            cal_settling = true;
        break;
    case CAL_FAST:
        if (why == STEPPER3_STOP_LIMIT) {
            cal_stage = CAL_BACK;
            cal_move(stepper3_position() + n_backoff, f_home);
        } else if (why == STEPPER3_STOP_DONE) {
            raise(F07, true, "Homing: switch not found within %ld steps",
                  (long)(((int64_t)pos_max * 11 + 5) / 10));
        } else {
            cal_abort(stepper3_stop_name(why));
        }
        break;
    case CAL_CREEP:
        if (why == STEPPER3_STOP_LIMIT) {
            if (!stepper3_set_position(n_offset)) {
                cal_abort("count not set");
                break;
            }
            calibrated = true;
            cal_done_ms = lv_tick_get();
            cal_done = true;
            cal_took_s = lv_tick_elaps(cal_t0) / 1000;
            lv_strlcpy(cal_done_iso, rtclock_iso(), sizeof cal_done_iso);
            ev("REF", "Homed in %lu s, position %ld", (unsigned long)cal_took_s, (long)n_offset);
            cal_end(VALVE_MANUAL);
        } else if (why == STEPPER3_STOP_DONE) {
            raise(F07, true, "Homing: switch not found on the creep close");
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
    int32_t moved = cal_stage == CAL_CLEAR ? n_clear : n_backoff;
    if (limit_open_now()) {
        raise(F09, true, "Switch still active after %ld-step %s", (long)moved,
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
    seq_seen = stepper3_move_seq();
    stepper3_on_reference_change(valve_uncalibrate);
    ev("F-14", "Power-up (event only), valve not homed");
    update_lock();
}

void valve_task(void)
{
    evt_dump_task();
    uint32_t seq = stepper3_move_seq();
    if (seq != seq_seen) {
        seq_seen = seq;
        stepper3_stop_t why = stepper3_last_stop();
        if (why == STEPPER3_STOP_TIMEOUT)
            raise(F13, false, "Motion timeout at count %ld", (long)stepper3_position());
        else if (mode == VALVE_CALIBRATING && cal_moving)
            cal_move_ended(why);
        else if (why == STEPPER3_STOP_LIMIT)
            limit_hit();
    }
    if (mode == VALVE_CALIBRATING && cal_settling && lv_tick_elaps(cal_settle_from) >= SETTLE_MS)
        cal_settled();
    bool open = limit_open_now();
    if (open && !switch_was_open)
        switch_at = stepper3_position();
    switch_was_open = open;
}

void valve_sense(const clicks_state_t *s)
{
    if (s->pm_diag)
        return;
    bool exc_bad = s->pm_found && (s->boost_tripped || s->bus_mv < EXC_MIN_MV || s->bus_mv > EXC_MAX_MV);
    cause_f03 = exc_bad;
    cause_f01 = !s->pm_found || (!exc_bad && !s->reading_ok);
    sensed = true;
    if (cause_f01)
        raise(F01, false, s->pm_found ? "No Druck signal (%s)" : "No Druck signal (Power Monitor not answering)%s",
              s->pm_found ? (s->druck_absent ? "outside 0.005-11 mV/V" : "no valid reading") : "");
    if (cause_f03) {
        if (s->boost_tripped)
            raise(F03, false, "Excitation supply tripped at %ld mV", (long)s->boost_trip_mv);
        else
            raise(F03, false, "Excitation %ld mV, outside 9500-10500 mV", (long)s->bus_mv);
    }
}

void valve_uncalibrate(const char *why)
{
    if (mode == VALVE_CALIBRATING) {
        cal_abort(why);
        cal_was = false;
    }
    if (!calibrated)
        return;
    calibrated = false;
    question = false;
    mode = VALVE_NOT_CAL;
    ev("REF", "Homing cleared (%s)", why);
    update_lock();
}

bool valve_restore(int32_t count, const char **why)
{
    if (mode == VALVE_CALIBRATING) { *why = "calibrating"; return false; }
    if (calibrated)                 { *why = "already calibrated"; return false; }
    if (any_latched())              { *why = "fault latched, CTRL RESET first"; return false; }
    if (stepper3_busy() || !stepper3_set_position(count)) { *why = "motor busy"; return false; }
    calibrated = true;
    mode = VALVE_MANUAL;
    char p[16];
    ev("REF", "Saved position used: count %ld (%s %%), not checked against the switch",
       (long)count, pct_text(p, sizeof p, count));
    update_lock();
    return true;
}

bool valve_set_closed(const char **why)
{
    if (mode == VALVE_CALIBRATING) { *why = "homing, STOP to cancel"; return false; }
    if (any_latched())              { *why = "fault latched, CTRL RESET first"; return false; }
    if (stepper3_busy() || !stepper3_set_position(0)) { *why = "moving, wait for it to stop"; return false; }
    calibrated = true;
    mode = VALVE_MANUAL;
    ev("REF", "Set as closed by the operator: count 0 (0 %%), not checked against the switch");
    update_lock();
    return true;
}

valve_mode_t valve_mode(void)   { return mode; }
bool valve_calibrated(void)     { return calibrated; }
int32_t valve_pos_max(void)     { return pos_max; }
int32_t valve_pct10(void)       { return pct10_of(stepper3_position()); }
bool valve_fault_latched(void)  { return any_latched(); }
bool valve_motion_blocked(void) { return any_latched(); }

const char *valve_mode_name(void)
{
    return mode == VALVE_CALIBRATING ? "HOMING" : mode == VALVE_MANUAL ? "MANUAL" : "NOT HOMED";
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
    if (calibrated)
        lv_snprintf(status_text, sizeof status_text, "Valve: %s, calibrated, %s %% (count %ld of %ld)%s%s%s",
                    valve_mode_name(), pct_text(p, sizeof p, stepper3_position()),
                    (long)stepper3_position(), (long)pos_max, f[0] ? ", faults " : "", f,
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
    if (f == F03) return sensed && !cause_f03;
    return true;
}

bool valve_stop_latches(void)
{
    return false;    // only in AUTO (stage 8); MANUAL has no AUTO to hold off
}

void valve_stop(void)
{
    bool was_cal = mode == VALVE_CALIBRATING;
    if (!valve_stop_latches()) {
        // Outside AUTO, STOP is an operating stop: stop and hold, nothing latched.
        stepper3_stop();
        seq_seen = stepper3_move_seq();
        cal_abort("STOP");
        ev("STOP", "Operator stop at count %ld%s", (long)stepper3_position(),
           was_cal ? ", calibrating cancelled" : "");
    } else if (latched[F12]) {
        // STOP always stops, even with F-12 already latched.
        stepper3_stop();
        cal_abort("STOP");
        ev("F-12", "STOP again at count %ld", (long)stepper3_position());
    } else {
        stepper3_stop();
        seq_seen = stepper3_move_seq();
        raise(F12, false, "Operator stop at count %ld%s", (long)stepper3_position(),
              was_cal ? ", calibrating cancelled" : "");
    }
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
    if (valve_settings_check(v) || mode == VALVE_CALIBRATING || stepper3_busy())
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
    if (!calibrated)                { *why = "valve not calibrated, VALVE CAL first"; return false; }
    int32_t to = (int32_t)(((int64_t)pct100 * pos_max + 5000) / 10000);
    if (to < 0) to = 0;
    if (to > pos_max) to = pos_max;
    if (!stepper3_goto(to))         { *why = stepper3_refusal(); return false; }
    return true;
}

bool valve_move_steps(int32_t steps, const char **why)
{
    if (mode == VALVE_CALIBRATING)  { *why = "calibrating, CTRL STOP to cancel"; return false; }
    if (any_latched())              { *why = "fault latched, CTRL RESET first"; return false; }
    int32_t from = stepper3_busy() ? stepper3_target() : stepper3_position();
    if (!stepper3_goto(from + steps)) { *why = stepper3_refusal(); return false; }
    return true;
}

bool valve_calibrate(const char **why) { return cal_start(why); }

uint32_t valve_cal_elapsed_s(void) { return mode == VALVE_CALIBRATING ? lv_tick_elaps(cal_t0) / 1000 : 0; }

uint32_t valve_cal_budget_s(void)
{
    return (uint32_t)(((int64_t)pos_max * 11 / 10 + n_clear + 2 * n_backoff) / f_home
                      + (n_backoff + CREEP_EXTRA) / f_creep + 1);
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
    return (int32_t)(((int64_t)pos * 100000 + (pos >= 0 ? pos_max / 2 : -pos_max / 2)) / pos_max);
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
    if (set_zero && stepper3_busy()) { *why = "moving, wait for it to stop"; return false; }
    question = false;
    if (set_zero) {
        int32_t d = stepper3_position() - question_at;
        stepper3_set_position(n_offset + d);
        switch_at += n_offset - question_at;
        ev("W-01", "Point set as 0 %%, count %ld", (long)n_offset);
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
