#ifndef _SENSE_H_
#define _SENSE_H_

#include <stdbool.h>
#include <stdint.h>

// Once a second from the main loop: read the boards, update the checks.
void sense_second(void);

// W-3 controller hot (0.11.36, Ryan 23 Oct): the Power Monitor die
// temperature at or above SENSE_HOT_MC, cleared below SENSE_COOL_MC. A
// warning only: not latched, blocks nothing, logged.
#define SENSE_HOT_MC   55000
#define SENSE_COOL_MC  50000
bool sense_hot(int32_t *die_mc);


#endif // _SENSE_H_
