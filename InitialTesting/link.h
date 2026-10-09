#ifndef _LINK_H_
#define _LINK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

// True once after a PC (re)connects, so one-off status lines can be resent.
bool link_take_new_client(void);

// Header chip: short text and whether the link is usable / in use.
const char *link_chip_text(void);
bool link_chip_ok(void);

// One-line state for the status lines.
const char *link_status(void);

/*
 * Line assembly, shared by the USB and Ethernet code. CR, LF or CRLF end a
 * line (terminals differ) and empty lines are ignored. Characters past the
 * buffer are dropped.
 */
typedef struct {
    char text[96];
    size_t len;
    uint32_t last_ms;    // lv_tick_get() at the last character that was not CR/LF
} link_line_t;

// Adds one received character. True with the line (no CR/LF) in out when it ends one.
bool link_line_feed(link_line_t *l, char c, char *out, size_t n);
// Ends a pending line once nothing has arrived for idle_ms. True with the line in out.
bool link_line_idle(link_line_t *l, uint32_t idle_ms, char *out, size_t n);

#endif // _LINK_H_
