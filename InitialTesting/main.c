#include <string.h>
/**
 * @file main.c
 * @brief Main source file for the InitialTesting LVGL Designer application.
 *
 * This example demonstrates usage of the LVGL graphics library
 * for creating and running GUI applications with display and
 * touch support on embedded devices.
 */

#ifdef PREINIT_SUPPORTED
#include "preinit.h"
#endif

#ifdef __GNUC__
#include "delays.h"
#endif

#include "display_lvgl.h"
#include "lv_port_indev.h"
#include "1ms_Timer.h"
#include "pmc_config.h"
#include "link.h"
#include "sysinfo.h"
#include "rtclock.h"
#include "uncert.h"
#include "clicks.h"
#include "cal.h"
#include "stepper3.h"
#include "valve.h"
#include "cfg.h"
#include "ui.h"
#include "limit.h"
#include "sense.h"
#include "valvepos.h"
#include "trend.h"

/*
 * PERF?: how responsive the main loop is, since the last PERF?. Touch is
 * read inside lv_timer_handler, so a long handler or loop pass is input lag.
 */
extern uint32_t disp_flush_ms, disp_flush_px;
static uint32_t perf_since, perf_loops, perf_gap_max, perf_handler_max, perf_last_end;

static void perf_note(uint32_t t0)
{
    uint32_t now = lv_tick_get();
    uint32_t handler = now - t0;
    if (handler > perf_handler_max) perf_handler_max = handler;
    if (perf_last_end && now - perf_last_end > perf_gap_max) perf_gap_max = now - perf_last_end;
    perf_last_end = now;
    perf_loops++;
}

static bool perf_command(const char *line)
{
    if (strcmp(line, "PERF?") != 0)
        return false;
    uint32_t span = lv_tick_elaps(perf_since);
    lv_mem_monitor_t mem;
    lv_mem_monitor(&mem);
    link_printf("PERF over %lu ms: loops=%lu longest_loop_ms=%lu longest_lvgl_ms=%lu "
                "flush_ms=%lu flush_px=%lu\r\n",
                (unsigned long)span, (unsigned long)perf_loops, (unsigned long)perf_gap_max,
                (unsigned long)perf_handler_max, (unsigned long)disp_flush_ms,
                (unsigned long)disp_flush_px);
    // LVGL heap: peak is the most ever used since boot, not since the last PERF?.
    link_printf("HEAP total=%lu used=%lu peak=%lu biggest_free=%lu frag_pct=%u\r\nOK\r\n",
                (unsigned long)mem.total_size, (unsigned long)(mem.total_size - mem.free_size),
                (unsigned long)mem.max_used, (unsigned long)mem.free_biggest_size,
                (unsigned)mem.frag_pct);
    perf_since = lv_tick_get();
    perf_loops = perf_gap_max = perf_handler_max = 0;
    disp_flush_ms = disp_flush_px = 0;
    return true;
}

// HELP (or ?): every command the link takes. Keep in step with README.md.
// HELP prints the groups; HELP <group> prints one group, short enough to
// read in the terminal without scrolling.
static const char *const help_top[] = {
    "Commands (upper case, one per line, CR or LF ends a line). Groups:",
    "  HELP SYS     version, data lines, time, timing, diagnostics",
    "  HELP CFG     save and load settings",
    "  HELP CAL     pressure calibration",
    "  HELP VALVE   valve calibration, % position, STOP, faults, events",
    "  HELP AUTO    AUTO, target and PID tuning",
    "  HELP MOT     valve motor and limit switch (engineering)",
};
static const char *const help_valve[] = {
    "Valve (nothing moves at boot; VALVE CAL first):",
    "  VALVE?                     mode, position, settings, switch question",
    "  VALVE CAL                  calibrate to the closed switch (0 %)",
    "  VALVE CLOSED               take the valve as closed (0 %) where it stands",
    "  VALVE GOTO <0-100>         go to % open, one decimal",
    "  VALVE SETZERO | VALVE SKIP answer 'closed switch pressed': this is 0 % / keep the count",
    "  VALVE SAVED? | VALVE RESTORE  position saved at the last standstill / use it (at start-up only)",
    "  VALVE POSMAX|FHOME|FCREEP|BACKOFF|CLEAR|OFFSET <n>   settings (CFG SAVE keeps)",
    "  CTRL STOP                  stop now (latches F-12 in AUTO)",
    "  CTRL RESET | CTRL?         clear latches whose cause has gone / list them",
    "  EVT? [1-200]               last events (default 20)",
};
static const char *const help_auto[] = {
    "AUTO (pressure control; the valve homed, no fault, a target set):",
    "  CTRL SP <mbar>             target, 0.1 mbar, 0 to the sensor range",
    "  CTRL AUTO                  start or resume AUTO, bumpless",
    "  CTRL MAN [<0-100>]         to MANUAL, valve ramps down and holds [then goes to %]",
    "  PID?                       all control parameters",
    "  PID KP|TI|OUTMIN|DB|BAND <v>  %/mbar, min (0 off), %, steps, mbar (live; CFG SAVE keeps)",
    "  PID KDIR +1|-1             +1 opening raises the pressure",
    "In AUTO: F-02 above range 5 s, F-10 no response, F-16/F-17 wrong way, W-02 pauses after 1800 s",
};
static const char *const help_sys[] = {
    "System:",
    "  VER?                       firmware version and build time",
    "  DATA ON | DATA OFF         once-a-second DATA and status lines (off at boot)",
    "  TIME? | TIME SET YYYY-MM-DD HH:MM:SS",
    "  UNC?                       pressure uncertainty budget",
    "  TOUCH?                     touch controller counters",
    "  PERF?                      main loop and display timing (resets the maxima), LVGL heap",
    "  TREND? | TREND DUMP <1-600>  trend store / the last N one-second samples",
#if PMC_PM_DIAG
    "  PM DIAG                    sweep the excitation 9-11 V, then put it back",
#endif
};
static const char *const help_cfg[] = {
    "Settings (motor, valve, tuning, sensors and cals; changes are live, unsaved until saved):",
    "  CFG?                       saved records, what was loaded, unsaved count",
    "  CFG DIFF?                  settings that differ from the newest save",
    "  CFG SAVE                   save everything (CAL SAVE does the same)",
    "  CFG LOAD LAST|PREVIOUS     newest / previous save (refused while moving)",
    "  CFG LOAD DEFAULTS          factory motor, valve and tuning settings, sensors kept",
    "  CFG TEST CORRUPT|ERASE     bench test: spoil the newest record / erase both",
};
static const char *const help_cal[] = {
    "Calibration:",
    "  CAL?                       active cal and bench points",
    "  CAL VAC [mbar] [unc]       capture the vacuum point (default 0 mbar, 0-50)",
    "  CAL AIR <mbar> [unc]       capture the air point (500-1100 mbar)",
    "  CAL SAVE                   save all settings, cal included (= CFG SAVE)",
    "  CAL ERASE                  use nominal and save (old cal record erased)",
    "  CAL NEW [id] / CAL PT <mV/V> <mbar> / CAL APPLY   enter a table by hand",
};
static const char *const help_mot[] = {
    "Valve motor (+ opens, - closes):",
    "  MOT?                       position, target, settings, limit",
    "  MOT GOTO <pos> [sps]       absolute, own rate; retargets a running move",
    "  MOT MOVE <steps>           relative, 1 to 400000",
    "  MOT STOP [RAMP] | MOT OFF  stop now [ramped] / stop and de-energise",
    "  MOT ZERO                   call this position 0",
    "  MOT OVERRIDE [OFF]         next close ignores the limit (one move)",
    "  LIM?                       limit switch state",
    "Motor settings (refused during a move; CFG SAVE keeps):",
    "  MOT RATE <16-1000>         run rate, steps/s",
    "  MOT START <16-1000>        start rate, steps/s",
    "  MOT ACCEL <10-20000>       steps/s^2",
    "  MOT MODE WAVE|FULL|HALF    MOT HOLD 0|1    MOT DIR 0|1    MOT ORDER <abcd>",
};

static void help_print(const char *const *lines, size_t n)
{
    for (size_t i = 0; i < n; i++)
        link_printf("%s\r\n", lines[i]);
    link_printf("OK\r\n");
}
#define HELP_PRINT(a) help_print(a, sizeof a / sizeof a[0])

static bool help_command(const char *line)
{
    if (strcmp(line, "HELP") == 0 || strcmp(line, "?") == 0)
        HELP_PRINT(help_top);
    else if (strcmp(line, "HELP SYS") == 0)
        HELP_PRINT(help_sys);
    else if (strcmp(line, "HELP CFG") == 0)
        HELP_PRINT(help_cfg);
    else if (strcmp(line, "HELP CAL") == 0)
        HELP_PRINT(help_cal);
    else if (strcmp(line, "HELP VALVE") == 0 || strcmp(line, "HELP CTRL") == 0)
        HELP_PRINT(help_valve);
    else if (strcmp(line, "HELP AUTO") == 0 || strcmp(line, "HELP PID") == 0)
        HELP_PRINT(help_auto);
    else if (strcmp(line, "HELP MOT") == 0 || strcmp(line, "HELP LIM") == 0)
        HELP_PRINT(help_mot);
    else if (strncmp(line, "HELP ", 5) == 0)
        link_printf("ERR expected: HELP, HELP SYS, HELP CFG, HELP CAL, HELP VALVE, HELP AUTO or HELP MOT\r\n");
    else
        return false;
    return true;
}

/**
 * @brief Initializes board peripherals for display, touch, and LVGL timing.
 */
void board_init()
{
    lv_init();
    // A second LVGL heap pool in main SRAM: the 64 KB in CCM ran out with the
    // 0.6.0 Service screens built and the Valve page keypad open (0.8.1).
#if PMC_LINK_ETHERNET
    // 16 KB in the Ethernet build, whose network buffers need the SRAM: 80 KB
    // of LVGL heap in all, and the host previews peak at about 56 KB of it
    // (with 64-bit pointers; less on the board).
    static uint8_t lv_pool2[16 * 1024] __attribute__((aligned(8)));
#else
    static uint8_t lv_pool2[24 * 1024] __attribute__((aligned(8)));
#endif
    lv_mem_add_pool(lv_pool2, sizeof lv_pool2);
    lv_port_disp_init();
    lv_port_indev_init();

    // Configure 1ms timer interrupt for LVGL tick updates.
    timerInit();
}

/**
 * @brief Initializes the LVGL application and loads the main screen.
 */
void application_init()
{
    // Why the last reset happened (before anything else touches RCC).
    sysinfo_init();
    rtclock_init();

    // Initialize board peripherals and LVGL drivers.
    board_init();

    // Druck calibration from the on-board serial flash (nominal if none stored).
    cal_init();

    // Click boards on the shield. Stepper coils off; the motor only moves
    // on a MOT MOVE command.
    clicks_init();
    limit_init();
    valve_init();   // NOT CALIBRATED: nothing moves until VALVE CAL or a MOT command
    // Saved motor, valve and cal settings (factory defaults if none). Nothing moves.
    cfg_init();
    // The valve position saved at the last standstill: offered on the
    // screen (Use saved / Calibrate), never used by itself.
    valvepos_init();

    // The frame and Home. The 0.6.0 motor screen is built the first time
    // Menu > Motor opens it, which keeps its LVGL heap free until then.
    ui_init();

    // Draw the screen once first, so a link start-up fault cannot leave it blank.
    lv_timer_handler();

    // Text link to the PC (pmc_config.h): USB COM port (sets the 48 MHz USB
    // clock) or Ethernet (sets SYSCLK 150 MHz and the 50 MHz RMII clock).
    link_init();
}

/**
 * @brief Independent watchdog: resets the MCU if the main loop stops for
 * more than ~3-4 s (LSI 32 kHz nominal, 17-47 kHz; /64, reload 2000).
 * Covers hangs such as LV_ASSERT's while(1), HardFault and unbounded
 * hardware waits. Started after init; a software-started IWDG is stopped
 * by the reset it causes, so mikroBootloader is unaffected.
 */
static void watchdog_start(void)
{
    IWDG->KR = 0x5555;      // unlock PR/RLR
    IWDG->PR = 4;           // /64
    IWDG->RLR = 2000;
    IWDG->KR = 0xAAAA;      // reload
    IWDG->KR = 0xCCCC;      // start
}

static inline void watchdog_kick(void)
{
    IWDG->KR = 0xAAAA;
}

/**
 * @brief Application entry point.
 *
 * Initializes the MCU and LVGL environment, then enters
 * the main event loop for GUI processing.
 */
int main(void)
{
    /* Do not remove this line — it ensures correct MCU initialization. */
#ifdef PREINIT_SUPPORTED
    preinit();
#endif

    // Initialize the application.
    application_init();
    watchdog_start();

    ////////////////////////// LVGL timing routine (DO NOT REMOVE) //////////////////////////
    char line[96];
    uint32_t sense_ms = lv_tick_get();
    while (1)
    {
        link_task();
        touch_poll();
        rtclock_poll();
        limit_poll();
        stepper3_poll();
        valve_task();
        valvepos_task();
        trend_task();
        if (lv_tick_elaps(sense_ms) >= 1000) {
            sense_ms += 1000;
            if (lv_tick_elaps(sense_ms) >= 1000)   // fell behind: no catch-up burst
                sense_ms = lv_tick_get();
            sense_second();
        }
        bool got = link_getline(line, sizeof line);
        if (got)
            link_printf("> %s\r\n", line);   // echo, so the log shows what was received
        if (got && !help_command(line) && !cfg_command(line) && !cal_command(line) && !sysinfo_command(line) && !trend_command(line) && !rtclock_command(line) && !touch_command(line) && !perf_command(line) && !uncert_command(line) &&
            !limit_command(line) && !valvepos_command(line) && !valve_command(line) && !stepper3_command(line) && !clicks_command(line) &&
            !sense_data_command(line))
            link_printf("ERR unknown command\r\n");
        uint32_t t0 = lv_tick_get();
        lv_timer_handler();
        perf_note(t0);
        watchdog_kick();
        Delay_ms(1);
    }
    ////////////////////////////////////////////////////////////////////////////////////////

    return 0;
}

/**
 * @brief 1ms interrupt routine for the LVGL tick (and the CycloneTCP tick).
 *
 * Touch is no longer polled here; see touchpad_read() in lv_port_indev.c.
 */
static volatile uint32_t msCount = 0;

#if PMC_LINK_ETHERNET
extern volatile uint32_t systemTicks;   // CycloneTCP time base, 1 ms
#endif

INTERRUPT_ROUTINE
{
    msCount++;
#if PMC_LINK_ETHERNET
    systemTicks++;
#endif

    if (5 == msCount) {
        msCount = 0;
        lv_tick_inc(5);
    }

    CLEAR_FLAG;
}
