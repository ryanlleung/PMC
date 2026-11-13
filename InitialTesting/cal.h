#ifndef _CAL_H_
#define _CAL_H_

#include <stdbool.h>
#include <stdint.h>

#include "clicks.h"

/**
 * @brief Druck calibration: a table of ratio (signal / excitation) against
 * pressure, linear interpolation between points, linear extrapolation past
 * the ends from the end segments.
 *
 * The normal cal is the bench two-point: a pumped-down vacuum point and an
 * air point at a known room pressure, both read live through the INA228 as
 * wired, so the INA228 input loading of the Druck output (about 5 % low)
 * and the INA228 gain and offset are inside the table.
 *
 * Saved inside the settings record (cfg.c, CFG SAVE or CAL SAVE) and loaded
 * from it at start-up. With no settings record yet, the old stand-alone
 * record (0.3.30 to 0.5.x, at 0x7FF000) is loaded instead; the first save
 * copies it into the settings record and leaves the old one in place. If
 * neither holds a valid table, the nominal 0 mV / 100 mV at 10 V (15 psia)
 * is used and cal_is_default() is true. An old record from before 0.3.30
 * (versions 1 to 3, CAL ATM era) is not loaded: redo the bench cal.
 *
 * The calibration is taken on the Calibrate transducer screen: a vacuum
 * point and an air point, each the live ratio averaged over 5 readings;
 * the air point at the Pressure 21 room reading or a typed barometer
 * reading. Once both are held the table is applied (RAM only, until Save
 * settings); either can be redone alone.
 */

#define CAL_MAX_POINTS 32
#define CAL_ID_LEN     24

#define CAL_KIND_TABLE 0   // a table (the nominal one)
#define CAL_KIND_BENCH 1   // CAL VAC + CAL AIR

#define CAL_CAPTURE_N       5      // readings per bench capture point, one a second (Ryan, 22 Oct: was 10)

#define CAL_MAGIC       0x4C414350UL  // "PCAL"
#define CAL_VERSION     4   // 4: bench two-point; 1 to 3 (CAL ATM era) are not loaded

// One table. Little-endian, fixed layout, CRC32 (IEEE) over everything
// before the crc field. Kept whole inside the settings record (cfg.c).
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t npts;
    char id[CAL_ID_LEN];
    int32_t r_ppb[CAL_MAX_POINTS];     // ratio, ppb, strictly ascending
    int32_t p_mmbar[CAL_MAX_POINTS];   // pressure, 0.001 mbar
    int32_t kind;            // CAL_KIND_TABLE or CAL_KIND_BENCH
    // Unused since 0.11.37 (the point uncertainties, with UNC?); kept so
    // saved records load unchanged. Written 0, ignored on load.
    int32_t spare[2];
    uint32_t crc;
} cal_rec_t;

// Loads the old stand-alone record if valid; cfg_init() then overrides it.
void cal_init(void);
// True if the active table came from the old record at start-up.
bool cal_from_old_record(void);

// For cfg.c. The record of the active table, crc filled in.
void cal_get_record(cal_rec_t *r);
// Header, points and crc valid.
bool cal_record_ok(const cal_rec_t *r);
// Makes r the active table; false (nothing changed) if it is not valid.
bool cal_set_record(const cal_rec_t *r, bool saved);
// Makes the nominal table active.
void cal_set_default(bool saved);
void cal_set_saved(bool saved);
// Call once per reading; runs a CAL VAC / CAL AIR capture.
void cal_update(const clicks_state_t *s);

// Pressure in 0.001 mbar for a ratio in ppb (1 mV/V = 1 000 000 ppb).
int64_t cal_pressure_mmbar(int64_t r_ppb);

bool cal_is_default(void);
const char *cal_id(void);
// True once the active table has been written to flash (or was loaded from
// it), false after APPLY or a bench capture until CFG SAVE or CAL SAVE.
bool cal_saved(void);
// Readings still to take in a running capture, 0 if none.
int cal_capture_left(void);
// The running capture: 0 none, 1 vacuum, 2 air.
int cal_capturing(void);
// Starts a bench capture (1 vacuum, 2 air) at p_mmbar, as CAL VAC / CAL AIR.
// An air point at CAL_P_BARO takes the Pressure 21's reading, averaged
// over the same readings (0.14.0). False with *why if refused.
#define CAL_P_BARO (-1)
bool cal_capture(int what, int32_t p_mmbar, const char **why);
typedef struct { bool have; int32_t r_ppb, p_mmbar; } cal_point_t;
// The bench points held for the sensor in use (captured, or from its table).
void cal_points(cal_point_t *vac, cal_point_t *air);
// The sensor in use: its serial for the bench cal id and its range for the
// nominal table (0 to 10 mV/V is 0 to fs_mmbar).
void cal_set_sensor(const char *serial, int32_t fs_mmbar);
// Pressure for a ratio with a given table (the saved one, for comparison).
int64_t cal_pressure_with(const cal_rec_t *c, int64_t r_ppb);


#endif // _CAL_H_
