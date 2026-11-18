#ifndef DISPLAY_PX_H
#define DISPLAY_PX_H

#include <stdint.h>

/* Write h lines of w RGB565 pixels to the SSD1963 (window already set),
 * stepping stride pixels per line in the source buffer. */
void display_px_write(const uint16_t *px, int32_t w, int32_t h, int32_t stride);

#endif
