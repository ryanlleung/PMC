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

// 1: at boot, read the INA228 ID through the mikroSDK I2C2 driver (old
// timeout 100, then default 10000) and send the result with the status
// lines, before the bit-banged driver takes over. Bench diagnostic only.
#define PMC_I2C_SDK_TEST  1

// 1: "PM DIAG" command on the link: INA228 register readback and a sweep of
// conversion times, modes and excitation, one line a second. Harmless when
// not asked for; it moves the excitation 9-11 V and puts it back.
#define PMC_PM_DIAG       1

// Shown on screen and returned by VER?, with the build date and time.
#define PMC_FW_VERSION    "0.3.4"

#define PMC_TCP_PORT      5000
#define PMC_HOSTNAME      "pmc"

// Address used if no DHCP server answers within PMC_DHCP_TIMEOUT_MS.
// Link-local (169.254.0.0/16), the range Windows gives itself on a direct
// cable with no DHCP, so a laptop plugged straight in can reach it.
#define PMC_FALLBACK_IP   "169.254.10.50"
#define PMC_FALLBACK_MASK "255.255.0.0"
#define PMC_DHCP_TIMEOUT_MS 10000

#endif // _PMC_CONFIG_H_
