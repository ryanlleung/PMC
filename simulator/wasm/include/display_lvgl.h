/* Simulator stand-in: the display is a frame buffer read by the page. */
#ifndef _SIM_DISPLAY_LVGL_H_
#define _SIM_DISPLAY_LVGL_H_
void lv_port_disp_init(void);
void lv_port_disp_light(void);   // the board's backlight on (0.15.7); nothing here
#endif
