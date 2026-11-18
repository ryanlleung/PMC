/* SSD1963 pixel write loop, on its own so it can be built at -O2 (see
 * CMakeLists.txt). Do not include lvgl_common.h or other mikroSDK GPIO
 * headers here: their inline WR_LOW/WR_HIGH break when optimised.
 *
 * The 16-bit bus is all of GPIOE (LCD_TFT_16BIT_CH0, mask 0xFFFF) and WR is
 * PF11 (TFT_WR). At -O2 the nops set the timing: one for data setup before
 * WR falls, three for the WR low pulse, one for data hold after WR rises.
 * About 13 cycles (77 ns) a pixel at 168 MHz, against about 226 ns at -O0. */
#include "display_px.h"
#include "mcu.h"

void display_px_write(const uint16_t *px, int32_t w, int32_t h, int32_t stride)
{
    for (int32_t y = 0; y < h; y++) {
        const uint16_t *p = px;
        for (int32_t i = 0; i < w; i++) {
            GPIOE->ODR = *p++;
            __asm volatile ("nop");
            GPIOF->BSRR = 1UL << (11 + 16);   /* WR low */
            __asm volatile ("nop\n\tnop\n\tnop");
            GPIOF->BSRR = 1UL << 11;          /* WR high: SSD1963 latches */
            __asm volatile ("nop");
        }
        px += stride;
    }
}
