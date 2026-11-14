#ifndef _LINK_H_
#define _LINK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pmc_config.h"

/**
 * @brief Text link to the PC over Ethernet (TCP port PMC_TCP_PORT).
 *
 * The transport (link_init, link_task, link_getline, link_tx_room,
 * link_client and link_send) is ethlink.c on the board. A host build with
 * PMC_LINK_ETHERNET 0 (the simulator) supplies those six itself.
 */

void link_init(void);

// Call often from the main loop.
void link_task(void);

// Sends s to the client, for link_write only.
void link_send(const char *s);

/*
 * Output. The board never sends a line by itself: the remote command set
 * (remote.c) sends its reply lines with link_write. Status changes, events
 * and saves from the screen go to the event log (EVENT?), not the link.
 */
void link_write(const char *s);

// Bytes link_write can send now without dropping or waiting.
size_t link_tx_room(void);

// Non-blocking: true with one complete line (no CR/LF) in buf.
bool link_getline(char *buf, size_t n);


/*
 * Remote traffic, for the header's remote light (0.15.14, Ryan 5 Nov):
 * requests in and replies out, text port and Modbus, counted since start.
 * remote.c counts the text port's lines in, link_write its replies out,
 * ethlink.c the Modbus requests and replies.
 */
void link_count(bool out);
void link_traffic(uint32_t *in, uint32_t *out);
// Milliseconds since the last request or reply; UINT32_MAX before the first.
uint32_t link_quiet_ms(void);
// A remote client is connected (the text port or Modbus).
bool link_client(void);

/*
 * Line assembly. CR, LF or CRLF end a
 * line (terminals differ) and empty lines are ignored. A line of up to
 * LINK_LINE_MAX characters is taken; a longer one comes out as
 * LINK_LINE_LONG (ERR 103), so nothing cut short is run.
 */
#define LINK_LINE_MAX  96
#define LINK_LINE_LONG "\x7f"
typedef struct {
    char text[LINK_LINE_MAX + 1];
    size_t len;
    bool over;           // characters were dropped
} link_line_t;

// Adds one received character. True with the line (no CR/LF) in out when it
// ends one; out needs LINK_LINE_MAX + 1 bytes.
bool link_line_feed(link_line_t *l, char c, char *out, size_t n);

#endif // _LINK_H_
