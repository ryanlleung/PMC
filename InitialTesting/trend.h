#ifndef _TREND_H_
#define _TREND_H_

#include <stdbool.h>
#include <stdint.h>

#include "clicks.h"

/**
 * @brief Pressure and valve history for the Trend page and the Home
 * sparklines (stage 7). One sample a second for 10 minutes, and one every
 * 10 s (the mean of those ten) for an hour. RAM only: empty after a restart.
 */
#define TREND_FINE_N    600     // 1 s samples
#define TREND_COARSE_N  360     // 10 s samples
#define TREND_NONE      INT32_MIN

typedef struct {
    int32_t p;      // pressure, 0.001 mbar, or TREND_NONE (no reading)
    int32_t sp;     // target in AUTO, 0.001 mbar, or TREND_NONE (not in AUTO)
    int32_t v;      // valve, 0.01 %, or TREND_NONE (not calibrated)
} trend_pt_t;

void trend_second(const clicks_state_t *s);       // after each 1 s pressure read
uint32_t trend_seconds(void);                     // samples taken since start-up
// The 1 s sample age_s seconds ago (0 = newest), false if none.
bool trend_fine(uint32_t age_s, trend_pt_t *o);
// The 10 s sample covering age_s seconds ago, false if none.
bool trend_coarse(uint32_t age_s, trend_pt_t *o);
bool trend_command(const char *line);             // TREND?, TREND DUMP <s>
void trend_task(void);                            // main loop: sends a TREND DUMP

#endif // _TREND_H_
