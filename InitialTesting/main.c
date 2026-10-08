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
#include "screens.h"
#include "pmc_config.h"
#include "link.h"
#include "sysinfo.h"
#include "rtclock.h"
#include "clicks.h"
#include "cal.h"
#include "stepper3.h"
#include "main_screen.h"

/**
 * @brief Initializes board peripherals for display, touch, and LVGL timing.
 */
void board_init()
{
    lv_init();
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

    // Initialize all available screens.
    init_screens();

    // Show the main screen.
    // To display another screen, call its respective show function.
    show_main_screen();

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
    while (1)
    {
        link_task();
        rtclock_poll();
        if (link_getline(line, sizeof line) && !cal_command(line) && !sysinfo_command(line) && !rtclock_command(line) &&
            !stepper3_command(line) && !clicks_command(line) &&
            !main_screen_data_command(line))
            link_printf("ERR unknown command\r\n");
        lv_timer_handler();
        watchdog_kick();
        Delay_ms(5);
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
