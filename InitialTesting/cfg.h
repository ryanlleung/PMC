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
 * The record is version 12 from 0.17.0 (cfg.c). Version 10 and 11 records
 * (0.16.21 to 0.16.33) are read and converted; older ones are not read.
 *
 * Three things act on the record (0.16.31, Ryan 13 Nov: "saving is too
 * clunky, maybe just a save an undo, and a reset to factory is enough"):
 *   Save   cfg_save_with(): the screens' drafts and the settings in use,
 *          checked together, written, then put in use, all or nothing.
 *   Undo   cfg_undo(): the newest save put back in use.
 *   Reset  cfg_reset_factory(): the motor, valve and tuning settings back
 *          to factory and saved; the calibration, the name, the address and
 *          Remote control stay.
 * The name, the address and Remote control are set live and saved at once
 * with cfg_save_system(). A captured calibration and what Motor override
 * changes are in use at once and unsaved until the next Save.
 * The record before the newest stays in the other sector as the fallback
 * for a failed write; nothing loads it by choice (Load settings > Previous
 * save and Review and save went in 0.16.31).
 */

// After cal_init(), stepper3_init() and valve_init().
void cfg_init(void);
// Number of settings differing from the newest saved record (or, with none
// saved, from the start-up values).
int cfg_unsaved(void);

/* --------------------------------------------------------------------------
 * For the settings screens
 * ------------------------------------------------------------------------ */
#define CFG_NAME_LEN 12

// Cryostat name, shown as "<name> pressure controller" on Home. Set live;
// cfg_save_system() keeps it.
const char *cfg_name(void);
bool cfg_name_ok(const char *n);
bool cfg_set_name(const char *n, const char **why);

// Ethernet address (0.12.13; the only one from 0.14.9, no DHCP), a.b.c.d as
// a << 24 | b << 16 | c << 8 | d; 0 0 is the build's PMC_IP. Set live
// (cfg_save_system() keeps it); the link uses the one loaded at start-up
// (cfg_net_boot).
bool cfg_net_parse(const char *s, uint32_t *v);          // "a.b.c.d"
const char *cfg_net_why(uint32_t ip, uint32_t mask);    // NULL if usable
const char *cfg_net_text(char *buf, size_t n, uint32_t ip, uint32_t mask);
void cfg_net(uint32_t *ip, uint32_t *mask);
bool cfg_set_net(uint32_t ip, uint32_t mask, const char **why);
void cfg_net_boot(char ip[16], char mask[16]);
bool cfg_net_restart_due(void);   // the live address differs from the one in use

// Remote control (0.13.4): Off, the network can only read (remote.c ERR 211,
// Modbus exception 1). On by default and in records before version 5 (0.13.7;
// Off to 0.13.6). Set live (cfg_save_system() keeps it); a change is a NET
// event.
bool cfg_remote(void);
void cfg_set_remote(bool on);
// Saves the name, the address and Remote control as they are now, with the
// rest of the record as last saved (0.16.31): a calibration or a Motor
// override change in use but unsaved is not saved by the way.
bool cfg_save_system(const char **why);

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
    CFG_DB,        // deadband, steps, opening and closing (a separate closing one to 0.16.31)
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
    // A fourth PID set (0.16.14): set 4 from boundary 3.
    CFG_KP4, CFG_TI4, CFG_KD4,
    CFG_B3,        // boundary set 3 / set 4, 0.1 mbar
    // Disable switch (the pass band, valve.h) edges (0.16.21), % open, 0
    // to 100: From below To.
    CFG_PASS_LO, CFG_PASS_HI,
    CFG_SPT,       // motor steps per output turn (0.17.0): the turns shown; 100 % stays in steps
    CFG_N
} cfg_key_t;

typedef enum { CFG_FROM_LIVE, CFG_FROM_SAVED } cfg_from_t;

// A setting from the live values or the newest save. False if nothing is
// saved, or k is no key.
bool cfg_value(cfg_from_t from, cfg_key_t k, int32_t *v);
bool cfg_rev(cfg_from_t from, uint32_t *rev, const char **saved_at);
void cfg_range(cfg_key_t k, int32_t *lo, int32_t *hi);
#define CFG_TUNE_LAST CFG_PASS_HI
int32_t cfg_tune(cfg_key_t k);   // live tuning value
int32_t cfg_steps_per_turn(void);  // live CFG_SPT
// The key of PID set 1 to 4's KP (which 0), TI (1) or KD (2).
cfg_key_t cfg_pid_key(int set, int which);
// Applies the live values with v[k] for each set[k], checked together, then
// saves. Nothing changes unless all of it is good.
bool cfg_save_with(const int32_t v[CFG_N], const bool set[CFG_N], const char **why);
// The same checks, then the values put in use without saving (0.17.0: an
// edit on the screens is in use at once; Save keeps it, Undo drops it).
bool cfg_apply_with(const int32_t v[CFG_N], const bool set[CFG_N], const char **why);
// Undo: the newest save (with none, the start-up settings) back in use.
// Refused in AUTO and while the valve moves or homes.
bool cfg_undo(const char **why);
// Reset to factory: the motor, valve and tuning settings to factory, saved
// and put in use. The calibration in use, the name, the address and Remote
// control stay. Refused in AUTO and while the valve moves or homes.
bool cfg_reset_factory(const char **why);

// The sensor's full scale, 0.001 mbar: the 15 psia Druck, 1034.214 mbar
// (the setpoint's limit, the nominal conversion before a cal).
int32_t cfg_sensor_range_mmbar(void);
// The sensor's cal in the newest save: false with nothing saved, cal NULL
// if saved without a cal (nominal).
bool cfg_saved_cal(const cal_rec_t **cal);

#endif // _CFG_H_
