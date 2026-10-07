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
#define PMC_LINK_ETHERNET 1

#define PMC_TCP_PORT      5000
#define PMC_HOSTNAME      "pmc"

// Address used if no DHCP server answers within PMC_DHCP_TIMEOUT_MS.
// Link-local (169.254.0.0/16), the range Windows gives itself on a direct
// cable with no DHCP, so a laptop plugged straight in can reach it.
#define PMC_FALLBACK_IP   "169.254.10.50"
#define PMC_FALLBACK_MASK "255.255.0.0"
#define PMC_DHCP_TIMEOUT_MS 10000

#endif // _PMC_CONFIG_H_
