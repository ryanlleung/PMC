#ifndef _CAL_H_
#define _CAL_H_

#include <stdbool.h>
#include <stdint.h>

#include "clicks.h"

/**
 * @brief Druck calibration: two points of ratio (signal / excitation)
 * against pressure, a straight line through them and past them.
 *
 * The cal is the bench two-point: a pumped-down vacuum point and an
 * air point at a known room pressure, both read live through the INA228 as
 * wired, so the INA228 input loading of the Druck output (about 5 % low)
 * and the INA228 gain and offset are inside the two points.
 *
 * Saved inside the settings record (cfg.c) and loaded from it at start-up.
 * With none saved, the nominal 0 mV / 100 mV at 10 V (15 psia) is used and
 * cal_is_default() is true.
 *
 * To 0.16.29 the record was a table of up to 32 points (version 4), of
 * which the bench cal used two; typed tables went with the engineering
 * commands. cal_from_v4() reads the two points out of one, for the version
 * 10 settings records. The stand-alone record at 0x7FF000 (0.3.30 to 0.5.x)
 * is no longer read (0.16.30).
 *
 * The calibration is taken on the Calibrate transducer screen: a vacuum
 * point and an air point, each the live ratio averaged over 5 readings;
 * the air point at the Pressure 21 room reading or a typed barometer
 * reading. Once both are held the table is applied (RAM only, until Save
 * settings); either can be redone alone.
 */


#define CAL_CAPTURE_N       5      // readings per bench capture point, one a second (Ryan, 22 Oct: was 10)

#define CAL_MAGIC       0x4C414350UL  // "PCAL"
#define CAL_VERSION     5   // 5: the two bench points (0.16.30); 4 is read through cal_from_v4()

// The two points. Little-endian, fixed layout, CRC32 (IEEE) over everything
// before the crc field. Kept whole inside the settings record (cfg.c).
typedef struct {
    uint32_t magic;
    uint32_t version;
    int32_t r_ppb[2];        // ratio, ppb: the vacuum point, then the air point (higher)
    int32_t p_mmbar[2];      // pressure, 0.001 mbar
    uint32_t crc;
} cal_rec_t;

// Version 4 (0.3.30 to 0.16.29), as it sits inside a version 10 settings
// record. Read only.
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t npts;
    char id[24];             // "5880156 bench 26-10-16": not kept (nothing showed it)
    int32_t r_ppb[32];
    int32_t p_mmbar[32];
    int32_t kind;
    int32_t spare[2];
    uint32_t crc;
} cal_rec_v4_t;
// The two points of a version 4 record as a version 5 one. False if it is
// not a valid version 4 record of exactly two points.
bool cal_from_v4(const cal_rec_v4_t *o, cal_rec_t *r);

// The nominal cal; cfg_init() then loads the saved one.
void cal_init(void);

// For cfg.c. The record of the active table, crc filled in.
void cal_get_record(cal_rec_t *r);
// Header, the two points and crc valid.
bool cal_record_ok(const cal_rec_t *r);
// Makes r the cal in use; false (nothing changed) if it is not valid.
bool cal_set_record(const cal_rec_t *r, bool saved);
// Makes the nominal cal the one in use.
void cal_set_default(bool saved);
void cal_set_saved(bool saved);
// Call once per reading; runs a CAL VAC / CAL AIR capture.
void cal_update(const clicks_state_t *s);

// Pressure in 0.001 mbar for a ratio in ppb (1 mV/V = 1 000 000 ppb).
int64_t cal_pressure_mmbar(int64_t r_ppb);

bool cal_is_default(void);
// True once the cal in use has been written to flash (or was loaded from
// it), false after a bench capture until the next save.
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
// The bench points held (captured, or from the saved cal).
void cal_points(cal_point_t *vac, cal_point_t *air);
// The sensor's full scale, 0.001 mbar: the 15 psia Druck, 1034.214 mbar.
// The nominal cal is 0 to 10 mV/V over it.
#define CAL_FS_MMBAR 1034214
// Pressure for a ratio with a given cal (the saved one, for comparison).
int64_t cal_pressure_with(const cal_rec_t *c, int64_t r_ppb);


#endif // _CAL_H_
