/*
 * Host test of the Modbus TCP server: modbus.c (protocol) and mbmap.c (the
 * register map) against stubs of the valve, sensor and motor calls.
 *   test_modbus            runs the frame tests
 *   test_modbus serve N    also answers Modbus TCP on 127.0.0.1:N, for
 *                          client.py (a real client library on the other end)
 * run.sh builds it twice: writes off and writes on (the default).
 */
#include <arpa/inet.h>
#include <math.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "pmc_config.h"
#include "clicks.h"
#include "modbus.h"
#include "valve.h"
#include "cfg.h"

/* ---- stubs ------------------------------------------------------------- */

static clicks_state_t cs = { .pm_found = true, .reading_ok = true, .bus_mv = 10012,
                             .p_mmbar = 4625, .die_mc = 31500 };
static valve_mode_t mode = VALVE_MANUAL;
static int32_t sp = 0;
static bool sp_set;
static int32_t pos_milli = 14200, goto_milli = 14200;
static const char *warn;
static bool question, fault_f6, busy, limit_open;
static int resets, stops;
static char last_event[160];
static bool remote = true;   // the Remote control switch (0.13.4)

const clicks_state_t *clicks_state(void) { return &cs; }
valve_mode_t valve_mode(void) { return mode; }
bool valve_auto(void) { return mode == VALVE_AUTO; }
bool valve_calibrated(void) { return mode >= VALVE_MANUAL; }
valve_ref_t valve_reference(void) { return VALVE_REF_HOMED; }
bool valve_target(int32_t *m) { *m = sp; return sp_set; }
int32_t valve_range_mmbar(void) { return 1034000; }
ctrl_reg_t valve_regulation(void) { return CTRL_REG_BAND; }
bool valve_fault_is(const char *t) { return fault_f6 && !strcmp(t, "F-6"); }
int valve_fault_count(void) { return fault_f6; }
uint32_t valve_event_total(void) { return 70000; }
const char *valve_warning(void) { return warn; }
void valve_warning_clear(void) { warn = NULL; }
bool valve_question(int32_t *p) { (void)p; return question; }
int32_t valve_pct_milli(void) { return pos_milli; }
int32_t valve_target_pct_milli(void) { return goto_milli; }
bool stepper3_busy(void) { return busy; }
bool stepper3_limit_open(void) { return limit_open; }
bool cfg_remote(void) { return remote; }

// The gain schedule settings (0.15.15; set 4 and B3 from 0.16.14): defaults,
// ranges and the boundary checks as cfg.c; saves counts the settings saves.
static int32_t tune[CFG_N];
static int saves, pid_set_now = 1;
static void tune_reset(void)
{
    static const cfg_key_t k[] = { CFG_KP, CFG_TI, CFG_KD, CFG_KP2, CFG_TI2, CFG_KD2, CFG_KP3, CFG_TI3, CFG_KD3,
                                   CFG_KP4, CFG_TI4, CFG_KD4, CFG_SCHED, CFG_B1, CFG_B2, CFG_B3, CFG_HYST };
    static const int32_t v[] = { 400, 25, 0, 400, 25, 0, 400, 25, 0, 400, 25, 0, 2, 60, 100, 140, 15 };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
        tune[k[i]] = v[i];
}
int32_t cfg_tune(cfg_key_t k) { return tune[k]; }
cfg_key_t cfg_pid_key(int set, int which)
{
    static const cfg_key_t key[4][3] = { { CFG_KP, CFG_TI, CFG_KD }, { CFG_KP2, CFG_TI2, CFG_KD2 },
                                         { CFG_KP3, CFG_TI3, CFG_KD3 }, { CFG_KP4, CFG_TI4, CFG_KD4 } };
    return key[set - 1][which];
}
void cfg_range(cfg_key_t k, int32_t *lo, int32_t *hi)
{
    *lo = 0;
    *hi = 10000;
    if (k == CFG_KP || k == CFG_KP2 || k == CFG_KP3 || k == CFG_KP4 || k == CFG_B1 || k == CFG_B2) *lo = 1;
    if (k == CFG_SCHED) *lo = *hi = 2;   // the PID table always on (0.16.20)
    if (k == CFG_HYST) *hi = 40;
}
bool cfg_save_with(const int32_t v[CFG_N], const bool set[CFG_N], const char **why)
{
    int32_t t[CFG_N];
    memcpy(t, tune, sizeof t);
    for (int k = 0; k < CFG_N; k++)
        if (set[k])
            t[k] = v[k];
    if ((int64_t)t[CFG_B1] * (100 + t[CFG_HYST]) >= (int64_t)t[CFG_B2] * (100 - t[CFG_HYST])) {
        *why = "B2 must be above B1, by more than the hysteresis either side";
        return false;
    }
    if (t[CFG_B3] != 0 && (int64_t)t[CFG_B2] * (100 + t[CFG_HYST]) >= (int64_t)t[CFG_B3] * (100 - t[CFG_HYST])) {
        *why = "B3 must be above B2, by more than the hysteresis either side, or 0 for three sets";
        return false;
    }
    memcpy(tune, t, sizeof t);
    saves++;
    return true;
}
int valve_pid_set(void) { return pid_set_now; }
uint32_t lv_tick_get(void) { return 123456789u; }   // 123456 s up
int lv_snprintf(char *b, size_t n, const char *fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    int k = vsnprintf(b, n, fmt, a);
    va_end(a);
    return k;
}
size_t lv_strlcpy(char *d, const char *s, size_t n)
{
    snprintf(d, n, "%s", s);
    return strlen(s);
}

bool valve_set_target(int32_t m, const char **why)
{
    if (fault_f6) { *why = "fault"; return false; }
    sp = (int32_t)(lround(m / 100.0) * 100);
    sp_set = true;
    return true;
}
bool valve_goto_pct100(int32_t p, const char **why)
{
    if (mode == VALVE_AUTO) { *why = "Switch to MANUAL first"; return false; }
    goto_milli = p * 10;
    return true;
}
bool valve_start_auto(const char **why)
{
    if (!sp_set) { *why = "No target set"; return false; }
    mode = VALVE_AUTO;
    return true;
}
bool valve_to_manual(const char **why) { (void)why; mode = VALVE_MANUAL; return true; }
void valve_stop(void) { stops++; if (mode == VALVE_AUTO) mode = VALVE_MANUAL; }
int valve_reset(char *left, size_t n) { resets++; fault_f6 = false; if (n) left[0] = 0; return 1; }
void valve_event(const char *tag, const char *fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    int k = snprintf(last_event, sizeof last_event, "%s ", tag);
    vsnprintf(last_event + k, sizeof last_event - k, fmt, a);
    va_end(a);
}

/* ---- frame helpers ---------------------------------------------------- */

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static uint8_t rsp[MODBUS_ADU_MAX];
static size_t rlen;

// Sends a PDU (given as bytes) with transaction id 0x1234, unit 0x11.
static void req(size_t n, const uint8_t *p)
{
    uint8_t in[MODBUS_ADU_MAX] = { 0x12, 0x34, 0, 0, (uint8_t)((n + 1) >> 8), (uint8_t)(n + 1), 0x11 };
    memcpy(in + 7, p, n);
    rlen = modbus_handle(in, 7 + n, rsp);
}
#define REQ(...) do { uint8_t _p[] = { __VA_ARGS__ }; req(sizeof _p, _p); } while (0)

static bool is_ex(uint8_t fc, uint8_t exc)
{
    return rlen == 9 && rsp[7] == (fc | 0x80) && rsp[8] == exc && rsp[0] == 0x12 && rsp[1] == 0x34 &&
           rsp[4] == 0 && rsp[5] == 3 && rsp[6] == 0x11;
}
static uint16_t r16(int i) { return (uint16_t)(rsp[9 + 2 * i] << 8 | rsp[10 + 2 * i]); }
static float rf(int i)
{
    uint32_t u = (uint32_t)r16(i) << 16 | r16(i + 1);
    float f;
    memcpy(&f, &u, 4);
    return f;
}
static void f32(float f, uint8_t *b)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    b[0] = u >> 24; b[1] = u >> 16; b[2] = u >> 8; b[3] = u;
}

static void tests(void)
{
    tune_reset();
    // Read the version 1 map.
    REQ(0x03, 0, 0, 0, 26);
    CHECK(rlen == 9 + 52 && rsp[7] == 3 && rsp[8] == 52 && rsp[5] == 3 + 52 && rsp[6] == 0x11);
    CHECK(r16(0) == 3);                         // map version (2 from 0.15.15, 3 from 0.16.14)
    CHECK(r16(1) == (uint16_t)123456);          // heartbeat
    CHECK(r16(2) == VALVE_MANUAL);
    CHECK(r16(3) & 1);                          // reading OK
    CHECK(((r16(3) >> 8) & 1) == 1);                // remote writes on
    CHECK(r16(8) == (uint16_t)70000);
    CHECK(fabsf(rf(10) - 4.625f) < 1e-6f);
    CHECK(isnan(rf(12)));                       // no target yet
    CHECK(fabsf(rf(14) - 14.2f) < 1e-5f);
    CHECK(fabsf(rf(18) - 1034.0f) < 1e-3f);
    CHECK(fabsf(rf(20) - 10.012f) < 1e-5f);
    CHECK(fabsf(rf(22) - 31.5f) < 1e-5f);
    CHECK(r16(24) == 1 && r16(25) == (uint16_t)(123456 - 65536));
    // Pressure alone, and no reading = NaN.
    cs.reading_ok = false;
    REQ(0x03, 0, 10, 0, 2);
    CHECK(rlen == 13 && isnan(rf(0)));
    cs.reading_ok = true;
    // The gain schedule (version 2, 0.15.15): set in use, type, B1, B2, H, the nine gains;
    // version 3 (0.16.14): set 4's gains (51-56) and B3 (57-58).
    pid_set_now = 4;
    REQ(0x03, 0, 0, 0, 59);
    CHECK(rlen == 9 + 118 && r16(26) == 4 && r16(27) == 2 && fabsf(rf(28) - 6.0f) < 1e-5f &&
          fabsf(rf(30) - 10.0f) < 1e-5f && r16(32) == 15);
    CHECK(fabsf(rf(33) - 4.0f) < 1e-5f && fabsf(rf(35) - 0.25f) < 1e-5f && rf(37) == 0.0f &&
          fabsf(rf(45) - 4.0f) < 1e-5f && fabsf(rf(49) - 0.0f) < 1e-5f);
    CHECK(fabsf(rf(51) - 4.0f) < 1e-5f && fabsf(rf(53) - 0.25f) < 1e-5f && rf(55) == 0.0f &&
          fabsf(rf(57) - 14.0f) < 1e-5f);
    pid_set_now = 1;
    // Past the end, zero count, too many.
    REQ(0x03, 0, 58, 0, 2);   CHECK(is_ex(3, MODBUS_EX_ADDRESS));
    REQ(0x03, 0, 0, 0, 0);    CHECK(is_ex(3, MODBUS_EX_VALUE));
    REQ(0x03, 0, 0, 0, 126);  CHECK(is_ex(3, MODBUS_EX_VALUE));
    REQ(0x03, 0xFF, 0xFF, 0, 2); CHECK(is_ex(3, MODBUS_EX_ADDRESS));
    REQ(0x03, 0, 0, 0);       CHECK(is_ex(3, MODBUS_EX_VALUE));   // short PDU
    // Unsupported functions.
    REQ(0x04, 0, 0, 0, 1);    CHECK(is_ex(4, MODBUS_EX_FUNCTION));
    REQ(0x06, 0, 12, 0, 1);   CHECK(is_ex(6, MODBUS_EX_FUNCTION));
    REQ(0x02, 0, 0, 0, 1);    CHECK(is_ex(2, MODBUS_EX_FUNCTION));
    // Coils: 0-3 commands, 16-25 status; 4-15 not in the map.
    limit_open = true;
    REQ(0x01, 0, 16, 0, 10);
    CHECK(rlen == 11 && rsp[8] == 2 && (rsp[9] & 0x01) && (rsp[9] & 0x10));
    REQ(0x01, 0, 0, 0, 4);    CHECK(rlen == 10 && rsp[9] == 0);
    REQ(0x01, 0, 0, 0, 5);    CHECK(is_ex(1, MODBUS_EX_ADDRESS));
    REQ(0x01, 0, 26, 0, 1);   CHECK(is_ex(1, MODBUS_EX_ADDRESS));
    limit_open = false;
    // Bad coil value.
    REQ(0x05, 0, 1, 0x12, 0x34); CHECK(is_ex(5, MODBUS_EX_VALUE));
    // Bad framing is not answered.
    uint8_t bad[] = { 0, 1, 0, 1, 0, 6, 1, 3, 0, 0, 0, 1 };   // protocol id 1
    CHECK(modbus_handle(bad, sizeof bad, rsp) == 0);
    CHECK(modbus_frame_len(bad, sizeof bad) == (size_t)-1);
    uint8_t part[] = { 0, 1, 0, 0, 0, 6, 1, 3, 0 };
    CHECK(modbus_frame_len(part, sizeof part) == 0);

    uint8_t w[] = { 0x10, 0, 12, 0, 2, 4, 0, 0, 0, 0 };
    f32(4.8f, w + 6);
    req(sizeof w, w);
    CHECK(rlen == 12 && rsp[7] == 0x10 && rsp[9] == 12 && rsp[11] == 2);
    CHECK(sp_set && sp == 4800);
    CHECK(strstr(last_event, "target 4.8 mbar") != NULL);
    REQ(0x03, 0, 12, 0, 2);   CHECK(fabsf(rf(0) - 4.8f) < 1e-5f);
    // Half a float, a read-only register, out of range, NaN.
    REQ(0x10, 0, 12, 0, 1, 2, 0x40, 0x99);       CHECK(is_ex(0x10, MODBUS_EX_ADDRESS));
    REQ(0x10, 0, 10, 0, 2, 4, 0x40, 0x99, 0, 0); CHECK(is_ex(0x10, MODBUS_EX_ADDRESS));
    f32(2000.0f, w + 6); req(sizeof w, w);       CHECK(is_ex(0x10, MODBUS_EX_VALUE));
    f32(NAN, w + 6); req(sizeof w, w);           CHECK(is_ex(0x10, MODBUS_EX_VALUE));
    // Byte count that does not match.
    REQ(0x10, 0, 12, 0, 2, 3, 0x40, 0x99, 0);    CHECK(is_ex(0x10, MODBUS_EX_VALUE));
    // The schedule in one write (27-32): PV, 6.5 and 11 mbar, 10 %; one save.
    {
        uint8_t q[] = { 0x10, 0, 27, 0, 6, 12, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 10 };
        f32(6.5f, q + 8); f32(11.0f, q + 12);
        int s0 = saves;
        req(sizeof q, q);
        CHECK(rlen == 12 && rsp[9] == 27 && rsp[11] == 6 && tune[CFG_SCHED] == 2 && tune[CFG_B1] == 65 &&
              tune[CFG_B2] == 110 && tune[CFG_HYST] == 10 && saves == s0 + 1);
        CHECK(strstr(last_event, "gain schedule") != NULL);
        req(sizeof q, q);   // the same again: nothing saved
        CHECK(rlen == 12 && saves == s0 + 1);
        // Set 3's gains (45-50) in AUTO terms: KP 1.2, TI 0.3, KD 0.05.
        uint8_t g[] = { 0x10, 0, 45, 0, 6, 12, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        f32(1.2f, g + 6); f32(0.3f, g + 10); f32(0.05f, g + 14);
        req(sizeof g, g);
        CHECK(rlen == 12 && tune[CFG_KP3] == 120 && tune[CFG_TI3] == 30 && tune[CFG_KD3] == 5 && tune[CFG_KP] == 400);
        REQ(0x03, 0, 45, 0, 2);   CHECK(fabsf(rf(0) - 1.2f) < 1e-5f);
        // Set 4's gains and B3 in one write (51-58, 0.16.14): KP 0.3, TI 0.25, KD 0, 16 mbar.
        uint8_t g4[] = { 0x10, 0, 51, 0, 8, 16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        f32(0.3f, g4 + 6); f32(0.25f, g4 + 10); f32(0.0f, g4 + 14); f32(16.0f, g4 + 18);
        s0 = saves;
        req(sizeof g4, g4);
        CHECK(rlen == 12 && rsp[9] == 51 && rsp[11] == 8 && tune[CFG_KP4] == 30 && tune[CFG_TI4] == 25 &&
              tune[CFG_KD4] == 0 && tune[CFG_B3] == 160 && saves == s0 + 1);
        REQ(0x03, 0, 57, 0, 2);   CHECK(fabsf(rf(0) - 16.0f) < 1e-5f);
        // B3 within the hysteresis of B2: refused; 0, three sets: taken.
        uint8_t b3[] = { 0x10, 0, 57, 0, 2, 4, 0, 0, 0, 0 };
        f32(11.5f, b3 + 6); req(sizeof b3, b3);      CHECK(is_ex(0x10, MODBUS_EX_REFUSED) && tune[CFG_B3] == 160);
        CHECK(strstr(last_event, "B3 must be above B2") != NULL);
        f32(0.0f, b3 + 6); req(sizeof b3, b3);       CHECK(rlen == 12 && tune[CFG_B3] == 0);
        // Out of range, a type past 2, 1 (the setpoint, removed) or 0 (Off, removed), half a float, the read-only set register, B2 under B1.
        uint8_t k0[] = { 0x10, 0, 33, 0, 2, 4, 0, 0, 0, 0 };
        f32(0.0f, k0 + 6); req(sizeof k0, k0);       CHECK(is_ex(0x10, MODBUS_EX_VALUE) && tune[CFG_KP] == 400);
        f32(NAN, k0 + 6); req(sizeof k0, k0);        CHECK(is_ex(0x10, MODBUS_EX_VALUE));
        REQ(0x10, 0, 27, 0, 1, 2, 0, 3);             CHECK(is_ex(0x10, MODBUS_EX_VALUE) && tune[CFG_SCHED] == 2);
        REQ(0x10, 0, 32, 0, 1, 2, 0, 41);            CHECK(is_ex(0x10, MODBUS_EX_VALUE));
        REQ(0x10, 0, 27, 0, 1, 2, 0, 1);             CHECK(is_ex(0x10, MODBUS_EX_VALUE) && tune[CFG_SCHED] == 2);   // 1, the setpoint, removed in 0.15.20
        REQ(0x10, 0, 27, 0, 1, 2, 0, 0);             CHECK(is_ex(0x10, MODBUS_EX_VALUE) && tune[CFG_SCHED] == 2);   // 0, Off, removed in 0.16.20
        REQ(0x10, 0, 28, 0, 1, 2, 0x40, 0xD0);       CHECK(is_ex(0x10, MODBUS_EX_ADDRESS));
        REQ(0x10, 0, 26, 0, 1, 2, 0, 3);             CHECK(is_ex(0x10, MODBUS_EX_ADDRESS));
        REQ(0x10, 0, 29, 0, 2, 4, 0, 0, 0, 0);       CHECK(is_ex(0x10, MODBUS_EX_ADDRESS));   // the second half of B1
        uint8_t b[] = { 0x10, 0, 30, 0, 2, 4, 0, 0, 0, 0 };
        f32(6.0f, b + 6); req(sizeof b, b);          CHECK(is_ex(0x10, MODBUS_EX_REFUSED) && tune[CFG_B2] == 110);
        CHECK(strstr(last_event, "B2 must be above B1") != NULL);
        tune_reset();
    }
    // Valve % in MANUAL.
    uint8_t v[] = { 0x10, 0, 16, 0, 2, 4, 0, 0, 0, 0 };
    f32(25.5f, v + 6); req(sizeof v, v);
    CHECK(rlen == 12 && goto_milli == 25500);
    // AUTO on, then valve % refused.
    REQ(0x05, 0, 0, 0xFF, 0);
    CHECK(rlen == 12 && rsp[10] == 0xFF && mode == VALVE_AUTO);
    REQ(0x01, 0, 0, 0, 1);   CHECK(rsp[9] == 1);
    req(sizeof v, v);        CHECK(is_ex(0x10, MODBUS_EX_REFUSED));
    CHECK(strstr(last_event, "Switch to MANUAL first") != NULL);
    // STOP drops to MANUAL; STOP 0 does nothing.
    REQ(0x05, 0, 1, 0, 0);   CHECK(rlen == 12 && stops == 0);
    REQ(0x05, 0, 1, 0xFF, 0); CHECK(stops == 1 && mode == VALVE_MANUAL);
    // RESET, and a target refused while a fault is latched.
    fault_f6 = true;
    REQ(0x03, 0, 5, 0, 1);   CHECK(r16(0) == 1u << 5);
    f32(5.0f, w + 6); req(sizeof w, w); CHECK(is_ex(0x10, MODBUS_EX_REFUSED));
    REQ(0x05, 0, 2, 0xFF, 0); CHECK(resets == 1 && !fault_f6);
    // Warning OK.
    warn = "W-2 Not settling: switched to MANUAL, valve idle";
    REQ(0x03, 0, 6, 0, 1);   CHECK(r16(0) == 1u << 1);
    REQ(0x05, 0, 3, 0xFF, 0); CHECK(warn == NULL);
    // Status coils are read only.
    REQ(0x05, 0, 16, 0xFF, 0); CHECK(is_ex(5, MODBUS_EX_ADDRESS));
    // Remote control Off (0.13.4): writes refused, reads answer, the writes bit clear.
    remote = false;
    f32(6.0f, w + 6); req(sizeof w, w); CHECK(is_ex(0x10, MODBUS_EX_FUNCTION) && sp != 6000);
    REQ(0x05, 0, 1, 0xFF, 0); CHECK(is_ex(5, MODBUS_EX_FUNCTION) && stops == 1);
    REQ(0x03, 0, 3, 0, 1);    CHECK(rlen == 11 && ((r16(0) >> 8) & 1) == 0);
    REQ(0x05, 0, 0, 0xFF, 0); CHECK(is_ex(5, MODBUS_EX_FUNCTION));
    // Outside the map is still an address error, not a write refusal.
    REQ(0x05, 0, 9, 0xFF, 0); CHECK(is_ex(5, MODBUS_EX_ADDRESS));
    remote = true;
    REQ(0x03, 0, 3, 0, 1);    CHECK(((r16(0) >> 8) & 1) == 1);
    // Warning bits: W-1 question, the strip warning by its number (W-5 move
    // timed out, 0.16.29); bit 2 (W-3 controller hot, removed) stays 0.
    question = true; cs.die_mc = 56000; warn = "W-5 Move timed out, valve stopped";
    REQ(0x03, 0, 6, 0, 1);   CHECK(r16(0) == ((1u << 0) | (1u << 4)));
    question = false; cs.die_mc = 31500; warn = NULL;
}

/* ---- TCP server for client.py ---------------------------------------- */

static void serve(int port)
{
    int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port),
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 1)) { perror("listen"); exit(1); }
    printf("serving on 127.0.0.1:%d\n", port);
    fflush(stdout);
    for (;;) {
        int c = accept(ls, NULL, NULL);
        uint8_t buf[MODBUS_ADU_MAX], out[MODBUS_ADU_MAX];
        size_t len = 0;
        mbmap_set_peer("127.0.0.1");
        for (;;) {
            ssize_t got = recv(c, buf + len, sizeof buf - len, 0);
            if (got <= 0) break;
            len += (size_t)got;
            size_t n;
            while ((n = modbus_frame_len(buf, len)) && n != (size_t)-1) {
                size_t r = modbus_handle(buf, n, out);
                send(c, out, r, 0);
                memmove(buf, buf + n, len - n);
                len -= n;
            }
            if (n == (size_t)-1) break;
        }
        close(c);
        if (last_event[0]) { printf("event: %s\n", last_event); fflush(stdout); }
    }
}

int main(int argc, char **argv)
{
    tests();
    printf("modbus test: %d checks, %d failed\n", checks, fails);
    // serve <port> [off]: off serves with Remote control Off (writes refused).
    if (argc >= 3 && !strcmp(argv[1], "serve")) {
        sp = 0; sp_set = false; mode = VALVE_MANUAL;
        remote = !(argc == 4 && !strcmp(argv[3], "off"));
        serve(atoi(argv[2]));
    }
    return fails != 0;
}
