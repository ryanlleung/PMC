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
#include "usb_serial.h"
#include "clicks.h"
#include "cal.h"

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
    // Initialize board peripherals and LVGL drivers.
    board_init();

    // Druck calibration from the on-board serial flash (nominal if none stored).
    cal_init();

    // Click boards on the shield (no motor drive, no Boost 10 writes).
    clicks_init();

    // Initialize all available screens.
    init_screens();

    // Show the main screen.
    // To display another screen, call its respective show function.
    show_main_screen();

    // Draw the screen once first, so a USB start-up fault cannot leave it blank.
    lv_timer_handler();

    // USB virtual COM port for text output (also sets the 48 MHz USB clock).
    usb_serial_init();
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

    ////////////////////////// LVGL timing routine (DO NOT REMOVE) //////////////////////////
    char line[96];
    while (1)
    {
        usb_serial_task();
        if (usb_serial_getline(line, sizeof line) && !cal_command(line))
            usb_serial_printf("ERR unknown command\r\n");
        lv_timer_handler();
        Delay_ms(5);
    }
    ////////////////////////////////////////////////////////////////////////////////////////

    return 0;
}

/**
 * @brief 1ms interrupt routine for LVGL tick and touch processing.
 *
 * This routine is automatically triggered by the configured timer
 * and updates the LVGL internal tick counter and touch controller.
 */
static volatile uint32_t msCount = 0;

INTERRUPT_ROUTINE
{
    msCount++;

    if (5 == msCount) {
        msCount = 0;
        lv_tick_inc(5);
        process_tp();
    }

    CLEAR_FLAG;
}
