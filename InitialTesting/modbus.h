#ifndef _MODBUS_H_
#define _MODBUS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Modbus TCP server, protocol part (0.12.0). Plain C with no hardware
 * or socket calls, so tools/modbus-test runs it on the PC. ethlink.c feeds
 * it the bytes from each client on PMC_MODBUS_PORT; mbmap.c is the register
 * map (docs/modbus-register-map.md).
 *
 * Function codes: 1 read coils, 3 read holding registers, 5 write single
 * coil, 16 write multiple registers. Anything else answers exception 1.
 * The unit id is not checked and is echoed back.
 */

#define MODBUS_ADU_MAX 260   // MBAP header 7 + PDU 253

// Exception codes.
#define MODBUS_EX_FUNCTION 0x01   // function not supported, or remote writes off
#define MODBUS_EX_ADDRESS  0x02   // address outside the map, or half a float
#define MODBUS_EX_VALUE    0x03   // bad count or coil value
#define MODBUS_EX_REFUSED  0x04   // the controller refused the write (as on the screen)

/*
 * The register map (mbmap.c, or a stub in the host test). Addresses are
 * 0-based protocol addresses. mbmap_begin() is called once per request, so
 * a float read in one request comes from one snapshot.
 */
void mbmap_begin(void);
bool mbmap_read_reg(uint16_t addr, uint16_t *v);   // false: not in the map
bool mbmap_read_coil(uint16_t addr, bool *v);
// Returns 0 or a MODBUS_EX_ code. Called only when remote writes are on.
uint8_t mbmap_write_regs(uint16_t addr, uint16_t n, const uint16_t *v);
uint8_t mbmap_write_coil(uint16_t addr, bool v);
bool mbmap_writes_allowed(void);
// The client address for the NET events of its writes.
void mbmap_set_peer(const char *ip);

/*
 * One complete ADU in (MBAP + PDU), the reply ADU out. Returns the reply
 * length, or 0 if the frame is not Modbus TCP (wrong protocol id, length
 * field out of range): drop the connection then.
 */
size_t modbus_handle(const uint8_t *in, size_t n, uint8_t *out);

/*
 * Bytes in a client's buffer: 0 if more are needed, the ADU length once a
 * whole one is there, or (size_t)-1 if the header is not Modbus TCP.
 */
size_t modbus_frame_len(const uint8_t *buf, size_t n);

#endif // _MODBUS_H_
