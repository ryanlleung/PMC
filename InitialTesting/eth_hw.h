#ifndef _ETH_HW_H_
#define _ETH_HW_H_

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Board-level Ethernet set-up for ethlink.c: clocks, RMII pins, PHY
 * reset and a PHY probe. Register-level, kept apart from the TCP/IP stack
 * headers.
 */

#define ETH_HW_NO_REFCLK (-1)
#define ETH_HW_NO_PHY    (-2)

// SYSCLK to 150 MHz and 50 MHz on MCO1 (PA8). False if HSE does not start.
bool eth_hw_clock_50mhz(void);

// RMII pin mux, RMII select, PHY reset pulse. Called by the MAC driver.
void eth_hw_init_pins(void);

// Checks the REF_CLK and finds the PHY over MDIO. PHY address, or ETH_HW_NO_*.
int eth_hw_probe(uint32_t *phy_id);

// 96-bit device unique ID, for the MAC address.
void eth_hw_uid(uint8_t out[12]);

#endif // _ETH_HW_H_
