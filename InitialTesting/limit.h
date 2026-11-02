#ifndef _LIMIT_H_
#define _LIMIT_H_

#include <stdbool.h>

/**
 * @brief Valve limit switch input (pin and polarity in pmc_config.h).
 * Fail-safe closing stop: a trip counts at once, only the release is
 * debounced (20 ms). With PMC_LIMIT_STOP 1 the stepper refuses and stops
 * closing moves while it is open.
 *   LIM?   (engineering) state, raw pin level and changes since start-up
 */
typedef enum {
    LIMIT_UNKNOWN,   // not yet read steadily (first 20 ms); treated as open
    LIMIT_CLOSED,    // contact closed: healthy, valve away from the limit
    LIMIT_OPEN       // contact open: at the limit, or a broken wire
} limit_state_t;

void limit_init(void);
void limit_poll(void);           // main loop; samples every 5 ms
limit_state_t limit_state(void);
// True unless the contact is known closed and still reads closed now.
// Reads the pin directly; safe from the step interrupt.
bool limit_open_now(void);
// The pin alone, no debounce: the step interrupt's opening check counts
// steps of it itself, so one glitch (held 20 ms by limit_poll) is not a press.
bool limit_pin_open(void);
const char *limit_text(void);    // "closed", "OPEN" or "unknown"
bool limit_command(const char *line);

#endif // _LIMIT_H_
