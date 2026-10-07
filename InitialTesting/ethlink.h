#ifndef _ETHLINK_H_
#define _ETHLINK_H_

#include <stdbool.h>
#include <stddef.h>

/**
 * @brief Ethernet text link: TCP server on PMC_TCP_PORT (pmc_config.h),
 * address from DHCP, or PMC_FALLBACK_IP if no DHCP server answers.
 * Uses the mikroSDK CycloneTCP stack and the board's LAN8720A.
 */

void ethlink_init(void);

// Call often from the main loop: runs the TCP/IP stack.
void ethlink_task(void);

// printf-style output to the connected client. Dropped if none.
void ethlink_printf(const char *fmt, ...);

// Non-blocking line input from the client, as usb_serial_getline().
bool ethlink_getline(char *buf, size_t n);

// Header text: the IP, or a short fault ("NO CABLE", "NET NO CLK", ...).
const char *ethlink_chip_text(void);

// IPv4 address as text, "" until one is set.
const char *ethlink_ip(void);

bool ethlink_link_up(void);
bool ethlink_client_connected(void);

// Short text describing the link state.
const char *ethlink_status(void);

#endif // _ETHLINK_H_
