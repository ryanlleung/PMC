/* Simulator stand-in: touch comes from the page's mouse or finger. */
#ifndef _SIM_LV_PORT_INDEV_H_
#define _SIM_LV_PORT_INDEV_H_
#include <stdbool.h>
void lv_port_indev_init(void);
void touch_poll(void);
bool touch_command(const char *line);
#endif
