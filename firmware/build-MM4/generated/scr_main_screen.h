#ifndef main_screen_H
#define main_screen_H

#include "lvgl.h"

typedef struct {
	
	lv_obj_t* main_screen;
	lv_obj_t* switch_0;


} lvgl_main_screen_ui_t;






extern lvgl_main_screen_ui_t lvgl_main_screen_ui;

void init_main_screen_ui(lvgl_main_screen_ui_t* ui);


#endif
