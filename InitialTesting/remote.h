#ifndef _REMOTE_H_
#define _REMOTE_H_

/**
 * @brief The remote command set on the text link (0.13.0, Ryan 27 Oct):
 * what an operator can use and judge away from the cryostat. Homing,
 * calibration, settings, tuning, saving and the motor are on the touch
 * screen. The full description is the "mm4 Remote Commands" page; README.md
 * has the list.
 *
 *   PRES SP <mbar>   MODE AUTO   MODE MANUAL   VALV SET <%>   STOP
 *   ALRM RESET   ALRM OK   TIME SET <YYYY-MM-DD> <HH:MM:SS>
 *   PRES?  PRES SP?  MODE?  VALV?  VALV MOVING?  BARO?  ALRM?  EVNT? [n]
 *   NETW?  TIME?  *IDN?
 *
 * Reply rules: no echo; nothing sent unasked; a query replies one line
 * starting with its own words ("PRES 4.625 mbar", "VALV NONE"); a set
 * command replies nothing unless refused; a refusal is "ERR <code> <text>".
 * Commands can share a line with ';': they run in order, the query replies
 * come back on one line joined by ';', and the first refusal ends the line.
 * EVNT? is the only multi-line reply ("EVNT <count>", then that many lines)
 * and must be on a line of its own.
 */

// One command line from the link (no CR or LF). LINK_LINE_LONG is a line
// that was cut at the buffer (ERR 103).
void remote_line(const char *line);

#endif // _REMOTE_H_
