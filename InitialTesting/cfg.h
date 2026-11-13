#ifndef _CFG_H_
#define _CFG_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cal.h"

/**
 * @brief Settings store: the motor settings, the valve settings, the control
 * tuning, the sensor's bench cal, the cryostat name, the Ethernet address
 * and the Remote control switch in one record with a revision number and a
 * CRC32, in the on-board serial flash (extflash.c).
 *
 * Two 4 KB sectors are used in turn: a save goes to the sector not holding
 * the newest record, is read back and checked, and only then counts as
 * saved, so a failed or interrupted save leaves the last good record. At
 * start-up the newest valid record is loaded; with none, the factory
 * defaults are used and an event says so. Nothing moves at start-up either
 * way.
 *
 * The record is version 11 from 0.16.30 (cfg.c). Version 10 records
 * (0.16.21 to 0.16.29) are read and converted; older ones are not read.
 *
 * The Settings screens keep their edits as drafts and apply and save them
 * together with cfg_save_with(), all or nothing. The name, the address,
 * Remote control and a captured calibration are set live and stay unsaved
 * until the next save. A loaded record also comes in as live values;
 * anything differing from the newest saved record shows as unsaved.
 * Loading is refused while the valve moves or homes.
 */

// After cal_init(), stepper3_init() and valve_init().
void cfg_init(void);
// Saves the live settings; prints OK or ERR.
bool cfg_save(void);
// Number of settings differing from the newest saved record (or, with none
// saved, from the start-up values).
int cfg_unsaved(void);

/* --------------------------------------------------------------------------
 * For the settings screens
 * ------------------------------------------------------------------------ */
#define CFG_NAME_LEN 12

// Cryostat name, shown as "<name> pressure controller" on Home. Set live,
// unsaved until the next save, like a captured calibration.
const char *cfg_name(void);
bool cfg_name_ok(const char *n);
bool cfg_set_name(const char *n, const char **why);

// Ethernet address (0.12.13; the only one from 0.14.9, no DHCP), a.b.c.d as
// a << 24 | b << 16 | c << 8 | d; 0 0 is the build's PMC_IP. Set live, unsaved until a save;
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

// Settings with a field on the screens. CFG_KP to CFG_TUNE_LAST stay in
// order (cfg.c tune[]).
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
    CFG_BAND,      // target band, 0.01 mbar
    CFG_KD,        // derivative, 0.01 % per mbar/s (0 = off)
    // The PID table (0.15.15; always on, by the pressure): CFG_KP, CFG_TI
    // and CFG_KD are PID set 1 (low pressure), these sets 2 and 3.
    // (CFG_KDIR, the pressure response, and CFG_SCHED, the schedule type,
    // went in 0.16.30.)
    CFG_KP2, CFG_TI2, CFG_KD2,
    CFG_KP3, CFG_TI3, CFG_KD3,
    CFG_B1,        // boundary set 1 / set 2, 0.1 mbar
    CFG_B2,        // boundary set 2 / set 3, 0.1 mbar
    CFG_HYST,      // hysteresis on the pressure, % of the boundary
    CFG_DBC,       // deadband closing, steps (CFG_DB is the opening one)
    // A fourth PID set (0.16.14): set 4 from boundary 3.
    CFG_KP4, CFG_TI4, CFG_KD4,
    CFG_B3,        // boundary set 3 / set 4, 0.1 mbar
    // Disable switch (the pass band, valve.h) edges (0.16.21), % open, 0
    // to 100: From below To.
    CFG_PASS_LO, CFG_PASS_HI,
    CFG_N
} cfg_key_t;

typedef enum { CFG_FROM_LIVE, CFG_FROM_SAVED, CFG_FROM_PREVIOUS, CFG_FROM_DEFAULTS } cfg_from_t;

// Writes one line per unsaved change the screens have no field for (a new
// cal, the name, the address); 0 if none.
int cfg_unsaved_other(char *buf, size_t n);
// A setting from the live values, a save or the defaults. False if that
// record does not exist, or k is no key.
bool cfg_value(cfg_from_t from, cfg_key_t k, int32_t *v);
bool cfg_rev(cfg_from_t from, uint32_t *rev, const char **saved_at);
void cfg_range(cfg_key_t k, int32_t *lo, int32_t *hi);
#define CFG_TUNE_LAST CFG_PASS_HI
int32_t cfg_tune(cfg_key_t k);   // live tuning value
// The key of PID set 1 to 4's KP (which 0), TI (1) or KD (2).
cfg_key_t cfg_pid_key(int set, int which);
// Applies the live values with v[k] for each set[k], checked together, then
// saves. Nothing changes unless all of it is good.
bool cfg_save_with(const int32_t v[CFG_N], const bool set[CFG_N], const char **why);
// Makes a save (or the defaults) the live values, unsaved until saved.
bool cfg_load_live(cfg_from_t from, const char **why);

// The sensor's full scale, 0.001 mbar: the 15 psia Druck, 1034.214 mbar
// (the setpoint's limit, the nominal conversion before a cal).
int32_t cfg_sensor_range_mmbar(void);
// The sensor's cal in the newest save: false with nothing saved, cal NULL
// if saved without a cal (nominal).
bool cfg_saved_cal(const cal_rec_t **cal);

#endif // _CFG_H_
