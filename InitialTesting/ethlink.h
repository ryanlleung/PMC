#ifndef _ETHLINK_H_
#define _ETHLINK_H_

#include <stdbool.h>
#include <stddef.h>

/**
 * @brief Ethernet text link: TCP server on PMC_TCP_PORT (pmc_config.h),
 * on a static address (no DHCP): the one saved under Menu > System,
 * else PMC_IP.
 * Uses the mikroSDK CycloneTCP stack and the board's LAN8720A.
 */

void ethlink_init(void);

// Call often from the main loop: runs the TCP/IP stack.
void ethlink_task(void);

// printf-style output to the connected client. Dropped if none.
void ethlink_printf(const char *fmt, ...);
// Free space in the client's TCP send buffer; large with no client
// (output is dropped at once then).
size_t ethlink_tx_room(void);

// Non-blocking line input from the client: true with one complete line.
bool ethlink_getline(char *buf, size_t n);

// True once after a new client connects (so status lines can be resent).
bool ethlink_take_new_client(void);

// MAC address as text ("02:3A:91:C4:5E:07"), made from the chip's unique
// ID, the same every boot; "" if the Ethernet hardware did not start.
const char *ethlink_mac(void);

bool ethlink_client_connected(void);
// Modbus TCP clients connected on PMC_MODBUS_PORT (modbus.h).
int ethlink_modbus_clients(void);

#endif // _ETHLINK_H_
