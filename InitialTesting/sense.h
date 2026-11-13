#ifndef _SENSE_H_
#define _SENSE_H_

#include <stdbool.h>
#include <stdint.h>

// Once a second from the main loop: read the boards, update the checks.
void sense_second(void);

#endif // _SENSE_H_
