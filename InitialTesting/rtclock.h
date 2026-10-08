#ifndef _RTCLOCK_H_
#define _RTCLOCK_H_

#include <stdbool.h>

/**
 * @brief Wall clock on the STM32 RTC, clocked from the 32.768 kHz crystal
 * (X4 on the Mikromedia 4 schematic). The RTC sits in the backup domain
 * (VCC-RTC, from the BAT1 holder), so the time survives a reset, and a
 * power-off too if a cell is fitted.
 *
 * Local time, 24 h, no time zone or daylight saving handling. Until it has
 * been set the screen shows "time not set".
 *
 * Commands over the link (tools/logger/log_druck.py sends TIME SET from
 * the PC clock when it connects):
 *   TIME?                              show the time
 *   TIME SET YYYY-MM-DD HH:MM:SS       set it
 */

// Turns on the 32 kHz oscillator. Does not wait for it.
void rtclock_init(void);

// Finishes start-up once the oscillator runs. Call often.
void rtclock_poll(void);

// True once the time has been set (and the crystal is running).
bool rtclock_valid(void);

// "DD Mon  HH:MM" for the screen, or "time not set".
const char *rtclock_screen_text(void);

// "YYYY-MM-DD HH:MM:SS", or "" if not set.
const char *rtclock_iso(void);

// Handles one TIME command line. Returns false if it is not a TIME command.
bool rtclock_command(const char *line);

#endif // _RTCLOCK_H_
