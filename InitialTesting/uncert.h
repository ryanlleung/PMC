#ifndef _UNCERT_H_
#define _UNCERT_H_

#include <stdbool.h>
#include <stdint.h>

#include "clicks.h"

/**
 * @brief Uncertainty of the displayed pressure, from the known error sources,
 * combined root-sum-square. Each term is a limit (datasheet maximum, the
 * reference's stated uncertainty) or 2 standard deviations (noise), so the
 * total is roughly a 95 % figure, not a guaranteed bound.
 *
 * Included:
 *   Druck accuracy      +/-0.04 % FS (0.41 mbar), UNIK 5000 datasheet
 *   ATM reference       the barometer's uncertainty given to CAL ATM, scaled
 *                       by distance from the sensor's zero (a gain-type term:
 *                       full size at the reference, small near vacuum)
 *   INA228 offset       1 uV max (datasheet), through the table slope
 *   Noise               2 sd of the last 10 one-second readings; a real
 *                       pressure change also counts as noise here
 * Not included, listed by UNC?:
 *   INA228 gain errors (0.05 % shunt, 0.05 % bus): taken out by CAL ATM
 *   Druck thermal effects, and drift of the input-loading correction
 *   (RDIFF is typical only, no tempco): not known well enough to estimate
 *
 * UNC? prints the terms.
 */

// Call once per reading.
void uncert_update(const clicks_state_t *s);

// "+/- 0.45 mbar" style text for the screen; *warn set when the figure is
// missing or rests on an assumption.
const char *uncert_screen_text(bool *warn);

// Handles UNC?. Returns false if the line is not UNC?.
bool uncert_command(const char *line);

#endif // _UNCERT_H_
