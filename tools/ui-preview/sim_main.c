/*
 * Host render of the InitialTesting screen: builds the real main_screen.c
 * and generated designer code against LVGL 9.4 on a PC, with fake Click
 * readings (fake_hw.c), and writes one 480 x 272 frame as raw RGB565.
 *
 *   ./ui_preview <scenario> <out.raw>     scenario: ok | nopm | lowexc | trip | cal | unsaved
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "screens.h"
#include "clicks.h"

#define W 480
#define H 272

static uint16_t fb[W * H];
static uint8_t draw_buf[W * H * 2] __attribute__((aligned(4)));

void fake_hw_set_scenario(const char *name);

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    const uint16_t *src = (const uint16_t *)px;
    for (int32_t y = a->y1; y <= a->y2; y++)
        for (int32_t x = a->x1; x <= a->x2; x++)
            fb[y * W + x] = *src++;
    lv_display_flush_ready(d);
}

/* Exercise the font actually attached to the pressure label, including
 * digit-pair kerning and crossing 999.99 -> 1000.00. */
static void check_pressure_spacing(lv_obj_t *obj, int *found)
{
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const lv_font_t *font = lv_obj_get_style_text_font(obj, LV_PART_MAIN);
        if (font->line_height == lv_font_montserrat_48.line_height) {
            (*found)++;
            for (char c = '0'; c <= '9'; c++) {
                for (char next = '0'; next <= '9'; next++)
                    assert(lv_font_get_glyph_width(font, c, next) == 32);
                assert(lv_font_get_glyph_width(font, c, '.') == 32);
            }
            const char *values[] = { "111.11", "888.88", "999.99", "1000.00", "-1.23" };
            int32_t decimal_x = -1;
            for (unsigned i = 0; i < sizeof values / sizeof values[0]; i++) {
                lv_point_t full, prefix;
                char integer[16];
                size_t n = (size_t)(strchr(values[i], '.') - values[i]);
                memcpy(integer, values[i], n);
                integer[n] = '\0';
                lv_text_get_size(&full, values[i], font, 0, 0, 1000, LV_TEXT_FLAG_NONE);
                lv_text_get_size(&prefix, integer, font, 0, 0, 1000, LV_TEXT_FLAG_NONE);
                int32_t x = lv_obj_get_content_width(obj) - full.x + prefix.x;
                if (i == 0) decimal_x = x;
                assert(x == decimal_x);
            }
        }
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++)
        check_pressure_spacing(lv_obj_get_child(obj, i), found);
}

static lv_obj_t *find_label(lv_obj_t *obj, const char *text)
{
    if (lv_obj_check_type(obj, &lv_label_class) && !strcmp(lv_label_get_text(obj), text))
        return obj;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *found = find_label(lv_obj_get_child(obj, i), text);
        if (found) return found;
    }
    return NULL;
}

static void check_detail_tabs(bool show_raw)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_t *raw_tab = find_label(scr, "Chip readings");
    lv_obj_t *cal_tab = find_label(scr, "Calibration");
    lv_obj_t *raw_field = find_label(scr, "INA228 signal");
    lv_obj_t *cal_field = find_label(scr, "Druck serial number");
    assert(raw_tab && cal_tab && raw_field && cal_field);
    lv_obj_t *raw_panel = lv_obj_get_parent(raw_field);
    lv_obj_t *cal_panel = lv_obj_get_parent(cal_field);
    assert(lv_obj_has_flag(raw_panel, LV_OBJ_FLAG_HIDDEN));
    lv_obj_send_event(lv_obj_get_parent(raw_tab), LV_EVENT_CLICKED, NULL);
    assert(!lv_obj_has_flag(raw_panel, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(cal_panel, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_state(lv_obj_get_parent(raw_tab), LV_STATE_CHECKED));
    lv_obj_send_event(lv_obj_get_parent(cal_tab), LV_EVENT_CLICKED, NULL);
    assert(!lv_obj_has_flag(cal_panel, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(raw_panel, LV_OBJ_FLAG_HIDDEN));
    assert(!lv_obj_has_state(lv_obj_get_parent(raw_tab), LV_STATE_CHECKED));
    if (show_raw) lv_obj_send_event(lv_obj_get_parent(raw_tab), LV_EVENT_CLICKED, NULL);
}

int main(int argc, char **argv)
{
    if (argc != 3 && argc != 4) {
        fprintf(stderr, "usage: %s ok|nopm|lowexc|trip|cal out.raw [raw]\n", argv[0]);
        return 2;
    }
    fake_hw_set_scenario(argv[1]);

    lv_init();
    lv_display_t *d = lv_display_create(W, H);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d, draw_buf, NULL, sizeof draw_buf, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(d, flush_cb);

    clicks_init();
    init_screens();
    show_main_screen();

    // Let the 1 s status timer run twice, then render.
    for (int i = 0; i < 3; i++) {
        lv_tick_inc(1100);
        lv_timer_handler();
    }
    check_detail_tabs(argc == 4 && !strcmp(argv[3], "raw"));
    lv_obj_update_layout(lv_screen_active());
    int pressure_labels = 0;
    check_pressure_spacing(lv_screen_active(), &pressure_labels);
    assert(pressure_labels == 1);
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(d);

    FILE *f = fopen(argv[2], "wb");
    if (!f) { perror(argv[2]); return 1; }
    fwrite(fb, sizeof fb, 1, f);
    fclose(f);
    return 0;
}
