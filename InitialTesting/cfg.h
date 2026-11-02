#ifndef _CFG_H_
#define _CFG_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cal.h"

/**
 * @brief Settings store (stage 3 of the porting plan): the motor settings
 * (MOT RATE ... ORDER), the valve settings (VALVE POSMAX ... RETURN) and the
 * Druck calibration in one record with a revision number and a CRC32, in the
 * on-board serial flash (extflash.c).
 *
 * Two 4 KB sectors are used in turn: a save goes to the sector not holding
 * the newest record, is read back and checked, and only then counts as
 * saved, so a failed or interrupted save leaves the last good record. At
 * start-up the newest valid record is loaded; with none, the factory
 * defaults are used (and the old stand-alone cal record, if any) and an
 * event says so. Nothing moves at start-up either way.
 *
 * Version 2 (0.9.0) also holds the control tuning (used from stage 8), up
 * to four pressure sensors (serial, range, own cal; one in use) and the
 * save time. Version 1 records load as one sensor, s/n 5880156 at 15 psi.
 *
 * Changes made over COM3 are live at once and stay unsaved until CFG SAVE.
 * The Settings screens instead keep their edits as drafts and apply and
 * save them together with cfg_save_with(), all or nothing. A loaded record also comes in as live values;
 * anything differing from the newest saved record shows as unsaved.
 *
 * Commands:
 *   CFG?                         both sectors, what was loaded, unsaved count
 *   CFG DIFF?                    each setting that differs from the newest save
 *   CFG SAVE                     save everything (CAL SAVE does the same)
 *   CFG LOAD LAST|PREVIOUS       the newest / the other saved record
 *   CFG LOAD DEFAULTS            factory motor, valve and tuning settings
 *                                (sensors and their cals kept)
 *   CFG TEST CORRUPT             spoil the newest record's CRC (bench test)
 *   CFG NAME <text> | CFG NAME?  the cryostat name in the header (up to 12
 *                                characters; unsaved until CFG SAVE)
 *   CFG TEST ERASE               erase both sectors (bench test)
 * LOAD is refused while the valve moves or calibrates.
 */

// After cal_init(), stepper3_init() and valve_init().
void cfg_init(void);
// Saves the live settings; prints OK or ERR.
bool cfg_save(void);
// Number of settings differing from the newest saved record (or, with none
// saved, from the start-up values).
int cfg_unsaved(void);
// Engineering (PMC_ENG_COMMANDS): PID?, PID, NET?, NET IP, CFG ...
bool cfg_command(const char *line);

/* --------------------------------------------------------------------------
 * For the Settings screens
 * ------------------------------------------------------------------------ */
#define CFG_SENS_MAX 4
#define CFG_NAME_LEN 12

// Cryostat name, shown as "<name> pressure controller" on Home. Set live,
// unsaved until the next save, like a sensor change.
const char *cfg_name(void);
bool cfg_name_ok(const char *n);
bool cfg_set_name(const char *n, const char **why);

// Ethernet fallback address (0.12.13), a.b.c.d as a << 24 | b << 16 | c << 8
// | d; 0 0 is the build's PMC_FALLBACK_IP. Set live, unsaved until a save;
// the link uses the one loaded at start-up (cfg_net_boot).
bool cfg_net_parse(const char *s, uint32_t *v);          // "a.b.c.d"
const char *cfg_net_why(uint32_t ip, uint32_t mask);    // NULL if usable
const char *cfg_net_text(char *buf, size_t n, uint32_t ip, uint32_t mask);
void cfg_net(uint32_t *ip, uint32_t *mask);
bool cfg_set_net(uint32_t ip, uint32_t mask, const char **why);
void cfg_net_boot(char ip[16], char mask[16]);
bool cfg_net_restart_due(void);   // the live address differs from the one in use

// Remote control (0.13.4): Off, the network can only read (remote.c ERR 211,
// Modbus exception 1). On by default and in records before version 5 (0.13.7;
// Off to 0.13.6). Set live, unsaved until a save, like the name; a change is
// a NET event.
bool cfg_remote(void);
void cfg_set_remote(bool on);

// Settings with a field on the screens. CFG_KP to CFG_BAND stay in order.
typedef enum {
    CFG_POSMAX,    // steps for 100 %
    CFG_REVERSE,   // motor direction, 0 / 1 (a change clears the calibration)
    CFG_BACKOFF,   // steps
    CFG_RETURN,    // steps off the switch after the slow close, homing step 4 (0.13.6)
    CFG_FHOME,     // reference speed, steps/s
    CFG_FCREEP,    // creep speed, steps/s
    CFG_RATE,      // run speed, steps/s
    CFG_START,     // start speed, steps/s
    CFG_ACCEL,     // steps/s2
    CFG_KP,        // gain, 0.01 %/mbar
    CFG_TI,        // integral time, 0.01 min (0 = off)
    CFG_OUTMIN,    // auto minimum, 0.1 %
    CFG_DB,        // deadband, steps
    CFG_KDIR,      // pressure response, +1 opening raises pressure, -1 lowers
    CFG_BAND,      // target band, 0.01 mbar
    CFG_SENSOR,    // serial number of the sensor in use
    CFG_N
} cfg_key_t;

typedef enum { CFG_FROM_LIVE, CFG_FROM_SAVED, CFG_FROM_PREVIOUS, CFG_FROM_DEFAULTS } cfg_from_t;

typedef struct {
    uint32_t serial;
    int32_t psi10;          // range, 0.1 psi
    int32_t fs_mmbar;       // the same in 0.001 mbar
    bool in_use;
    bool has_cal;           // false: nominal over the range
    bool saved;             // as in the newest save
    const cal_rec_t *cal;   // NULL without a cal; valid until the next call
} cfg_sensor_t;

int32_t cfg_psi10_mmbar(int32_t psi10);
// Writes one line per unsaved change the screens have no field for (new
// cals, sensors added or removed, serial-only settings); 0 if none.
int cfg_unsaved_other(char *buf, size_t n);
// A setting from the live values, a save or the defaults. False if that
// record does not exist (or CFG_SENSOR from the defaults, which keep it).
bool cfg_value(cfg_from_t from, cfg_key_t k, int32_t *v);
bool cfg_rev(cfg_from_t from, uint32_t *rev, const char **saved_at);
void cfg_range(cfg_key_t k, int32_t *lo, int32_t *hi);
int32_t cfg_tune(cfg_key_t k);   // live tuning value
// Applies the live values with v[k] for each set[k], checked together, then
// saves. Nothing changes unless all of it is good.
bool cfg_save_with(const int32_t v[CFG_N], const bool set[CFG_N], const char **why);
// Makes a save (or the defaults) the live values, unsaved until saved.
bool cfg_load_live(cfg_from_t from, const char **why);

int cfg_sensor_count(void);
int cfg_sensor_active(void);
bool cfg_sensor(int i, cfg_sensor_t *out);
// The cal of that sensor in the newest save: false if not in it, cal NULL
// if saved without a cal.
bool cfg_saved_cal(uint32_t serial, const cal_rec_t **cal);
bool cfg_sensor_add(uint32_t serial, int32_t psi10, const char **why);
bool cfg_sensor_remove(uint32_t serial, const char **why);
// Puts a sensor on the board in use now, with its own cal, unsaved until
// the next save (as after a capture). Refused while the valve moves.
bool cfg_sensor_use(uint32_t serial, const char **why);

#endif // _CFG_H_
