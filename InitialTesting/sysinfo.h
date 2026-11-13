#ifndef _SYSINFO_H_
#define _SYSINFO_H_

#include <stdbool.h>

/**
 * @brief The cause of the last reset, the restart and the chip's ID.
 *
 * The reset cause comes from the RCC reset flags, read and cleared once at
 * start-up. The saved valve position uses it (valvepos.c).
 */

// Call first thing at start-up: reads and clears the RCC reset flags.
void sysinfo_init(void);

// "power-on", "reset pin", "watchdog", "brown-out", "software", ...
const char *sysinfo_reset_reason(void);

// Restarts the processor now (the screen's Restart, 0.12.19). The next
// start-up reports the reset as "software".
void sysinfo_restart(void);
// The chip's unique ID, 24 hex digits (the serial in *IDN?).
const char *sysinfo_chip_id(void);


#endif // _SYSINFO_H_
