#ifndef _SYSINFO_H_
#define _SYSINFO_H_

#include <stdbool.h>

/**
 * @brief Firmware version and the cause of the last reset.
 *
 * The reset cause comes from the RCC reset flags, read and cleared once at
 * start-up. It is shown on screen, sent with the status lines, and returned
 * by the VER? command.
 */

// Call first thing at start-up: reads and clears the RCC reset flags.
void sysinfo_init(void);

// e.g. "0.3.0 (Oct  7 2026 12:50:01)"
const char *sysinfo_version(void);

// "power-on", "reset pin", "watchdog", "brown-out", "software", ...
const char *sysinfo_reset_reason(void);

// True if the last reset was a fault (watchdog or brown-out).
bool sysinfo_reset_abnormal(void);

// One line for the status output: "PMC firmware ..., last reset: ...".
const char *sysinfo_line(void);

// Restarts the processor now (the screen's Restart, 0.12.19). The next
// start-up reports the reset as "software".
void sysinfo_restart(void);
// The chip's unique ID, 24 hex digits (the serial in *IDN?).
const char *sysinfo_chip_id(void);


#endif // _SYSINFO_H_
