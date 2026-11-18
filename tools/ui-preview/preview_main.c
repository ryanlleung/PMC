/*
 * Host render of the screens (ui.c, ui_home.c, ui_valve.c, ui_settings.c,
 * ui_trend.c, motor_screen.c) against fake readings (fake_hw.c) and a fake
 * valve layer below. Writes one 480 x 272 frame as raw RGB565 and checks
 * that the LVGL heap comes back after opening and leaving screens.
 *
 *   ./preview <scenario> <out.raw>
 *
 * build.sh lists the scenarios. Menu: three columns of 148 at x 10, 166,
 * 322 (centres 84, 240, 396), rows y 48, 104, 160 (centres 72, 128, 184):
 * Trend, Events, PID / Motor, Valve, Pressure sensor / System, Factory
 * reset, Diagnostics.
 */
#ifndef TVX
#define TVX 444
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "clicks.h"
#include "stepper3.h"
#include "valve.h"
#include "ui.h"
#include "cal.h"
#include "cfg.h"
#include "extflash.h"
#include "valvepos.h"
#include <stdarg.h>
#include <stddef.h>

#define W 480
#define H 272
#ifndef POOL2_KB
#define POOL2_KB 24
#endif

void fake_hw_set_scenario(const char *name);

static uint16_t fb[W * H];
static uint8_t draw_buf[W * H * 2] __attribute__((aligned(4)));

/* Fake valve layer ------------------------------------------------------- */
static valve_mode_t v_mode = VALVE_NOT_CAL;
static bool v_cal, v_f01, v_f12, v_f16;
static int v_step = -1;
static int32_t v_pct10, v_target10;
static bool moving;

valve_mode_t valve_mode(void) { return v_mode; }
const char *valve_mode_name(void)
{
    return v_mode == VALVE_CALIBRATING ? "HOMING" : v_mode == VALVE_MANUAL ? "MANUAL"
         : v_mode == VALVE_AUTO ? "AUTO" : v_mode == VALVE_SHUTDOWN ? "SHUTDOWN"
         : v_mode == VALVE_OFF ? "OFF" : "NOT HOMED";
}
// Shut down.
bool valve_shutdown(bool close, const char **why) { (void)why; v_mode = close ? VALVE_SHUTDOWN : VALVE_OFF; return true; }
bool valve_turn_on(const char **why) { (void)why; v_mode = VALVE_MANUAL; return true; }
bool valve_shutdown_progress(int *left, int *secs)
{
    if (v_mode != VALVE_SHUTDOWN) return false;
    *left = 60; *secs = 34;
    return true;
}
const char *valve_off_why(void) { return v_mode == VALVE_OFF ? "Controller is off" : NULL; }
// AUTO.
static bool v_sp_set;
static int32_t v_sp = 5000;
static ctrl_reg_t v_reg = CTRL_REG_BAND;
bool valve_auto(void) { return v_mode == VALVE_AUTO; }
static const char *v_warn;
const char *valve_warning(void) { return v_warn; }
void valve_warning_clear(void) { v_warn = NULL; }
int valve_fault_count(void);
bool valve_start_auto(const char **why)
{
    if (valve_fault_count()) { *why = "Reset the fault first"; return false; }
    v_mode = VALVE_AUTO;
    return true;
}
bool valve_to_manual(const char **why) { (void)why; v_mode = VALVE_MANUAL; return true; }
bool valve_set_target(int32_t m, const char **why) { (void)why; v_sp = m / 100 * 100; v_sp_set = true; return true; }
bool valve_target(int32_t *m) { if (m) *m = v_sp; return v_sp_set; }
void valve_hold(bool on) { (void)on; }
// As valve.c: the target changes only if AUTO starts.
bool valve_start_auto_at(int32_t m, const char **why)
{
    if (valve_fault_count()) { *why = "Reset the fault first"; return false; }
    return valve_set_target(m, why) && valve_start_auto(why);
}
int32_t valve_range_mmbar(void) { return 1034214; }
ctrl_reg_t valve_regulation(void) { return v_mode == VALVE_AUTO ? v_reg : CTRL_REG_NONE; }
static int v_pidset = 1;
int valve_pid_set(void) { return v_pidset; }
bool valve_calibrated(void) { return v_cal; }
static valve_ref_t v_ref = VALVE_REF_HOMED;
valve_ref_t valve_reference(void) { return v_cal ? v_ref : VALVE_REF_NONE; }
int32_t valve_pct_milli(void) { return v_pct10 * 100 + 37; }
int32_t valve_target_pct_milli(void) { return (moving ? v_target10 : v_pct10) * 100; }
int valve_cal_step(void) { return v_step; }
uint32_t valve_cal_budget_s(void) { return 300; }
bool valve_set_closed(const char **why) { (void)why; v_cal = true; v_ref = VALVE_REF_HAND; v_mode = VALVE_MANUAL; v_pct10 = 0; return true; }
bool valve_set_position(int32_t pct100, const char **why) { (void)why; v_cal = true; v_ref = VALVE_REF_HAND; v_mode = VALVE_MANUAL; v_pct10 = pct100 / 10; return true; }
bool valve_calibrate(const char **why) { (void)why; v_mode = VALVE_CALIBRATING; v_step = 1; return true; }
bool valve_goto_pct10(int32_t p, const char **why) { (void)why; v_target10 = p; moving = true; return true; }
bool valve_goto_pct100(int32_t p, const char **why) { return valve_goto_pct10(p / 10, why); }
void valve_stop(void) { moving = false; }
bool valve_stop_latches(void) { return valve_auto(); }
int valve_fault_count(void) { return v_f01 + v_f16 + v_f12; }
bool valve_motion_blocked(void) { return v_f12 || v_f16; }
bool valve_fault_first(const char **tag, const char **text)
{
    if (v_f01) { *tag = "F-1"; *text = "No pressure signal"; return true; }
    if (v_f16) { *tag = "F-2"; *text = "Limit not found while homing"; return true; }
    if (v_f12) { *tag = "F-3"; *text = "Limit pressed when it should be free"; return true; }
    return false;
}
bool valve_fault_is(const char *tag)
{
    return (v_f01 && !strcmp(tag, "F-1")) || (v_f16 && !strcmp(tag, "F-2")) || (v_f12 && !strcmp(tag, "F-3"));
}
int valve_reset(char *left, size_t n) { int a = valve_fault_count(); v_f12 = v_f16 = false; if (n) strcpy(left, v_f01 ? "F-1" : ""); return a; }
static bool v_q;
bool valve_question(int32_t *p) { if (v_q && p) *p = 187; return v_q; }
int32_t valve_question_milli(void) { return 187 * 10; }
bool valve_question_auto(void) { return false; }
int valve_homed_take(uint32_t *took_s, int32_t *found_milli) { (void)took_s; (void)found_milli; return 0; }
const char *valve_warning_tag(void) { return ""; }
int valve_auto_pinned(void) { return 0; }
uint32_t valve_cal_fast_s(void) { return 60; }
int valve_switch(void) { return 0; }
bool valve_answer(bool z, const char **why) { (void)z; (void)why; v_q = false; return true; }
bool valve_fault_get(int i, valve_fault_t *f)
{
    // The three faults (v_f16: F-2 limit not found, v_f12: F-3 limit pressed).
    const char *tags[3] = { "F-1", "F-2", "F-3" },
               *texts[3] = { "No pressure signal", "Limit not found while homing", "Limit pressed when it should be free" };
    bool on[3] = { v_f01, v_f16, v_f12 };
    int k = 0;
    for (int j = 0; j < 3; j++) {
        if (!on[j]) continue;
        if (k++ != i) continue;
        f->tag = tags[j]; f->text = texts[j]; f->age_ms = j ? 64000 : 312000;
        f->mode = j == 1 ? VALVE_CALIBRATING : VALVE_MANUAL; f->can_reset = j != 0;
        return true;
    }
    return false;
}
static const char *const ev_tag[] = { "STOP", "W-1", "REF", "REF", "BOOT" };
static const char *const ev_text[] = { "Operator stop at count 33880", "Point set as closed (count 0)",
    "Homed in 15 s, position 0", "Homing started at count 1203", "Power-up (event only), valve not homed" };
int valve_event_count(void) { return 5; }
uint32_t valve_event_total(void) { return 5; }
uint64_t valve_ms(void)   // as valve.c: the tick carried past its 49.7 day wrap
{
    static uint32_t last;
    static uint64_t ms;
    uint32_t now = lv_tick_get();
    ms += (uint32_t)(now - last);
    last = now;
    return ms;
}
bool valve_event_get(int i, uint64_t *age, const char **tag, const char **text)
{
    if (i < 0 || i >= 5) return false;
    *age = 64000 + i * 61000; *tag = ev_tag[i]; *text = ev_text[i];
    return true;
}
bool valve_last_cal(uint64_t *age, uint32_t *took) { if (!v_cal) return false; *age = 186000; *took = 15; return true; }
bool limit_open_now(void) { return v_cal && v_pct10 == 0; }
const char *rtclock_iso(void) { return "2026-10-15 14:32:05"; }
const char *valve_last_cal_iso(void) { return "2026-10-05 14:28:59"; }
bool stepper3_last_move(int32_t *from, uint32_t *ms) { if (!v_cal) return false; *from = 0; *ms = 41300; return true; }
void stepper3_stop_ramped(void) {}
bool stepper3_busy(void);
extern bool fake_override;
bool stepper3_override_cancel(void) { fake_override = false; return false; }

/* Settings store: the real cfg.c and cal.c over a serial flash in RAM. */
static uint8_t *flash;
bool extflash_init(void) { return true; }
void extflash_read(uint32_t a, void *b, size_t n) { memcpy(b, flash + a, n); }
bool extflash_erase_4k(uint32_t a) { memset(flash + (a & ~0xFFFu), 0xFF, 4096); return true; }
bool extflash_write(uint32_t a, const void *b, size_t n)
{
    const uint8_t *p = b;
    for (size_t i = 0; i < n; i++) flash[a + i] &= p[i];
    return true;
}
static int32_t v_set[VALVE_N_SETTINGS] = { 99072, 400, 25, 400, 800, 400 };
void valve_settings(int32_t v[VALVE_N_SETTINGS]) { memcpy(v, v_set, sizeof v_set); }
const char *valve_settings_check(const int32_t v[VALVE_N_SETTINGS]) { (void)v; return NULL; }
bool valve_set_settings(const int32_t v[VALVE_N_SETTINGS]) { memcpy(v_set, v, sizeof v_set); return true; }
int32_t valve_pos_max(void) { return v_set[0]; }
static bool v_band;
bool valve_limit_band(void) { return v_band; }
void valve_limit_band_set(bool on) { v_band = on; }
int valve_pass_lo(void) { return (int)cfg_tune(CFG_PASS_LO); }
int valve_pass_hi(void) { return (int)cfg_tune(CFG_PASS_HI); }
void valve_uncalibrate(const char *why) { (void)why; v_cal = false; v_mode = VALVE_NOT_CAL; }
void valve_event(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
static bool vp_offer, vp_moving;
bool valvepos_offer(int32_t *c) { if (vp_offer) *c = 33880; return vp_offer; }
bool valvepos_use(const char **why) { (void)why; vp_offer = false; v_cal = true; v_ref = VALVE_REF_SAVED; v_mode = VALVE_MANUAL; return true; }
void valvepos_decline(void) { vp_offer = false; }
bool valvepos_offer_moving(int32_t *s) { *s = 400; return vp_offer && vp_moving; }
bool valvepos_offer_auto(int32_t *sp) { (void)sp; return false; }
bool valvepos_offer_unparked(void) { return false; }

static uint32_t crc32(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t c = 0xFFFFFFFFUL;
    while (len--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320UL & (0UL - (c & 1)));
    }
    return ~c;
}

// A board with saved settings: a two-point calibration, vacuum and
// 1013.25 mB (written through cfg.c, so the record is always the current
// version).
static void seed_flash(void)
{
    cal_init();
    cfg_init();
    cal_rec_t c;
    cal_get_record(&c);
    c.r_ppb[0] = 127900; c.p_mmbar[0] = 0;
    c.r_ppb[1] = 9798300; c.p_mmbar[1] = 1013250;
    c.crc = crc32(&c, offsetof(cal_rec_t, crc));
    int32_t v[CFG_N] = { 0 };
    bool on[CFG_N] = { false };
    const char *why;
    if (!cal_set_record(&c, false) || !cfg_save_with(v, on, &why)) printf("seed not saved\n");
}

/* Render ------------------------------------------------------------------ */
static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    const uint16_t *src = (const uint16_t *)px;
    for (int32_t y = a->y1; y <= a->y2; y++)
        for (int32_t x = a->x1; x <= a->x2; x++)
            fb[y * W + x] = *src++;
    lv_display_flush_ready(d);
}

static lv_point_t touch_point;
static bool touch_down;
static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point = touch_point;
    data->state = touch_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void run(int ms)
{
    for (int t = 0; t < ms; t += 20) { lv_tick_inc(20); lv_timer_handler(); }
}

static void tap(int32_t x, int32_t y)
{
    touch_point.x = x;
    touch_point.y = y;
    touch_down = true;
    run(100);
    touch_down = false;
    run(100);
}

// Types on the IP page's keys: 7 8 9 delete / 4 5 6 0 / 1 2 3 . at x 41,
// 96, 150, 205 and y 77, 128, 179; '<' is delete.
static void ip_type(const char *s)
{
    static const char keys[] = "789<4560123.";
    static const int32_t kx[4] = { 41, 96, 150, 205 }, ky[3] = { 77, 128, 179 };
    for (; *s; s++) {
        int k = (int)(strchr(keys, *s) - keys);
        tap(kx[k % 4], ky[k / 4]);
    }
}

static void drag(int32_t x, int32_t y0, int32_t y1)
{
    touch_point.x = x;
    touch_point.y = y0;
    touch_down = true;
    run(60);
    for (int i = 1; i <= 10; i++) {
        touch_point.y = y0 + (y1 - y0) * i / 10;
        run(20);
    }
    touch_down = false;
    run(600);
}

static size_t heap_used(void)
{
    lv_mem_monitor_t m;
    lv_mem_monitor(&m);
    return m.total_size - m.free_size;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <state> out.raw\n", argv[0]);
        return 2;
    }
    const char *st = argv[1];
    fake_hw_set_scenario(!strcmp(st, "nodruck") ? "nodruck" : !strcmp(st, "neg") ? "neg" : "cal");
    bool page = !strcmp(st, "valve") || !strcmp(st, "valvenc") || !strcmp(st, "limithit") ||
                !strcmp(st, "calpage") || !strcmp(st, "calhelp") || !strcmp(st, "faults") || !strcmp(st, "detail") || !strcmp(st, "diag") ||
                !strcmp(st, "help") || !strcmp(st, "help2") || !strcmp(st, "help3") || !strcmp(st, "back") ||
                !strncmp(st, "set-", 4) || !strncmp(st, "valve-", 6) || !strncmp(st, "cal", 3) || strstr(st, "trend");
    if (!strcmp(st, "manual") || !strcmp(st, "moving") || !strcmp(st, "stop") || !strcmp(st, "keypad") ||
        !strcmp(st, "neg") || (page && strcmp(st, "valvenc"))) {
        v_mode = VALVE_MANUAL; v_cal = true; v_pct10 = 342;
    }
    if (!strcmp(st, "moving")) { extern bool fake_busy; fake_busy = moving = true; v_target10 = 500; }
    if (!strcmp(st, "calib") || !strcmp(st, "calpage2")) { v_mode = VALVE_CALIBRATING; v_step = 4; }
    if (!strcmp(st, "nodruck")) v_f01 = true;
    // AUTO.
    bool aut = !strncmp(st, "auto", 4) || !strcmp(st, "paused") || !strcmp(st, "homefail") || !strcmp(st, "trendauto");
    if (aut) { v_mode = VALVE_MANUAL; v_cal = true; v_pct10 = 342; v_sp_set = strcmp(st, "autofirst") != 0; }
    if (!strcmp(st, "auto") || !strcmp(st, "autostable") || !strcmp(st, "autoreg") || !strcmp(st, "autosp") ||
        !strcmp(st, "automan") || !strcmp(st, "autovalve") || !strcmp(st, "autozone")) v_mode = VALVE_AUTO;
    if (!strcmp(st, "autozone")) v_pidset = 2;
    if (!strcmp(st, "autostable")) v_reg = CTRL_REG_STABLE;
    if (!strcmp(st, "autoreg")) { extern bool fake_busy; fake_busy = moving = true; v_target10 = 356; v_reg = CTRL_REG_REGULATING; }
    if (!strcmp(st, "paused")) v_warn = "W-2 No pressure response: switched to MANUAL";   // shown until OK
    if (!strcmp(st, "homefail")) v_f16 = true;   // F-2 Limit not found while homing
    if (!strcmp(st, "faults") || !strcmp(st, "detail")) { v_f01 = true; v_f12 = true; }

    lv_init();
    // An hour of trend: pumped down from air, then held near 5 mbar with the
    // valve moved by hand. After lv_init, so the tick that trend.c's
    // one-second slots follow keeps running.
    if (v_cal && strcmp(st, "notrend")) {
        int32_t keep = v_pct10;
        clicks_state_t s = *clicks_state();
        for (int i = 0; i < 3600; i++) {
            double tt = i;
            double p = i < 900 ? 1013.0 * exp(-tt / 120.0) + 5.0 : 5.0 + (i > 2700 ? 0.8 : 0) + (i > 3300 ? -0.5 : 0);
            p += 0.15 * sin(tt / 23.0) + 0.05 * sin(tt / 3.1);
            s.p_mmbar = (int32_t)(p * 1000);
            v_pct10 = i < 900 ? 342 : i > 3300 ? 300 : i > 2700 ? 380 : 342;
            if (!strcmp(st, "trendauto"))   // AUTO at 5.0 then 5.8 mB for the middle of the hour
                v_mode = i >= 1500 && i < 3300 ? VALVE_AUTO : VALVE_MANUAL, v_sp = i < 2700 ? 5000 : 5800;
            trend_second(&s);
            lv_tick_inc(1000);   // one sample per second of real time (trend.c slots)
        }
        v_pct10 = keep;
    }
    static uint8_t pool2[POOL2_KB * 1024] __attribute__((aligned(8)));   // 24 KB as main.c board_init; 64-bit pointers need more
    lv_mem_add_pool(pool2, sizeof pool2);
    lv_display_t *d = lv_display_create(W, H);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d, draw_buf, NULL, sizeof draw_buf, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(d, flush_cb);
    lv_indev_t *in = lv_indev_create();
    lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(in, touch_read);

    flash = malloc(8u << 20);
    memset(flash, 0xFF, 8u << 20);
    if (strcmp(st, "nosave"))
        seed_flash();
    if (!strcmp(st, "splash")) {   // the start-up screen
        ui_splash();
        FILE *f = fopen(argv[2], "wb");
        if (!f) { perror(argv[2]); return 1; }
        fwrite(fb, sizeof fb, 1, f);
        fclose(f);
        return 0;
    }
    cal_init();
    cfg_init();
    // The PID table with other gains in zones 1 to 3.
    if (!strcmp(st, "autozone")) {
        int32_t v[CFG_N] = { 0 };
        bool on[CFG_N] = { false };
        const char *why;
        static const cfg_key_t k[] = { CFG_KP, CFG_TI, CFG_KP2, CFG_TI2, CFG_KP3, CFG_TI3 };
        static const int32_t x[] = { 10, 40, 80, 30, 120, 30 };
        for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) { v[k[i]] = x[i]; on[k[i]] = true; }
        if (!cfg_save_with(v, on, &why)) printf("PID table not saved: %s\n", why);
    }
    ui_init();
    run(1200);
    valve_mode_t start_mode = v_mode;

    // Heap check: Menu open and back ten times.
    size_t before = heap_used();
    // Header: Home at x 0-44 on every page but Home, Back at 44-88; on
    // Home the power icon is at 0-44. Menu: nine tiles, see the top of
    // this file.
#define HOME() tap(22, 20)
#define BACK() tap(66, 20)
#define MENU() tap(415, 184)
#define M_TREND()  tap(84, 72)
#define M_EVENTS() tap(240, 72)
#define M_PID()    tap(396, 72)
#define M_MOTOR()  tap(84, 128)
#define M_VALVE()  tap(240, 128)
#define M_SENSOR() tap(396, 128)
#define M_SYS()    tap(84, 184)
#define M_RESET()  tap(240, 184)   // Factory reset: a question, no page
#define M_DIAG()   tap(396, 184)
    for (int i = 0; i < 10; i++) {
        tap(415, 184);          // Menu
        HOME();
    }
    tap(300, 184);              // Start AUTO / Go to area of the valve card
    HOME();                     // Home closes any popup
    for (int i = 0; i < 5; i++) {
        // Measured from the second round: AUTO keeps 144 B that a page
        // builds once, the same after 5 rounds or 15.
        if (i == 1)
            before = heap_used();
        // Motor first, opened from the Menu and again by ui_service(): a
        // page left built, with a keypad after it, would run the heap out.
        MENU(); M_MOTOR(); ui_service(); run(300);
        BACK(); run(300);
        HOME();
        tap(415, 72); BACK();                                   // Home's PID button
        MENU(); M_TREND(); HOME();
        MENU(); M_EVENTS(); HOME();
        MENU(); M_SENSOR(); HOME();
        MENU(); M_SYS(); HOME();
        MENU(); M_MOTOR(); BACK();
        MENU(); M_VALVE(); BACK();
        MENU(); M_PID(); BACK();
        MENU(); M_DIAG(); BACK();
        HOME();
    }
    run(1200);
    size_t after = heap_used();
    lv_mem_monitor_t mm;
    lv_mem_monitor(&mm);
    fprintf(stderr, "%s: heap used %zu B, after the round trips %zu B, peak %zu B\n", st, before, after,
            (size_t)mm.max_used);
    if (after > before + 64) {
        fprintf(stderr, "heap grew\n");
        return 1;
    }
    // The round trips tap Home's valve tile, which is Stop AUTO in AUTO:
    // back to the scenario's mode before its own taps.
    v_mode = start_mode;
    run(1200);

    if (!strcmp(st, "stop")) tap(415, 240);
    // Motor: Jog at x 14-123, Settings at 123-232.
    if (!strncmp(st, "motor", 5)) { MENU(); M_MOTOR(); }   // Motor, Jog tab
    if (!strcmp(st, "motor-held")) { extern bool fake_busy; fake_override = true; fake_busy = true; }   // Limit override held
    if (!strcmp(st, "motor-moving")) { extern bool fake_busy; fake_busy = true; ui_service(); }   // a move running
    if (!strcmp(st, "motor-settings") || !strcmp(st, "motor-speed")) tap(177, 72);   // Settings tab
    if (!strcmp(st, "motor-speed")) tap(64, 128);                                    // Speed: keypad
    if (!strcmp(st, "overon")) { fake_override = true; tap(415, 184); }   // Menu, override armed
    if (!strcmp(st, "menu")) MENU();
    if (!strcmp(st, "keypad")) { tap(300, 184); tap(41, 184); tap(111, 240); tap(251, 240); tap(111, 184); tap(111, 184); }
    if (!strcmp(st, "confirm")) { v_mode = VALVE_NOT_CAL; v_cal = false; tap(269, 184); }   // Home valve: Home or Set position
    if (!strcmp(st, "autofirst") || !strcmp(st, "autoask")) tap(93, 184);   // Start AUTO (the Pressure tile)
    if (!strcmp(st, "automan") || !strcmp(st, "paused2")) tap(269, 184);     // Stop AUTO (the Valve tile)
    if (!strcmp(st, "autosp")) { tap(115, 184); tap(41, 240); tap(111, 240); tap(321, 184); }   // Setpoint 12: applies at once
    if (!strcmp(st, "limithit")) v_q = true;
    if (!strcmp(st, "shutask")) tap(24, 20);                      // the power icon (top left): confirm
    if (!strcmp(st, "shutting")) { tap(24, 20); tap(250, 180); }   // Shut down: black, closing
    if (!strcmp(st, "off")) v_mode = VALVE_OFF;
    if (!strcmp(st, "shutclose") || !strcmp(st, "shutleave")) { v_mode = VALVE_MANUAL; v_cal = true; v_pct10 = 342; tap(24, 20); }
    if (!strcmp(st, "shutleave")) tap(240, 111);                    // Close the valve unticked
    if (!strcmp(st, "calset")) v_ref = VALVE_REF_HAND;   // the position set by hand
    if (!strcmp(st, "handset")) { v_mode = VALVE_MANUAL; v_cal = true; v_pct10 = 342; v_ref = VALVE_REF_HAND; }
    if (!strcmp(st, "setpos")) { v_mode = VALVE_NOT_CAL; v_cal = false; tap(269, 184); tap(240, 146); }   // Home valve, Set position
    if (!strcmp(st, "bootpick")) { v_mode = VALVE_NOT_CAL; v_cal = false; vp_offer = true; }
    if (!strcmp(st, "bootask")) { v_mode = VALVE_NOT_CAL; v_cal = false; vp_offer = true; }
    if (!strcmp(st, "bootmove")) { v_mode = VALVE_NOT_CAL; v_cal = false; vp_offer = true; vp_moving = true; }
    // Up time on System: 23 h 59 min, 123 d 23 h and close to the largest
    // count (49710 d 6 h), the tick stepped in jumps under its 49.7 day wrap.
    if (!strncmp(st, "set-up", 6)) {
        uint64_t want = !strcmp(st, "set-up-h") ? 86370ull : !strcmp(st, "set-up-d") ? 123ull * 86400 + 23 * 3600 + 600
                      : 4294900000ull;
        uint64_t left = (want - ui_uptime_s()) * 1000;
        while (left) {
            uint32_t step = left > 3000000000ull ? 3000000000u : (uint32_t)left;
            lv_tick_inc(step);
            (void)ui_uptime_s();
            left -= step;
        }
        run(600);   // the jump ran the idle time-out: back on Home, dimmed
        ui_wake();  // so the Menu tap below is not taken as a wake
        fprintf(stderr, "uptime %s: %lu s\n", st, (unsigned long)ui_uptime_s());
    }
    // The settings pages, each from the Menu.
    if (!strncmp(st, "set-", 4) || !strncmp(st, "valve-", 6)) MENU();
    if (!strncmp(st, "set-net", 7) || !strcmp(st, "set-remote") || !strncmp(st, "set-up", 6)) M_SYS();
    if (!strcmp(st, "set-remote")) tap(357, 128);   // Network control Off (On by default)
    if (!strncmp(st, "set-netaddr", 11) || !strcmp(st, "set-netused")) tap(123, 72);   // IP address and mask
    if (!strcmp(st, "set-netaddr2") || !strcmp(st, "set-netused")) {
        ip_type("<<<<<<<<<<<<<10.1.2.50");           // Address
        tap(357, 128);                               // Mask
        ip_type("<<<<<<<<<<<<<255.255.255.0");
    }
    if (!strcmp(st, "set-netused")) tap(298, 184);   // Save
    // Valve: Position, Homing and Limit tabs centred at x 70, 181, 293.
    if (!strncmp(st, "valve-", 6)) M_VALVE();
    if (!strcmp(st, "valve-travel")) tap(181, 128);                          // Position tab, Travel: keypad
    if (!strcmp(st, "valve-homing")) tap(181, 72);                           // Homing tab
    if (!strcmp(st, "valve-limit") || !strcmp(st, "valve-limiton")) tap(293, 72);   // Limit tab
    if (!strcmp(st, "valve-limiton")) tap(273, 184);                         // Disable switch On
    // PID: Gains, Zones and Settings tabs, as Valve's.
    if (!strncmp(st, "set-tune", 8) || !strncmp(st, "set-save", 8)) M_PID();
    if (!strcmp(st, "set-tunezones")) tap(181, 72);   // Zones tab
    if (!strcmp(st, "set-tuneauto")) tap(293, 72);    // Settings tab (the AUTO limits)
    if (!strcmp(st, "set-tunehelp")) tap(415, 72);    // Gains tab, Help
    if (!strcmp(st, "set-tunezone3")) tap(225, 128);  // Gains tab, Zone 3
    // KP 1's keypad with 0.1 typed; then Set, which leaves a draft.
    if (!strcmp(st, "set-tunekp") || !strcmp(st, "set-tunedraft") || !strcmp(st, "set-saved") ||
        !strcmp(st, "set-tuneundo")) { tap(65, 184); tap(231, 183); tap(231, 233); tap(45, 233); }
    if (!strcmp(st, "set-tunedraft") || !strcmp(st, "set-saved") || !strcmp(st, "set-tuneundo")) tap(305, 208);
    if (!strcmp(st, "set-saved")) tap(415, 128);    // Save: at once, no question
    if (!strcmp(st, "set-tuneundo")) tap(415, 184);   // Undo: at once for drafts
    if (!strcmp(st, "reset-ask") || !strcmp(st, "reset-done")) { MENU(); M_RESET(); }
    if (!strcmp(st, "reset-done")) tap(249, 136 + 40);
    if (strstr(st, "trend")) { MENU(); M_TREND(); }   // Menu, Trend
    if (!strcmp(st, "trend1h")) { tap(415, 72); tap(415, 72); }
    if (!strcmp(st, "trend1m")) { tap(415, 128); tap(415, 184); tap(415, 184); }
    if (!strcmp(st, "calsensor") || !strcmp(st, "calvac")) { MENU(); M_SENSOR(); }
    if (!strcmp(st, "calvac")) tap(298, 128);
    if (!strcmp(st, "back")) { MENU(); M_DIAG(); BACK(); }   // Diagnostics, Back: Menu
    if (!strcmp(st, "help") || !strcmp(st, "help2") || !strcmp(st, "help3")) { tap(415, 72); tap(415, 72); }   // PID, Help
    if (!strcmp(st, "help2")) tap(TVX, 172);
    if (!strcmp(st, "help3")) { tap(TVX, 172); tap(TVX, 172); tap(TVX, 172); tap(TVX, 172); }
    if (!strcmp(st, "faults") || !strcmp(st, "detail")) { MENU(); M_EVENTS(); }
    if (!strcmp(st, "detail")) tap(150, 72);
    if (!strcmp(st, "calpage") || !strcmp(st, "calpage2") || !strncmp(st, "calset", 6)) { MENU(); M_VALVE(); }
    if (!strcmp(st, "calhelp")) { MENU(); M_VALVE(); tap(415, 72); }   // Valve, Help
    if (!strcmp(st, "bootpick")) { run(600); tap(240, 146); }   // start-up question, Set position
    if (!strcmp(st, "diag")) { stepper3_move(0); MENU(); M_DIAG(); }   // a move done: "At target, 41.3 s"
    run(1200);

    FILE *f = fopen(argv[2], "wb");
    if (!f) { perror(argv[2]); return 1; }
    fwrite(fb, sizeof fb, 1, f);
    fclose(f);
    return 0;
}
