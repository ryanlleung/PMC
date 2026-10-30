/*
 * Host render of the new screens (ui.c, ui_home.c, ui_valve.c): Home, Menu,
 * the keypad and dialogs, and the stage 5 pages, against fake readings
 * (fake_hw.c) and a fake valve layer below. Writes one 480 x 272 frame as raw RGB565 and
 * checks the LVGL heap comes back after opening and leaving screens.
 *
 *   ./ui_new <state> <out.raw>
 *   state: notcal | manual | calib | moving | nodruck | stop | neg | menu | keypad | confirm
 *          | valve | valvenc | limithit | calpage | faults | detail | diag | help
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
         : v_mode == VALVE_AUTO ? "AUTO" : "NOT HOMED";
}
// AUTO (stage 8).
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
int32_t valve_range_mmbar(void) { return 1034214; }
int32_t valve_band_mmbar(void) { return 100; }
ctrl_reg_t valve_regulation(void) { return v_mode == VALVE_AUTO ? v_reg : CTRL_REG_NONE; }
bool valve_calibrated(void) { return v_cal; }
static valve_ref_t v_ref = VALVE_REF_HOMED;
valve_ref_t valve_reference(void) { return v_cal ? v_ref : VALVE_REF_NONE; }
int32_t valve_pct10(void) { return v_pct10; }
int32_t valve_target_pct10(void) { return moving ? v_target10 : v_pct10; }
int32_t valve_pct_milli(void) { return v_pct10 * 100 + 37; }
int32_t valve_target_pct_milli(void) { return valve_target_pct10() * 100; }
int valve_cal_step(void) { return v_step; }
uint32_t valve_cal_budget_s(void) { return 300; }
uint32_t valve_cal_elapsed_s(void) { return v_mode == VALVE_CALIBRATING ? 42 : 0; }
bool valve_set_closed(const char **why) { (void)why; v_cal = true; v_ref = VALVE_REF_HAND; v_mode = VALVE_MANUAL; v_pct10 = 0; return true; }
bool valve_set_position(int32_t p, const char **why) { (void)why; v_cal = true; v_ref = VALVE_REF_HAND; v_mode = VALVE_MANUAL; v_pct10 = p / 10; return true; }
bool valve_calibrate(const char **why) { (void)why; v_mode = VALVE_CALIBRATING; v_step = 1; return true; }
bool valve_goto_pct10(int32_t p, const char **why) { (void)why; v_target10 = p; moving = true; return true; }
bool valve_goto_pct100(int32_t p, const char **why) { return valve_goto_pct10(p / 10, why); }
bool valve_move_steps(int32_t steps, const char **why) { (void)steps; (void)why; return true; }
void valve_stop(void) { moving = false; }
bool valve_stop_latches(void) { return valve_auto(); }
int valve_fault_count(void) { return v_f01 + v_f16 + v_f12; }
bool valve_motion_blocked(void) { return v_f12 || v_f16; }
bool valve_fault_first(const char **tag, const char **text)
{
    if (v_f01) { *tag = "F-1"; *text = "No pressure signal"; return true; }
    if (v_f12) { *tag = "F-5"; *text = "Motion timeout"; return true; }
    if (v_f16) { *tag = "F-6"; *text = "No pressure response"; return true; }
    return false;
}
bool valve_fault_is(const char *tag)
{
    return (v_f01 && !strcmp(tag, "F-1")) || (v_f12 && !strcmp(tag, "F-5")) || (v_f16 && !strcmp(tag, "F-6"));
}
int valve_reset(char *left, size_t n) { int a = valve_fault_count(); v_f12 = v_f16 = false; if (n) strcpy(left, v_f01 ? "F-1" : ""); return a; }
static bool v_q;
bool valve_question(int32_t *p) { if (v_q && p) *p = 187; return v_q; }
bool valve_answer(bool z, const char **why) { (void)z; (void)why; v_q = false; return true; }
bool valve_fault_get(int i, valve_fault_t *f)
{
    const char *tags[3] = { "F-1", "F-5", "F-6" },
               *texts[3] = { "No pressure signal", "Motion timeout", "No pressure response" };
    bool on[3] = { v_f01, v_f12, v_f16 };
    int k = 0;
    for (int j = 0; j < 3; j++) {
        if (!on[j]) continue;
        if (k++ != i) continue;
        f->tag = tags[j]; f->text = texts[j]; f->age_ms = j ? 64000 : 312000;
        f->mode = j == 2 ? VALVE_AUTO : VALVE_MANUAL; f->can_reset = j != 0;
        return true;
    }
    return false;
}
static const char *const ev_tag[] = { "STOP", "W-1", "REF", "REF", "BOOT" };
static const char *const ev_text[] = { "Operator stop at count 33880", "Point set as 0 %, count 0",
    "Homed in 15 s, position 0", "Homing started at count 1203", "Power-up (event only), valve not homed" };
int valve_event_count(void) { return 5; }
uint32_t valve_event_total(void) { return 5; }
bool valve_event_get(int i, uint32_t *age, const char **tag, const char **text)
{
    if (i < 0 || i >= 5) return false;
    *age = 64000 + i * 61000; *tag = ev_tag[i]; *text = ev_text[i];
    return true;
}
bool valve_last_cal(uint32_t *age, uint32_t *took) { if (!v_cal) return false; *age = 186000; *took = 15; return true; }
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
static int32_t v_set[VALVE_N_SETTINGS] = { 99072, 400, 25, 400, 800, 0, 400 };
static const char *const v_name[VALVE_N_SETTINGS] = { "POSMAX", "FHOME", "FCREEP", "BACKOFF", "CLEAR", "OFFSET", "RETURN" };
const char *valve_setting_name(int i) { return v_name[i]; }
void valve_settings(int32_t v[VALVE_N_SETTINGS]) { memcpy(v, v_set, sizeof v_set); }
const char *valve_settings_check(const int32_t v[VALVE_N_SETTINGS]) { (void)v; return NULL; }
bool valve_set_settings(const int32_t v[VALVE_N_SETTINGS]) { memcpy(v_set, v, sizeof v_set); return true; }
int32_t valve_pos_max(void) { return v_set[0]; }
void valve_uncalibrate(const char *why) { (void)why; v_cal = false; v_mode = VALVE_NOT_CAL; }
void valve_event(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
static bool vp_offer, vp_moving;
bool valvepos_offer(int32_t *c) { if (vp_offer) *c = 33880; return vp_offer; }
bool valvepos_use(const char **why) { (void)why; vp_offer = false; v_cal = true; v_ref = VALVE_REF_SAVED; v_mode = VALVE_MANUAL; return true; }
void valvepos_decline(void) { vp_offer = false; }
bool valvepos_offer_moving(int32_t *s) { *s = 400; return vp_offer && vp_moving; }

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

// A 0.8.x board: one version 1 record (bench cal of s/n 5880156), rev 5.
static void seed_flash(void)
{
    struct {
        uint32_t magic, version, size, rev;
        int32_t mot[7];
        int32_t valve[6];   // a version 1 record
        uint32_t has_cal;
        cal_rec_t cal;
        uint32_t crc;
    } r;
    memset(&r, 0, sizeof r);
    r.magic = 0x47464350UL; r.version = 1; r.size = sizeof r; r.rev = 5;
    int32_t mot[7] = { 400, 200, 3200, 1, 0, 0, 213 };
    memcpy(r.mot, mot, sizeof mot);
    memcpy(r.valve, v_set, sizeof r.valve);
    r.has_cal = 1;
    cal_rec_t *c = &r.cal;
    c->magic = CAL_MAGIC; c->version = CAL_VERSION; c->npts = 2;
    strcpy(c->id, "5880156 bench 26-10-05");
    c->r_ppb[0] = 127900; c->p_mmbar[0] = 0;
    c->r_ppb[1] = 9798300; c->p_mmbar[1] = 1013250;
    c->kind = CAL_KIND_BENCH;
    c->crc = crc32(c, offsetof(cal_rec_t, crc));
    r.crc = crc32(&r, offsetof(__typeof__(r), crc));
    extflash_write(0x7FD000, &r, sizeof r);
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
                !strcmp(st, "calpage") || !strcmp(st, "calhelp") || !strcmp(st, "faults") || !strcmp(st, "detail") || !strcmp(st, "diag") || !strcmp(st, "hot") ||
                !strcmp(st, "help") || !strcmp(st, "help2") || !strcmp(st, "help3") || !strcmp(st, "back") || !strcmp(st, "svc") ||
                !strncmp(st, "set-", 4) || !strncmp(st, "cal", 3) || strstr(st, "trend");
    if (!strcmp(st, "manual") || !strcmp(st, "moving") || !strcmp(st, "stop") || !strcmp(st, "keypad") ||
        !strcmp(st, "neg") || (page && strcmp(st, "valvenc"))) {
        v_mode = VALVE_MANUAL; v_cal = true; v_pct10 = 342;
    }
    if (!strcmp(st, "moving")) { extern bool fake_busy; fake_busy = moving = true; v_target10 = 500; }
    if (!strcmp(st, "calib") || !strcmp(st, "calpage2")) { v_mode = VALVE_CALIBRATING; v_step = 4; }
    if (!strcmp(st, "nodruck")) v_f01 = true;
    // AUTO (stage 8).
    bool aut = !strncmp(st, "auto", 4) || !strcmp(st, "paused") || !strncmp(st, "warn", 4) || !strcmp(st, "f08") || !strcmp(st, "trendauto");
    if (aut) { v_mode = VALVE_MANUAL; v_cal = true; v_pct10 = 342; v_sp_set = strcmp(st, "autofirst") != 0; }
    if (!strcmp(st, "auto") || !strcmp(st, "autostable") || !strcmp(st, "autoreg") || !strcmp(st, "autosp") ||
        !strcmp(st, "automan") || !strcmp(st, "autovalve")) v_mode = VALVE_AUTO;
    if (!strcmp(st, "autostable")) v_reg = CTRL_REG_STABLE;
    if (!strcmp(st, "autoreg")) { extern bool fake_busy; fake_busy = moving = true; v_target10 = 356; v_reg = CTRL_REG_REGULATING; }
    if (!strcmp(st, "paused")) v_warn = "W-2 Not settling: switched to MANUAL, valve idle";   // shown until OK
    if (!strcmp(st, "warn4")) v_warn = "W-4 Switch pressed at 48.04 %: check the switch";
    if (!strcmp(st, "warn5")) v_warn = "W-5 Pressure above the sensor range: switched to MANUAL";
    if (!strcmp(st, "warn6")) v_warn = "W-6 Valve driven the wrong way: check KDIR";
    if (!strcmp(st, "f08")) v_f16 = true;
    if (!strcmp(st, "faults") || !strcmp(st, "detail")) { v_f01 = true; v_f12 = true; }

    lv_init();
    // An hour of trend: pumped down from air, then held near 5 mbar with the
    // valve moved by hand (stage 7 previews). After lv_init, so the tick
    // that trend.c's one-second slots follow keeps running.
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
            lv_tick_inc(1000);   // one sample per second of real time (trend.c slots, 0.10.10)
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
    if (!strcmp(st, "splash")) {   // the start-up screen (0.13.3; logo from 0.13.13), part way
        ui_splash();
        ui_splash_step(65);
        for (int i = 0; i < 60; i++)
            ui_splash_tick();   // the bar caught up
        FILE *f = fopen(argv[2], "wb");
        if (!f) { perror(argv[2]); return 1; }
        fwrite(fb, sizeof fb, 1, f);
        fclose(f);
        return 0;
    }
    cal_init();
    cfg_init();
    ui_init();
    run(1200);

    // Heap check: Menu open and back ten times.
    size_t before = heap_used();
    for (int i = 0; i < 10; i++) {
        tap(415, 184);          // Menu
        tap(24, 20);            // Back
    }
    tap(300, 184);              // Go to box (opens the keypad in MANUAL)
    tap(24, 20);                // Back closes it
    for (int i = 0; i < 5; i++) {
        // The 0.6.0 motor screen first (review, 16 Oct: it stayed built and
        // a settings keypad after it ran the heap out). Its Back opens
        // Settings, with Menu behind.
        tap(415, 184); tap(379, 184); ui_service(); run(300);   // from Settings
        tap(40, 15); run(300);                                  // its Back: Settings
        tap(24, 20); tap(24, 20);                               // Menu, Home
        tap(415, 128); tap(24, 20);                             // Valve
        tap(415, 184); tap(350, 72); tap(24, 20); tap(24, 20);  // Faults and log
        tap(415, 184); tap(120, 128); tap(24, 20); tap(24, 20); // Calibrate valve
        tap(415, 184); tap(120, 184); tap(24, 20); tap(24, 20); // Diagnostics
        tap(415, 184); tap(379, 184);                           // Settings
        tap(120, 72); tap(24, 20);                              //   Cryostat name
        tap(356, 72); tap(24, 20);                              //   Pressure sensor
        tap(120, 128); tap(270, 72); tap(24, 20);               //   Valve setup, Drive tab
        tap(356, 128); tap(94, 72); tap(24, 20); tap(24, 20);   //   Control tuning, keypad
        tap(120, 184); tap(24, 20);                             //   Review and save
        tap(24, 20); tap(24, 20);
        tap(415, 184); tap(379, 128); tap(24, 20); tap(24, 20); // Calibrate sensor
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

    if (!strcmp(st, "stop")) tap(415, 240);
    if (!strncmp(st, "motor", 5)) { tap(415, 184); tap(379, 184); ui_service(); }   // Settings > Motor override
    if (!strcmp(st, "motor-held")) { extern bool fake_busy; fake_override = true; fake_busy = true; }   // Limit override held
    if (!strcmp(st, "motor-moving")) { extern bool fake_busy; fake_busy = true; ui_service(); }   // a move running
    if (!strcmp(st, "motor-speed")) tap(122, 72);
    if (!strcmp(st, "motor-steps")) { tap(301, 128); ui_service(); }   // 100 steps instead of Hold to run
    if (!strcmp(st, "motor-coils") || !strcmp(st, "motor-ask")) tap(194, 72);
    if (!strcmp(st, "motor-ask")) { v_cal = true; tap(120, 184); }   // Coil order while homed: asks
    if (!strcmp(st, "overon")) { fake_override = true; tap(415, 184); }   // Menu, override armed
    if (!strcmp(st, "menu")) tap(415, 184);
    if (!strcmp(st, "keypad")) { tap(300, 184); tap(41, 184); tap(111, 240); tap(251, 240); tap(111, 184); tap(111, 184); }
    if (!strcmp(st, "confirm")) tap(415, 72);
    if (!strcmp(st, "autofirst") || !strcmp(st, "autoask") || !strcmp(st, "automan") || !strcmp(st, "paused2")) tap(415, 72);
    if (!strcmp(st, "autosp")) { tap(115, 184); tap(41, 240); tap(111, 240); tap(321, 184); }   // Target 12: asks
    if (!strcmp(st, "autovalve")) tap(415, 128);
    if (!strcmp(st, "valve") || !strcmp(st, "valvenc") || !strcmp(st, "limithit")) tap(415, 128);
    if (!strcmp(st, "limithit")) v_q = true;
    if (!strcmp(st, "svc")) {
        size_t a = heap_used();
        tap(415, 184); tap(300, 240);     // Menu, Service (0.6.0 screens built)
        run(1200);
        size_t b = heap_used();
        ui_home(); run(600);
        size_t c = heap_used();
        tap(415, 128); run(600);           // Valve page
        size_t d = heap_used();
        tap(60, 184); run(600);            // Go to: keypad
        size_t f = heap_used();
        lv_mem_monitor_t m; lv_mem_monitor(&m);
        fprintf(stderr, "svc: home %zu, on service %zu, back home %zu, valve %zu, keypad %zu, peak %zu, total %zu\n",
                a, b, c, d, f, (size_t)m.max_used, (size_t)m.total_size);
    }
    if (!strcmp(st, "calset")) v_ref = VALVE_REF_HAND;   // opens on Set position
    if (!strcmp(st, "hot")) { extern bool fake_hot; fake_hot = true; }
    if (!strcmp(st, "handset")) { v_mode = VALVE_MANUAL; v_cal = true; v_pct10 = 342; v_ref = VALVE_REF_HAND; }
    if (!strcmp(st, "setpos")) { v_mode = VALVE_NOT_CAL; v_cal = false; tap(415, 72); tap(240, 146); }   // Home valve, Set position
    if (!strcmp(st, "bootpick")) { v_mode = VALVE_NOT_CAL; v_cal = false; vp_offer = true; }
    if (!strcmp(st, "bootask")) { v_mode = VALVE_NOT_CAL; v_cal = false; vp_offer = true; }
    if (!strcmp(st, "bootmove")) { v_mode = VALVE_NOT_CAL; v_cal = false; vp_offer = true; vp_moving = true; }
    // Settings (stage 6).
    bool set = !strncmp(st, "set-", 4);
    if (set) { tap(415, 184); tap(379, 184); }
    if (!strcmp(st, "set-sensor") || !strcmp(st, "set-pick") || !strncmp(st, "set-add", 7) ||
        !strcmp(st, "set-caldetail")) tap(356, 72);
    if (!strcmp(st, "set-pick")) tap(120, 72);
    if (!strncmp(st, "set-name", 8)) { tap(120, 72); tap(120, 72); tap(331, 189); tap(331, 189); tap(24, 65); tap(338, 65); }
    if (!strcmp(st, "set-named")) { tap(415, 128); tap(24, 20); tap(24, 20); tap(24, 20); }   // Use name, Settings, Home
    // System, Fallback address (0.12.13): keys at x 45/122/199, y 66/107/149/190.
    if (!strncmp(st, "set-net", 7)) tap(120, 72);
    if (!strcmp(st, "set-remote")) { tap(120, 72); tap(357, 184); }   // System, Remote control Off (On by default)
    if (!strncmp(st, "set-netaddr", 11) || !strcmp(st, "set-netused")) tap(356, 72);
    if (!strcmp(st, "set-netaddr2") || !strcmp(st, "set-netused")) {
        for (int i = 0; i < 13; i++) tap(199, 190);
        static const int16_t k1[][2] = { {45,66},{122,190},{45,190},{45,66},{45,190},{122,66},{45,190},{122,107},{122,190} };
        for (size_t i = 0; i < sizeof k1 / sizeof k1[0]; i++) tap(k1[i][0], k1[i][1]);   // 10.1.2.50
        tap(356, 128);
        for (int i = 0; i < 11; i++) tap(199, 190);
        static const int16_t k2[][2] = { {122,66},{122,107},{122,107},{45,190},{122,66},{122,107},{122,107},{45,190},
                                         {122,66},{122,107},{122,107},{45,190},{122,190} };
        for (size_t i = 0; i < sizeof k2 / sizeof k2[0]; i++) tap(k2[i][0], k2[i][1]);   // 255.255.255.0
    }
    if (!strcmp(st, "set-netused")) tap(298, 184);
    if (!strncmp(st, "set-add", 7)) { tap(298, 72); tap(41, 240); tap(111, 240); tap(181, 240); tap(321, 184); }
    if (!strcmp(st, "set-addask") || !strcmp(st, "set-addcal")) tap(250, 84);   // 30 psi (second column)
    if (!strcmp(st, "set-addcal")) tap(318, 164);   // Calibrate

    if (!strcmp(st, "set-caldetail")) tap(120, 128);
    if (!strncmp(st, "set-valve", 9)) tap(120, 128);
    if (!strcmp(st, "set-valve2")) tap(182, 72);
    if (!strcmp(st, "set-valve3")) tap(294, 72);
    if (!strcmp(st, "set-valvepos")) tap(94, 128);
    if (!strncmp(st, "set-tune", 8) || !strncmp(st, "set-save", 8)) tap(356, 128);
    if (!strcmp(st, "set-tunekp") || !strcmp(st, "set-tunedraft") || !strcmp(st, "set-saveask") ||
        !strcmp(st, "set-saved")) { tap(94, 72); tap(181, 240); tap(251, 240); tap(111, 184); }
    if (!strcmp(st, "set-tunedraft") || !strcmp(st, "set-saveask") || !strcmp(st, "set-saved")) tap(321, 184);
    if (!strcmp(st, "set-saveask") || !strcmp(st, "set-saved")) tap(415, 128);
    if (!strcmp(st, "set-saved")) tap(249, 136 + 40);
    if (!strncmp(st, "set-review", 10) || !strcmp(st, "set-load")) tap(120, 184);
    if (!strcmp(st, "set-load")) tap(120, 184);
    if (strstr(st, "trend")) { tap(415, 184); tap(210, 72); }   // Menu, Trend
    if (!strcmp(st, "trend1h")) { tap(415, 72); tap(415, 72); }
    if (!strcmp(st, "trend1m")) { tap(415, 184); tap(415, 184); tap(415, 184); }
    if (!strcmp(st, "calsensor") || !strcmp(st, "calvac")) { tap(415, 184); tap(379, 128); }
    if (!strcmp(st, "calvac")) tap(298, 128);
    if (!strcmp(st, "back")) { tap(415, 184); tap(120, 184); tap(24, 20); }   // Diagnostics, Back: Menu
    if (!strcmp(st, "help") || !strcmp(st, "help2") || !strcmp(st, "help3")) { tap(415, 128); tap(415, 184); }
    if (!strcmp(st, "help2")) tap(TVX, 172);
    if (!strcmp(st, "help3")) { tap(TVX, 172); tap(TVX, 172); tap(TVX, 172); tap(TVX, 172); }
    if (!strcmp(st, "faults") || !strcmp(st, "detail")) { tap(415, 184); tap(350, 72); }
    if (!strcmp(st, "detail")) tap(150, 72);
    if (!strcmp(st, "calpage") || !strcmp(st, "calpage2") || !strncmp(st, "calset", 6)) { tap(415, 184); tap(210, 128); }
    if (!strcmp(st, "calhelp")) { tap(415, 184); tap(210, 128); tap(415, 72); }
    if (!strcmp(st, "bootpick")) { run(600); tap(240, 146); }   // start-up question, Set position
    if (!strcmp(st, "diag")) { tap(415, 184); tap(200, 184); }
    run(1200);

    FILE *f = fopen(argv[2], "wb");
    if (!f) { perror(argv[2]); return 1; }
    fwrite(fb, sizeof fb, 1, f);
    fclose(f);
    return 0;
}
