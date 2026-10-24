#ifndef _PMC_CONFIG_H_
#define _PMC_CONFIG_H_

/**
 * @brief Build options.
 *
 * PMC_LINK_ETHERNET
 *   1: DATA lines and CAL commands go over Ethernet (TCP port PMC_TCP_PORT).
 *      SYSCLK runs at 150 MHz for the 50 MHz RMII clock, and the USB COM
 *      port is off (the two clocks cannot share the PLL).
 *   0: USB COM port (COM3) as before, Ethernet off.
 * Flashing over USB-C with mikroBootloader works the same either way.
 */
#define PMC_LINK_ETHERNET 0

// Shown on screen and returned by VER?, with the build date and time.
#define PMC_FW_VERSION    "0.11.42"

// Valve limit switch: one NC dry contact to GND on S2 PWM, PD13.
// Pin level that means the contact is OPEN (at the limit or a broken wire).
// 1 for the bare contact with the pull-up; 0 for an inverting interface.
#define PMC_LIMIT_OPEN_LEVEL 1
// 1: MCU internal pull-up on the input (needed for the bare contact).
#define PMC_LIMIT_PULLUP  1
// 1: the limit switch is the closing stop: closing moves are refused, and
// stopped mid-move, while it reads open. 0: its state is only displayed.
#define PMC_LIMIT_STOP    1

// Motor 12 V sense (0.11.35): a 0-25 V divider module (30k / 7.5k, x5) from
// the motor supply to S2 RX, PA3 (Adapter Click IDC10 pad 8). 1: read, and
// moves are refused while it reads off (supply.h); 0: not fitted.
#define PMC_MOTOR_SENSE   1

// Serial number of the Druck in use, shown on the Calibration tab.
#define PMC_DRUCK_SERIAL  "5880156"

#define PMC_TCP_PORT      5000
#define PMC_HOSTNAME      "pmc"

// Address used if no DHCP server answers within PMC_DHCP_TIMEOUT_MS.
// Link-local (169.254.0.0/16), the range Windows gives itself on a direct
// cable with no DHCP, so a laptop plugged straight in can reach it.
#define PMC_FALLBACK_IP   "169.254.10.50"
#define PMC_FALLBACK_MASK "255.255.0.0"
#define PMC_DHCP_TIMEOUT_MS 10000


#endif // _PMC_CONFIG_H_
