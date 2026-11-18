#ifndef _VALVEPOS_H_
#define _VALVEPOS_H_

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Saved valve position. The step count lives in RAM; this
 * keeps a copy in the serial flash so a restart can offer it.
 *
 * Once calibrated: a "safe" entry after 2 s at standstill; a "moving"
 * entry just before each move starts (the move is refused if it cannot be
 * written, two tries) and then every 2 s while it moves, so a power cut
 * mid-move loses at most about 2 s of travel. Calibration
 * lost: an "unsafe" entry, nothing to offer. At start-up a safe or moving
 * entry is only offered: the screen asks "Use saved X % / Home" (saying
 * when it was saved while moving) and nothing is taken without the
 * operator. Any move, or a calibration, withdraws the offer.
 *
 * Shut down (the valve at 0 %): while OFF a "parked" entry, written at
 * once, saying whether the valve was homed. Every start is OFF; if the
 * limit switch then reads released, the parked count is
 * taken without asking (homed: PARKED, else SAVED). Pressed: not homed,
 * nothing offered, the entry withdrawn.
 *
 * Log of 16-byte entries over sixteen 4 KB sectors (0x7EC000-0x7FBFFF),
 * one erase per 256 entries, for wear: at one entry every 2 s of motion the
 * 100,000 erases a sector allows last some 26 years of continuous moving.
 */

void valvepos_init(void);      // after cfg_init()
void valvepos_task(void);      // every main-loop pass
// True while the start-up offer stands, with the saved count.
bool valvepos_offer(int32_t *count);
// True if the offer is from a power-up after the last power-off without
// Shut down: the start-up question says so.
bool valvepos_offer_unparked(void);
// True if the offer was saved during a move; *steps is how far off it may
// be (2 s at the run rate).
bool valvepos_offer_moving(int32_t *steps);
// True if the power was lost in AUTO, with the
// setpoint then, 0.001 mbar.
bool valvepos_offer_auto(int32_t *sp_mmbar);
bool valvepos_use(const char **why);   // VALVE RESTORE
void valvepos_decline(void);           // the operator chose Calibrate

#endif // _VALVEPOS_H_
