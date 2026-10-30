#ifndef _BARO_H_
#define _BARO_H_

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Room pressure from a Pressure 21 Click (Bosch BMP581) in S1, on
 * the shared I2C2 with the Power Monitor (PMC_BARO in pmc_config.h).
 *
 * Probed at start-up at 0x46 (the Click default, 0x47 with its ADDR jumper
 * moved): CHIP_ID 0x50 (0x51 for a BMP585), soft reset, NVM ready. Then
 * normal mode at 1 Hz, pressure oversampling x64 and temperature x4, IIR
 * off (the cal capture averages its own readings). Read once a second
 * after the INA228. Not found at start-up: "not fitted", nothing else
 * changes and the bus is left alone. Three failed reads in a row: lost,
 * probed again every 10 s.
 *
 * Register map and start-up order from the Bosch BMP5 Sensor API
 * (bmp5_defs.h, bmp5.c): TEMP_DATA 0x1D-0x1F, PRESS_DATA 0x20-0x22,
 * INT_STATUS 0x27, STATUS 0x28, OSR_CONFIG 0x36, ODR_CONFIG 0x37,
 * OSR_EFF 0x38, CMD 0x7E.
 */

#define BARO_ADDR       0x46
#define BARO_MIN_MMBAR  500000    // plausible room pressure, 0.001 mbar
#define BARO_MAX_MMBAR  1100000

// Burst read from TEMP_DATA_XLSB: temperature 24-bit two's complement in
// 1/65536 degC, pressure 24-bit unsigned in 1/64 Pa (1 Pa = 10 x 0.001 mbar).
static inline void baro_convert(const uint8_t b[6], int32_t *p_mmbar, int32_t *t_mc)
{
    uint32_t t = ((uint32_t)b[2] << 16) | ((uint32_t)b[1] << 8) | b[0];
    int32_t ts = (t & 0x800000u) ? (int32_t)(t | 0xFF000000u) : (int32_t)t;
    uint32_t p = ((uint32_t)b[5] << 16) | ((uint32_t)b[4] << 8) | b[3];
    *t_mc = (int32_t)(((int64_t)ts * 1000) / 65536);
    *p_mmbar = (int32_t)(((int64_t)p * 10 + 32) / 64);
}

static inline bool baro_plausible(int32_t p_mmbar)
{
    return p_mmbar >= BARO_MIN_MMBAR && p_mmbar <= BARO_MAX_MMBAR;
}

void baro_init(void);
// Once a second, from clicks_poll().
void baro_poll(void);
// Latest reading: true while fitted, answering and plausible.
bool baro_reading(int32_t *p_mmbar, int32_t *t_mc);
bool baro_fitted(void);        // found at start-up
const char *baro_status(void); // one line, for BARO STATUS?
// BARO STATUS? (engineering)
bool baro_command(const char *line);

#endif // _BARO_H_
