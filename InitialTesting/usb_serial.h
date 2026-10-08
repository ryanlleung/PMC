#ifndef _USB_SERIAL_H_
#define _USB_SERIAL_H_

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/**
 * @brief USB CDC virtual COM port on the Mikromedia 4 USB-C connector.
 *
 * Lets the application print text to a PC terminal (NECTO UART Terminal,
 * PuTTY, etc.) with no debugger attached.
 */

void usb_serial_init(void);

// Call often from the main loop: services the USB stack.
void usb_serial_task(void);

// Short text describing the USB link state, for on-screen diagnostics.
const char *usb_serial_status(void);

// printf-style output to the COM port. Dropped until the PC has enumerated the port.
void usb_serial_printf(const char *fmt, ...);

// Non-blocking line input. Returns true with a complete line in buf (no CR/LF,
// NUL-terminated) once one has arrived; longer lines are cut to n - 1 chars.
bool usb_serial_getline(char *buf, size_t n);

// Bytes received from the PC since start-up (shown in the USB header chip).
uint32_t usb_serial_rx_bytes(void);

#endif // _USB_SERIAL_H_
