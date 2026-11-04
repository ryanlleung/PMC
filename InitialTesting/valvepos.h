#ifndef _VALVEPOS_H_
#define _VALVEPOS_H_

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Saved valve position (0.9.0). The step count lives in RAM; this
 * keeps a copy in the serial flash so a restart can offer it.
 *
 * Once calibrated: a "safe" entry after 2 s at standstill; a "moving"
 * entry just before each move starts (the move is refused if it cannot be
 * written, two tries) and then every 2 s while it moves, so a power cut
 * mid-move loses at most about 2 s of travel (Ryan, 22 Oct). Calibration
 * lost: an "unsafe" entry, nothing to offer. At start-up a safe or moving
 * entry is only offered: the screen asks "Use saved X % / Home" (saying
 * when it was saved while moving) and nothing is taken without the
 * operator. Any move, or a calibration, withdraws the offer.
 *
 * Shut down (0.15.0): while OFF a "parked" entry, written at once. At the
 * next start-up, if the limit switch reads pressed, the parked count is
 * taken without asking (the needle is still on the seat): MANUAL after a
 * power-up, OFF again after a restart while off. Released: not homed,
 * nothing offered, the entry withdrawn.
 *
 * Log of 16-byte entries over sixteen 4 KB sectors (0x7EC000-0x7FBFFF),
 * one erase per 256 entries, for wear: at one entry every 2 s of motion the
 * 100,000 erases a sector allows last some 26 years of continuous moving.
 *
 * Commands:
 *   VALVE SAVED?    the saved entry and whether it is on offer
 *   VALVE RESTORE   take the offered count as the position
 */

void valvepos_init(void);      // after cfg_init()
void valvepos_task(void);      // every main-loop pass
// True while the start-up offer stands, with the saved count.
bool valvepos_offer(int32_t *count);
// True if the offer is from a power-up after the last power-off without
// Shut down (0.15.0): the start-up question says so.
bool valvepos_offer_unparked(void);
// True if the offer was saved during a move; *steps is how far off it may
// be (2 s at the run rate).
bool valvepos_offer_moving(int32_t *steps);
bool valvepos_use(const char **why);   // VALVE RESTORE
void valvepos_decline(void);           // the operator chose Calibrate

#endif // _VALVEPOS_H_
