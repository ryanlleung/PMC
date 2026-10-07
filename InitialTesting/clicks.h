#ifndef _CLICKS_H_
#define _CLICKS_H_

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Bring-up checks for the Click boards on the mikroBUS shield.
 *
 * Socket layout (Ryan, 7 Oct 2026):
 *   S2 Stepper 3     (ULN2003)  AN PB0, RST PC3, CS PA15, PWM PD13
 *   S3 Boost 10      (LT8337 + TPL0501)  SPI1 PA5/PA6/PB5, CS PF8, PG PG2
 *   S4 Power Monitor (INA228)   SCL PF1 / SDA PF0 (bit-banged I2C)
 *
 * Nothing here drives the motor. The Boost 10 is set to ~10 V at start-up
 * for the Druck excitation, and drops to ~5 V if VBUS reads over 11.5 V.
 */

// Latest readings for the UI, valid after clicks_init(), updated by clicks_poll().
typedef struct {
    bool pm_found;        // INA228 answering
    bool reading_ok;      // pm_found and excitation >= 7 V
    int32_t shunt_nv;     // Druck signal (Vshunt), nV
    int32_t bus_mv;       // excitation (Vbus), mV
    int32_t r_ppb;        // signal / excitation, ppb (1 mV/V = 1 000 000)
    int32_t p_mmbar;      // pressure, 0.001 mbar absolute
    int32_t die_mc;       // INA228 die temperature, 0.001 degC
    bool cal_nominal;     // true until the cert values are entered
    bool boost_pg;        // Boost 10 regulating
    bool boost_tripped;   // 11.5 V trip latched
    int32_t boost_trip_mv;
    int32_t boost_set_mv; // nominal setpoint for the wiper written
} clicks_state_t;

void clicks_init(void);

// Re-reads every board. Call about once a second.
void clicks_poll(void);

// One status line per board, valid after clicks_init().
const char *clicks_stepper3_status(void);

// Sets the Boost 10 (Druck excitation), clamped to 9.0-11.0 V nominal.
// Returns the nominal output in mV for the wiper written, or -1 if the
// 11.5 V trip has latched (no change until reset).
int32_t clicks_boost10_set_mv(int32_t mv);
const char *clicks_boost10_status(void);
const char *clicks_powermonitor_status(void);
const char *clicks_druck_status(void);

// Druck pressure in mbar absolute as text ("1013.25"), "----" if no reading.
const char *clicks_druck_value(void);
// True while the calibration is the nominal 0 mV / 100 mV, not the cert.
bool clicks_druck_cal_nominal(void);

const clicks_state_t *clicks_state(void);

#endif // _CLICKS_H_
