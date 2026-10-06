#ifndef _USB_SERIAL_H_
#define _USB_SERIAL_H_

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

#endif // _USB_SERIAL_H_
