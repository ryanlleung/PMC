#ifndef _CAL_H_
#define _CAL_H_

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Druck calibration: a table of ratio (signal / excitation) against
 * pressure, linear interpolation between points, linear extrapolation past
 * the ends from the end segments. A zero/span sheet is the two-point case.
 *
 * Stored in the on-board serial flash (extflash.c) with a CRC32, loaded at
 * start-up. If nothing valid is stored, the nominal 0 mV / 100 mV at 10 V
 * (15 psia) is used and cal_is_default() is true.
 *
 * Set over COM3, one command per line (tools/cal/druck_cal.py does this):
 *   CAL?                       show the active calibration
 *   CAL NEW <id text>          start a new table (id up to 23 chars)
 *   CAL PT <mV/V> <mbar>       add a point, ratio ascending
 *   CAL APPLY                  check the new table and use it (RAM only)
 *   CAL ATM <mbar>             scale the active table's ratios so the
 *                              live reading equals <mbar> (corrects the
 *                              INA228 input loading; RAM only, then SAVE)
 *   CAL SAVE                   write the active table to flash
 *   CAL ERASE                  erase flash and go back to the default
 * Replies start with OK or ERR.
 */

#define CAL_MAX_POINTS 32
#define CAL_ID_LEN     24

void cal_init(void);

// Pressure in 0.001 mbar for a ratio in ppb (1 mV/V = 1 000 000 ppb).
int64_t cal_pressure_mmbar(int64_t r_ppb);

bool cal_is_default(void);
const char *cal_id(void);
// True once the active table has been written to flash (or was loaded from
// it), false after APPLY or ATM until CAL SAVE.
bool cal_saved(void);
// True if CAL ATM has scaled the active table (its id ends in "+atm").
bool cal_atm_applied(void);

// Handles one command line from COM3. Returns false if it is not a CAL command.
bool cal_command(const char *line);

#endif // _CAL_H_
