#ifndef LV_PORT_DISP_TEMPL_H
#define LV_PORT_DISP_TEMPL_H

#ifdef __cplusplus
extern "C"
{
#endif

#include "lvgl.h"

/* Initialize low level display driver: the backlight stays off. */
void lv_port_disp_init(void);

/* The backlight on, once the first screen is drawn. */
void lv_port_disp_light(void);

/* The backlight at a level, 0 (off) to 255 (full). */
void lv_port_disp_backlight(uint8_t level);

/* Enable updating the screen (the flushing process) when disp_flush() is called by LVGL
 */
void disp_enable_update(void);

/* Disable updating the screen (the flushing process) when disp_flush() is called by LVGL
 */
void disp_disable_update(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /*LV_PORT_DISP_TEMPL_H*/
