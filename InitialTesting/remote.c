#include <stdarg.h>
#include <string.h>

#include "lvgl.h"   // lv_snprintf, lv_vsnprintf
#include "pmc_config.h"
#include "link.h"
#include "fixed.h"
#include "clicks.h"
#include "rtclock.h"
#include "sysinfo.h"
#include "stepper3.h"
#include "sense.h"
#include "valve.h"
#include "remote.h"
#include "cfg.h"

/* --------------------------------------------------------------------------
 * The reply line: query replies and at most one refusal, joined by ';'.
 * ------------------------------------------------------------------------ */
static char out[256];
static size_t out_len;
static bool refused;

static void add(const char *fmt, va_list ap)
{
    if (out_len && out_len < sizeof out - 1)
        out[out_len++] = ';';
    if (out_len < sizeof out - 1) {
        lv_vsnprintf(out + out_len, sizeof out - out_len, fmt, ap);
        out_len += strlen(out + out_len);
    }
}

static void reply(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    add(fmt, ap);
    va_end(ap);
}

static void refuse(int err, const char *fmt, ...)
{
    char text[120];
    va_list ap;
    va_start(ap, fmt);
    lv_vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    if (text[0] >= 'a' && text[0] <= 'z')
        text[0] = (char)(text[0] - 'a' + 'A');   // the reasons are shared with the screen
    reply("ERR %d %s", err, text);
    refused = true;
}

/*
 * Error codes, from the refusal reasons the valve, the control loop and the
 * clock give (shared with the screen). The first match on the start of the
 * reason wins, case ignored.
 *   100 unknown command           201 homing, STOP to cancel
 *   101 expected: <the form>      202 switch to MANUAL first
 *   102 outside <low> to <high>   203 reset the fault first
 *   103 line too long             204 home the valve first
 *   104 EVENT? on its own line    205 no valid pressure
 *                                 206 set a setpoint first
 *                                 207 setpoint outside the sensor range
 *                                 208 not used (pressure above the sensor
 *                                     range, to 0.16.28)
 *                                 209 control settings not set
 *                                 210 not reset, cause still present
 *                                 211 read only over the network
 *                                 212 the valve cannot move now (the
 *                                     switch, a move running), or the
 *                                     clock is not running
 *                                 213 controller off or shutting down
 *                                     (0.15.0): turned on at the screen
 */
static const struct { const char *head; int num; const char *text; } why_codes[] = {
    { "Homing", 201, "Homing, STOP to cancel" },
    { "Switch to MANUAL", 202, "Switch to MANUAL first" },
    { "Stop AUTO", 202, "Switch to MANUAL first" },   // the screen's wording from 0.15.3
    { "Reset the fault", 203, "Reset the fault first" },
    { "Home the valve", 204, "Home the valve first" },
    { "No valid pressure", 205, "No valid pressure" }, { "No pressure", 205, "No valid pressure" },
    { "Set a setpoint", 206, NULL },
    { "Setpoint outside", 207, NULL },
    { "Pressure response", 209, "Control settings not set" },
    { "Tuning not set", 209, "Control settings not set" },
    { "Controller is off", 213, "Controller is off" },
    { "Shutting down", 213, "Shutting down, STOP to cancel" },
    { "B2 must", 102, NULL },   // SCHED SET boundaries (0.15.15)
    { "B3 must", 102, NULL },   // and the third (0.16.14)
    { "Expected", 101, NULL },
    { "Outside", 102, NULL },
};

// The handover page's text for a listed code (the screen's own wording can
// differ); the reason itself for 101, 102 and 212.
static void refuse_why(const char *why)
{
    for (size_t i = 0; i < sizeof why_codes / sizeof why_codes[0]; i++) {
        size_t n = strlen(why_codes[i].head), k = 0;
        while (k < n && why[k] && (why[k] | 0x20) == (why_codes[i].head[k] | 0x20))
            k++;
        if (k == n) {
            refuse(why_codes[i].num, "%s", why_codes[i].text ? why_codes[i].text : why);
            return;
        }
    }
    refuse(212, "%s", why);
}

/* --------------------------------------------------------------------------
 * Queries
 * ------------------------------------------------------------------------ */

// ALARM?: faults first (tag, whether ALARM RESET clears it now, text), then
// the warnings: 1 for a warning ALARM OK clears, 0 while its cause is there.
// The parts join with '|'; a '|' or ';' in a text would split the reply, so
// it becomes ','.
static void alarm_query(void)
{
    char a[200];
    size_t n = 0;
    valve_fault_t f;
    int32_t at;
    a[0] = '\0';
#define PART(tag, ok, text) do { \
        char t_[80]; lv_strlcpy(t_, text, sizeof t_); \
        for (char *c_ = t_; *c_; c_++) if (*c_ == '|' || *c_ == ';') *c_ = ','; \
        n += (size_t)lv_snprintf(a + n, n < sizeof a ? sizeof a - n : 0, "%s%s,%d,%s", n ? "|" : "", tag, ok, t_); \
    } while (0)
    for (int i = 0; valve_fault_get(i, &f) && n < sizeof a; i++)
        PART(f.tag, f.can_reset ? 1 : 0, f.text);
    if (valve_question(&at))
        PART("W-1", 0, "Limit-switch question waiting at the screen");
    const char *w = valve_warning();   // "W-2 Not settling: ...": the tag is the first word
    if (w && n < sizeof a) {
        char tag[8];
        size_t k = strcspn(w, " ");
        if (k >= sizeof tag)
            k = sizeof tag - 1;
        memcpy(tag, w, k);
        tag[k] = '\0';
        PART(tag, 1, w[k] ? w + k + 1 : "");
    }
#undef PART
    if (n)
        reply("ALARM %s", a);
    else
        reply("ALARM NONE");
}

static void idn_query(void)
{
    reply("MercShore,iPC,%s,%s", sysinfo_chip_id(), PMC_FW_VERSION);
}

// One query; false if it is not one of the set.
static bool query(const char *c)
{
    const clicks_state_t *s = clicks_state();
    char v[24];
    int32_t t;

    if (strcmp(c, "PRESSURE?") == 0) {
        if (s->reading_ok)
            reply("PRESSURE %s mbar", fixed_fmt(v, sizeof v, s->p_mmbar, 3));
        else
            reply("PRESSURE NONE");
    } else if (strcmp(c, "SETPOINT?") == 0) {
        if (valve_target(&t))
            reply("SETPOINT %s mbar", fixed_fmt(v, sizeof v, fixed_round(t, 2), 1));
        else
            reply("SETPOINT NONE");
    } else if (strcmp(c, "MODE?") == 0) {
        reply("MODE %s", valve_mode_name());
    } else if (strcmp(c, "VALVE?") == 0) {
        if (valve_reference() != VALVE_REF_NONE)
            reply("VALVE %s %%", fixed_fmt(v, sizeof v, fixed_round(valve_pct_milli(), 1), 2));
        else
            reply("VALVE NONE");
    } else if (strcmp(c, "VALVE MOVING?") == 0) {
        reply("VALVE MOVING %d", stepper3_busy() || valve_mode() == VALVE_CALIBRATING ? 1 : 0);
    } else if (strcmp(c, "BAROMETER?") == 0) {
        if (s->baro_ok)
            reply("BAROMETER %s mbar", fixed_fmt(v, sizeof v, fixed_round(s->baro_mmbar, 1), 2));
        else
            reply("BAROMETER NONE");
    } else if (strcmp(c, "ALARM?") == 0) {
        alarm_query();
    } else if (strcmp(c, "TIME?") == 0) {
        if (rtclock_valid())
            reply("TIME %s", rtclock_iso());
        else
            reply("TIME NONE");
    } else if (strcmp(c, "*IDN?") == 0) {
        idn_query();
    } else if (strcmp(c, "PIDS?") == 0) {
        // The set in use, then sets 1 to 4 (set 4 from 0.16.14).
        char g[4][3][16];
        for (int n = 1; n <= 4; n++)
            for (int w = 0; w < 3; w++)
                fixed_fmt(g[n - 1][w], sizeof g[0][0], cfg_tune(cfg_pid_key(n, w)), 2);
        reply("PIDS %d,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s", valve_pid_set(), g[0][0], g[0][1], g[0][2],
              g[1][0], g[1][1], g[1][2], g[2][0], g[2][1], g[2][2], g[3][0], g[3][1], g[3][2]);
    } else if (strcmp(c, "SCHED?") == 0) {
        char b1[16], b2[16], b3[16];
        reply("SCHED %s,%s,%s,%s,%ld", cfg_tune(CFG_SCHED) == CTRL_SCHED_PV ? "PV" : "OFF",
              fixed_fmt(b1, sizeof b1, cfg_tune(CFG_B1), 1), fixed_fmt(b2, sizeof b2, cfg_tune(CFG_B2), 1),
              fixed_fmt(b3, sizeof b3, cfg_tune(CFG_B3), 1), (long)cfg_tune(CFG_HYST));
    } else {
        return false;
    }
    return true;
}

/* --------------------------------------------------------------------------
 * Set commands: silent unless refused.
 * ------------------------------------------------------------------------ */

// "<number>" and nothing after it, dec decimals kept (more are cut).
static bool number(const char *s, int dec, int64_t *v)
{
    if (!fixed_parse(&s, dec, v))
        return false;
    while (*s == ' ')
        s++;
    return *s == '\0';
}

// PIDS SET <set>,<kp>,<ti>[,<kd>] (for a PC gain schedule): what the PID
// tab's Save does, so it works in AUTO and the loop takes the gains at its
// next step (cfg_save_with, tuning only). KP %/mB, TI min, KD %/(mB/s), 2 dp.
// Each change is a settings save in flash: unchanged gains write nothing.
// PID? and PID SET (the set in use) were removed in 0.16.29.
static void pid_set(const char *c)
{
    static const char *const nm[3] = { "KP", "TI", "KD" };
    const char *form = "Expected: PIDS SET <set>,<kp>,<ti>[,<kd>]";
    int32_t v[CFG_N] = { 0 };
    bool on[CFG_N] = { false }, same = true;
    char buf[LINK_LINE_MAX + 1], a[16], b[16];
    int n = 0, set;
    lv_strlcpy(buf, c, sizeof buf);
    char *p = buf, *e;
    int64_t x;
    e = strchr(p, ',');
    if (!e) {
        refuse(101, form);
        return;
    }
    *e = '\0';
    if (!number(p, 0, &x)) {
        refuse(101, form);
        return;
    }
    if (x < 1 || x > 4) {
        refuse(102, "Outside 1 to 4 for the set");
        return;
    }
    set = (int)x;
    p = e + 1;
    for (; n <= 3; p = e + 1, n++) {
        e = strchr(p, ',');
        if (e)
            *e = '\0';
        if (n == 3 || !number(p, 2, &x)) {
            refuse(101, form);
            return;
        }
        const cfg_key_t key = cfg_pid_key(set, n);
        int32_t lo, hi;
        cfg_range(key, &lo, &hi);
        if (x < lo || x > hi) {
            refuse(102, "Outside %s to %s for %s", fixed_fmt(a, sizeof a, lo, 2), fixed_fmt(b, sizeof b, hi, 2), nm[n]);
            return;
        }
        v[key] = (int32_t)x;
        on[key] = true;
        same = same && x == cfg_tune(key);
        if (!e)
            break;
    }
    if (n < 1) {   // one number: KP alone is not the form
        refuse(101, form);
        return;
    }
    const char *why;
    if (!same && !cfg_save_with(v, on, &why))
        refuse_why(why);
}

// SCHED SET PV,<b1>,<b2>,<b3>,<h> (0.15.15; SP removed in 0.15.20; b3, the
// boundary to set 4, from 0.16.14; OFF refused from 0.16.20, the PID table
// always on, Ryan 12 Nov): the PID table, as the PID tuning PID table tab's
// Save. Boundaries in mbar (1 dp), hysteresis %.
static void sched_set(const char *c)
{
    static const char *const form = "Expected: SCHED SET PV,<b1>,<b2>,<b3>,<h>";
    static const cfg_key_t key[5] = { CFG_SCHED, CFG_B1, CFG_B2, CFG_B3, CFG_HYST };
    static const char *const nm[5] = { "", "B1", "B2", "B3", "H" };
    static const int dec[5] = { 0, 1, 1, 1, 0 };
    int32_t v[CFG_N] = { 0 };
    bool on[CFG_N] = { false }, same = true;
    char buf[LINK_LINE_MAX + 1], a[16], b[16];
    lv_strlcpy(buf, c, sizeof buf);
    char *p = buf, *e = NULL;
    for (int n = 0; n < 5; n++, p = e + 1) {
        e = strchr(p, ',');
        if ((n < 4) != (e != NULL)) {
            refuse(101, form);
            return;
        }
        if (e)
            *e = '\0';
        int64_t x;
        if (n == 0) {
            while (*p == ' ')
                p++;
            char *q = p + strlen(p);
            while (q > p && q[-1] == ' ')
                *--q = '\0';
            if (strcmp(p, "OFF") == 0) {
                refuse(102, "The PID table is always on: give PV");
                return;
            }
            if (strcmp(p, "PV") == 0) x = CTRL_SCHED_PV;
            else {
                refuse(101, form);
                return;
            }
        } else {
            if (!number(p, dec[n], &x)) {
                refuse(101, form);
                return;
            }
            int32_t lo, hi;
            cfg_range(key[n], &lo, &hi);
            if (x < lo || x > hi) {
                refuse(102, "Outside %s to %s for %s", fixed_fmt(a, sizeof a, lo, dec[n]),
                       fixed_fmt(b, sizeof b, hi, dec[n]), nm[n]);
                return;
            }
        }
        v[key[n]] = (int32_t)x;
        on[key[n]] = true;
        same = same && x == cfg_tune(key[n]);
    }
    const char *why;
    if (!same && !cfg_save_with(v, on, &why))
        refuse_why(why);
}

// What follows the command words (a space and the value, or ""), or NULL if
// the line does not start with them as whole words.
static const char *after(const char *c, const char *words)
{
    const size_t n = strlen(words);
    if (strncmp(c, words, n) != 0 || (c[n] != ' ' && c[n] != '\0'))
        return NULL;
    return c + n;
}

static bool set(const char *c)
{
    const char *why, *v_text;
    int64_t v;
    char a[16];

    if ((v_text = after(c, "SETPOINT")) != NULL) {
        int32_t fs = valve_range_mmbar();
        if (!number(v_text, 3, &v))
            refuse(101, "Expected: SETPOINT <mbar>");
        else if (v < 0 || v > fs)
            refuse(102, "Outside 0 to %s mbar", fixed_fmt(a, sizeof a, fixed_round(fs, 2), 1));
        else if (!valve_set_target((int32_t)v, &why))
            refuse_why(why);
    } else if (strcmp(c, "MODE AUTO") == 0) {
        if (!valve_start_auto(&why))
            refuse_why(why);
    } else if (strcmp(c, "MODE MANUAL") == 0) {
        // Outside AUTO there is nothing to end; the valve stays where it is.
        // Off or shutting down it is refused: only the screen turns it on.
        if (valve_off_why())
            refuse_why(valve_off_why());
        else if (valve_auto() && !valve_to_manual(&why))
            refuse_why(why);
    } else if (after(c, "MODE")) {
        refuse(101, "Expected: MODE AUTO or MODE MANUAL");
    } else if ((v_text = after(c, "VALVE SET")) != NULL) {
        if (!number(v_text, 1, &v))
            refuse(101, "Expected: VALVE SET <%%>");
        else if (v < 0 || v > 1000)
            refuse(102, "Outside 0 to 100 %%");
        else if (!valve_goto_pct10((int32_t)v, &why))
            refuse_why(why);
    } else if (strcmp(c, "STOP") == 0) {
        valve_stop();
    } else if (strcmp(c, "ALARM RESET") == 0) {
        char left[40];
        valve_reset(left, sizeof left);
        if (left[0])
            refuse(210, "Not reset, cause still present: %s", left);
    } else if (strcmp(c, "ALARM OK") == 0) {
        // The screen OK and the Modbus OK coil: clear the strip warning.
        const char *w = valve_warning();
        if (w) {
            valve_event("OK", "Warning OK (link): %s", w);
            valve_warning_clear();
        }
    } else if ((v_text = after(c, "PIDS SET")) != NULL) {
        pid_set(v_text);
    } else if ((v_text = after(c, "SCHED SET")) != NULL) {
        sched_set(v_text);
    } else if ((v_text = after(c, "TIME SET")) != NULL) {
        why = rtclock_set_text(v_text[0] ? v_text + 1 : "");
        if (why && strncmp(why, "Expected", 8) == 0)
            refuse(101, "Expected: TIME SET <YYYY-MM-DD> <HH:MM:SS>");
        else if (why)
            refuse_why(why);
    } else {
        return false;
    }
    return true;
}


/* --------------------------------------------------------------------------
 * A line: the parts in order, one reply line.
 * ------------------------------------------------------------------------ */
static void part(const char *c, bool chained)
{
    const char *count = after(c, "EVENT?");
    if (count) {
        int64_t n = 1;
        if (chained)
            refuse(104, "EVENT? must be on its own line");
        else if (count[0] && !number(count, 0, &n))
            refuse(101, "Expected: EVENT? [<n>]");
        else if (n < 1 || n > 100)
            refuse(102, "Outside 1 to 100");
        else
            valve_events_send((int)n);   // the count line now, the events as the link has room
        return;
    }
    if (query(c))
        return;
    // Set commands (STOP included) need the Remote control switch On
    // (0.13.4). Only the queries above are free: a line that merely ends in
    // '?' (CFG NAME X?) is not one.
    if (!cfg_remote()) {
        refuse(211, "Read only over the network");
        return;
    }
    if (set(c))
        return;
    refuse(100, "Unknown command");
}

void remote_line(const char *line)
{
    link_count(false);   // the header's remote light (link_write counts the reply)
    out_len = 0;
    out[0] = '\0';
    refused = false;
    if (strcmp(line, LINK_LINE_LONG) == 0) {
        refuse(103, "Line too long");
    } else {
        bool chained = strchr(line, ';') != NULL;
        const char *p = line;
        while (*p && !refused) {
            char c[LINK_LINE_MAX + 1];
            size_t n = strcspn(p, ";");
            size_t a = 0, b = n;
            while (a < b && p[a] == ' ')
                a++;
            while (b > a && p[b - 1] == ' ')
                b--;
            if (b > a) {
                memcpy(c, p + a, b - a);
                c[b - a] = '\0';
                part(c, chained);
            }
            p += n;
            if (*p == ';')
                p++;
        }
    }
    if (out_len) {
        link_write(out);
        link_write("\r\n");
    }
}
