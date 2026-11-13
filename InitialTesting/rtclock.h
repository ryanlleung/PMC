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

// "YYYY-MM-DD HH:MM:SS", or "" if not set.
const char *rtclock_iso(void);

// TIME SET (remote.c): p is "YYYY-MM-DD HH:MM:SS". NULL once set, else
// why not ("Expected: ...", "Outside 2000 to 2099", "Clock not running").
const char *rtclock_set_text(const char *p);

#endif // _RTCLOCK_H_
