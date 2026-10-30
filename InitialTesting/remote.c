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
#if PMC_LINK_ETHERNET
#include "ethlink.h"
#endif
#if PMC_ENG_COMMANDS
#include "cal.h"
#include "limit.h"
#include "trend.h"
#include "valvepos.h"
#include "baro.h"
#include "lv_port_indev.h"
bool perf_command(const char *line);   // main.c
#endif

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
 *   104 EVNT? on its own line     205 no valid pressure
 *                                 206 set a target first
 *                                 207 target outside the sensor range
 *                                 208 pressure above the sensor range
 *                                 209 control settings not set
 *                                 210 not reset, cause still present
 *                                 211 read only over the network
 *                                 212 the valve cannot move now (the
 *                                     switch, a move or the step test
 *                                     running), or the
 *                                     clock is not running
 */
static const struct { const char *head; int num; const char *text; } why_codes[] = {
    { "Homing", 201, "Homing, STOP to cancel" },
    { "Switch to MANUAL", 202, "Switch to MANUAL first" },
    { "Reset the fault", 203, "Reset the fault first" },
    { "Home the valve", 204, "Home the valve first" },
    { "No valid pressure", 205, "No valid pressure" }, { "No pressure", 205, "No valid pressure" },
    { "Set a setpoint", 206, "Set a target first" },
    { "Setpoint outside", 207, "Target outside the sensor range" },
    { "Pressure above", 208, "Pressure above the sensor range" },
    { "Pressure response", 209, "Control settings not set" },
    { "Tuning not set", 209, "Control settings not set" },
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

// ALRM?: faults first (tag, whether ALRM RESET clears it now, text), then
// the warnings: 1 for a warning ALRM OK clears, 0 while its cause is there.
// The parts join with '|'; a '|' or ';' in a text would split the reply, so
// it becomes ','.
static void alarm_query(void)
{
    char a[200];
    size_t n = 0;
    valve_fault_t f;
    int32_t at, mc;
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
    if (sense_hot(&mc) && n < sizeof a)
        PART("W-3", 0, "Controller hot");
#undef PART
    if (n)
        reply("ALRM %s", a);
    else
        reply("ALRM NONE");
}

static void idn_query(void)
{
    reply("MercShore,iPC,%s,%s", sysinfo_chip_id(), PMC_FW_VERSION);
}

static void netw_query(void)
{
#if PMC_LINK_ETHERNET
    if (ethlink_ip()[0])
        reply("NETW %s,%s,%s,%s", ethlink_fallback() ? "FALLBACK" : "DHCP", ethlink_ip(), ethlink_mask(),
              ethlink_mac()[0] ? ethlink_mac() : "NONE");
    else
        reply("NETW NONE");
#else
    reply("NETW NONE");   // the USB build: no Ethernet
#endif
}

// One query; false if it is not one of the set.
static bool query(const char *c)
{
    const clicks_state_t *s = clicks_state();
    char v[24];
    int32_t t;

    if (strcmp(c, "PRES?") == 0) {
        if (s->reading_ok)
            reply("PRES %s mbar", fixed_fmt(v, sizeof v, s->p_mmbar, 3));
        else
            reply("PRES NONE");
    } else if (strcmp(c, "PRES SP?") == 0) {
        if (valve_target(&t))
            reply("PRES SP %s mbar", fixed_fmt(v, sizeof v, fixed_round(t, 2), 1));
        else
            reply("PRES SP NONE");
    } else if (strcmp(c, "MODE?") == 0) {
        reply("MODE %s", valve_mode_name());
    } else if (strcmp(c, "VALV?") == 0) {
        if (valve_reference() != VALVE_REF_NONE)
            reply("VALV %s %%", fixed_fmt(v, sizeof v, fixed_round(valve_pct_milli(), 1), 2));
        else
            reply("VALV NONE");
    } else if (strcmp(c, "VALV MOVING?") == 0) {
        reply("VALV MOVING %d", stepper3_busy() || valve_mode() == VALVE_CALIBRATING ? 1 : 0);
    } else if (strcmp(c, "BARO?") == 0) {
        if (s->baro_ok)
            reply("BARO %s mbar", fixed_fmt(v, sizeof v, fixed_round(s->baro_mmbar, 1), 2));
        else
            reply("BARO NONE");
    } else if (strcmp(c, "ALRM?") == 0) {
        alarm_query();
    } else if (strcmp(c, "NETW?") == 0) {
        netw_query();
    } else if (strcmp(c, "TIME?") == 0) {
        if (rtclock_valid())
            reply("TIME %s", rtclock_iso());
        else
            reply("TIME NONE");
    } else if (strcmp(c, "*IDN?") == 0) {
        idn_query();
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

static bool set(const char *c)
{
    const char *why;
    int64_t v;
    char a[16];

    if (strncmp(c, "PRES SP ", 8) == 0) {
        int32_t fs = valve_range_mmbar();
        if (!number(c + 8, 3, &v))
            refuse(101, "Expected: PRES SP <mbar>");
        else if (v < 0 || v > fs)
            refuse(102, "Outside 0 to %s mbar", fixed_fmt(a, sizeof a, fixed_round(fs, 2), 1));
        else if (!valve_set_target((int32_t)v, &why))
            refuse_why(why);
    } else if (strcmp(c, "MODE AUTO") == 0) {
        if (!valve_start_auto(&why))
            refuse_why(why);
    } else if (strcmp(c, "MODE MANUAL") == 0) {
        // Outside AUTO there is nothing to end; the valve stays where it is.
        if (valve_auto() && !valve_to_manual(&why))
            refuse_why(why);
    } else if (strncmp(c, "MODE", 4) == 0 && (c[4] == ' ' || c[4] == '\0')) {
        refuse(101, "Expected: MODE AUTO or MODE MANUAL");
    } else if (strncmp(c, "VALV SET", 8) == 0 && (c[8] == ' ' || c[8] == '\0')) {
        if (!number(c + 8, 1, &v))
            refuse(101, "Expected: VALV SET <%%>");
        else if (v < 0 || v > 1000)
            refuse(102, "Outside 0 to 100 %%");
        else if (!valve_goto_pct10((int32_t)v, &why))
            refuse_why(why);
    } else if (strcmp(c, "STOP") == 0) {
        valve_stop();
    } else if (strcmp(c, "ALRM RESET") == 0) {
        char left[40];
        valve_reset(left, sizeof left);
        if (left[0])
            refuse(210, "Not reset, cause still present: %s", left);
    } else if (strcmp(c, "ALRM OK") == 0) {
        // The screen OK and the Modbus OK coil: clear the strip warning.
        const char *w = valve_warning();
        if (w) {
            valve_event("OK", "Warning OK (link): %s", w);
            valve_warning_clear();
        }
    } else if (strncmp(c, "TIME SET", 8) == 0 && (c[8] == ' ' || c[8] == '\0')) {
        why = rtclock_set_text(c[8] ? c + 9 : "");
        if (why && strncmp(why, "Expected", 8) == 0)
            refuse(101, "Expected: TIME SET <YYYY-MM-DD> <HH:MM:SS>");
        else if (why)
            refuse_why(why);
    } else {
        return false;
    }
    return true;
}

#if PMC_ENG_COMMANDS
// HELP in a bench build: the engineering commands (the remote set is in
// remote.h and README.md).
static bool remote_help(const char *c)
{
    static const char *const lines[] = {
        "Engineering commands (bench build only; old names, replies end in OK or ERR):",
        "  VALVE? | VALVE CAL | VALVE CLOSED | VALVE SETZERO | VALVE SKIP | VALVE SAVED? | VALVE RESTORE",
        "  VALVE POSMAX|FHOME|FCREEP|BACKOFF|CLEAR|OFFSET <n> | CTRL? | TUNE STEP [<+-%>] | TUNE? | TUNE STOP",
        "  PID? | PID KP|TI|OUTMIN|DB|BAND <v> | PID KDIR +1|-1 | CAL? | CAL VAC [mbar] | CAL AIR [mbar]",
        "  CAL SAVE | CAL ERASE | CAL NEW [id] | CAL PT <mV/V> <mbar> | CAL APPLY",
        "  CFG? | CFG DIFF? | CFG SAVE | CFG LOAD LAST|PREVIOUS|DEFAULTS | CFG NAME? | CFG NAME <text>",
        "  CFG TEST CORRUPT|ERASE | NET? | NET IP <address> <mask> | NET IP DEFAULT (USB build)",
        "  MOT? | MOT GOTO <pos> [sps] | MOT MOVE <steps> | MOT STOP [RAMP] | MOT OFF | MOT ZERO",
        "  MOT OVERRIDE [OFF] | MOT RATE|START|ACCEL <n> | MOT MODE WAVE|FULL|HALF | MOT HOLD|DIR 0|1",
        "  MOT ORDER <abcd> | LIM? | SENSE? | BARO STATUS? | TREND? | TREND DUMP <n> | VER? | TOUCH?",
    };
    if (strcmp(c, "HELP") != 0)
        return false;
    for (size_t i = 0; i < sizeof lines / sizeof lines[0]; i++)
        link_printf("%s\r\n", lines[i]);
    link_printf("OK\r\n");
    return true;
}

// The engineering commands (bench build, simulator): old names, own replies.
static bool eng(const char *c)
{
    link_eng_open(true);
    bool done = valve_command(c) || cfg_command(c) || cal_command(c) || sysinfo_command(c) ||
                baro_command(c) || trend_command(c) || limit_command(c) || sense_data_command(c) ||
                touch_command(c) || perf_command(c) || valvepos_command(c) || stepper3_command(c) ||
                remote_help(c);
    link_eng_open(false);
    return done;
}
#endif

/* --------------------------------------------------------------------------
 * A line: the parts in order, one reply line.
 * ------------------------------------------------------------------------ */
static void part(const char *c, bool chained)
{
    bool is_query = c[strlen(c) - 1] == '?';
    if (strcmp(c, "EVNT?") == 0 || strncmp(c, "EVNT? ", 6) == 0) {
        int64_t n = 1;
        if (chained)
            refuse(104, "EVNT? must be on its own line");
        else if (c[5] && !number(c + 6, 0, &n))
            refuse(101, "Expected: EVNT? [<n>]");
        else if (n < 1 || n > 100)
            refuse(102, "Outside 1 to 100");
        else
            valve_events_send((int)n);   // the count line now, the events as the link has room
        return;
    }
    if (query(c))
        return;
    // Over the network, set commands (STOP included) need PMC_LINK_WRITE 1
    // and the Remote control switch On (0.13.4); a USB link is local.
    if (PMC_REMOTE_SWITCH && !is_query && (!PMC_LINK_WRITE || !cfg_remote())) {
        refuse(211, "Read only over the network");
        return;
    }
    if (set(c))
        return;
#if PMC_ENG_COMMANDS
    if (eng(c))
        return;
#endif
    refuse(100, "Unknown command");
}

void remote_line(const char *line)
{
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
