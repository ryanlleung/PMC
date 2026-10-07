#ifndef _LINK_H_
#define _LINK_H_

#include <stdbool.h>
#include <stddef.h>

/**
 * @brief Text link to the PC: the USB COM port or Ethernet (TCP), chosen by
 * PMC_LINK_ETHERNET in pmc_config.h. Same DATA lines and CAL commands
 * either way.
 */

void link_init(void);

// Call often from the main loop.
void link_task(void);

void link_printf(const char *fmt, ...);

// Non-blocking: true with one complete line (no CR/LF) in buf.
bool link_getline(char *buf, size_t n);

// Header chip: short text and whether the link is usable / in use.
const char *link_chip_text(void);
bool link_chip_ok(void);

// One-line state for the status lines.
const char *link_status(void);

#endif // _LINK_H_
