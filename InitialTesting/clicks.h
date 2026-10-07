#ifndef _CLICKS_H_
#define _CLICKS_H_

/**
 * @brief Bring-up checks for the Click boards on the mikroBUS shield.
 *
 * Socket layout (Ryan, 7 Oct 2026):
 *   S2 Stepper 3     (ULN2003)  AN PB0, RST PC3, CS PA15, PWM PD13
 *   S3 Boost 10      (LT8337 + TPL0501)  SPI1 PA5/PA6/PB5, CS PF8, PG PG2
 *   S4 Power Monitor (INA228)   SCL PF1 / SDA PF0 (bit-banged I2C)
 *
 * Nothing here drives the motor or writes the Boost 10 digipot.
 */

void clicks_init(void);

// Re-reads every board. Call about once a second.
void clicks_poll(void);

// One status line per board, valid after clicks_init().
const char *clicks_stepper3_status(void);
const char *clicks_boost10_status(void);
const char *clicks_powermonitor_status(void);

#endif // _CLICKS_H_
