/*
 * Host render of the InitialTesting screen: builds the real main_screen.c
 * and generated designer code against LVGL 9.4 on a PC, with fake Click
 * readings (fake_hw.c), and writes one 480 x 272 frame as raw RGB565.
 *
 *   ./ui_preview <scenario> <out.raw>     scenario: ok | nopm | lowexc | trip
 */
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

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s ok|nopm|lowexc|trip out.raw\n", argv[0]);
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
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(d);

    FILE *f = fopen(argv[2], "wb");
    if (!f) { perror(argv[2]); return 1; }
    fwrite(fb, sizeof fb, 1, f);
    fclose(f);
    return 0;
}
