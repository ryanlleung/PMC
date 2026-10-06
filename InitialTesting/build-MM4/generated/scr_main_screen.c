#include "scr_main_screen.h"



// Object setup.
void init_main_screen_ui(lvgl_main_screen_ui_t* ui)
{
    ui->main_screen = lv_obj_create(NULL);
lv_obj_set_style_pad_all(ui->main_screen, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
lv_obj_set_style_bg_color(ui->main_screen, lv_color_make(238, 238, 238), LV_PART_MAIN | LV_STATE_DEFAULT);
lv_obj_set_style_bg_grad_color(ui->main_screen, lv_color_make(238, 238, 238), LV_PART_MAIN | LV_STATE_DEFAULT);
lv_obj_set_style_bg_grad_dir(ui->main_screen, LV_GRAD_DIR_NONE, LV_PART_MAIN | LV_STATE_DEFAULT);
lv_obj_set_style_bg_color(ui->main_screen, lv_color_make(238, 238, 238), LV_PART_MAIN | LV_STATE_DEFAULT);
lv_obj_set_style_bg_grad_color(ui->main_screen, lv_color_make(238, 238, 238), LV_PART_MAIN | LV_STATE_DEFAULT);
lv_obj_set_style_bg_grad_dir(ui->main_screen, LV_GRAD_DIR_NONE, LV_PART_MAIN | LV_STATE_DEFAULT);

}

