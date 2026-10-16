#ifndef _VALVEPOS_H_
#define _VALVEPOS_H_

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Saved valve position (0.9.0). The step count lives in RAM; this
 * keeps a copy in the serial flash so a restart can offer it.
 *
 * The count is written once the valve is calibrated and has stood still
 * for 2 s; the moment a move starts (or the calibration is lost) an
 * "unsafe" entry is written, so a power cut mid-move leaves nothing to
 * offer. If that entry cannot be written (two tries), the move is refused. At start-up a safe entry is only offered: the screen asks
 * "Use saved X % / Calibrate" and nothing is taken without the operator.
 * Any move, or a calibration, withdraws the offer.
 *
 * Log of 16-byte entries over sixteen 4 KB sectors (0x7EC000-0x7FBFFF),
 * one erase per 256 entries, for wear.
 *
 * Commands:
 *   VALVE SAVED?    the saved entry and whether it is on offer
 *   VALVE RESTORE   take the offered count as the position
 */

void valvepos_init(void);      // after cfg_init()
void valvepos_task(void);      // every main-loop pass
// True while the start-up offer stands, with the saved count.
bool valvepos_offer(int32_t *count);
bool valvepos_use(const char **why);   // VALVE RESTORE
void valvepos_decline(void);           // the operator chose Calibrate
bool valvepos_command(const char *line);

#endif // _VALVEPOS_H_
