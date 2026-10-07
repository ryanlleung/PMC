#ifndef _MAIN_SCREEN_H_
#define _MAIN_SCREEN_H_

#include <stdbool.h>
#include "scr_main_screen.h"

void init_main_screen();

void show_main_screen();

// DATA ON / DATA OFF: start or pause the once-a-second DATA lines.
// Returns false if the line is not a DATA command.
bool main_screen_data_command(const char *line);

#endif // !__MAIN_SCREEN_H__H_
