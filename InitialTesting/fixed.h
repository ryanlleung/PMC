#ifndef _FIXED_H_
#define _FIXED_H_

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Integer maths and text for fixed-point values: integers scaled by
 * a power of ten, such as 0.001 mbar or ppb. lv_snprintf has no float
 * support, so every reading is printed through these.
 */

// "-12.345" for v = -12345, dec = 3: all dec decimals, a minus sign if v is
// negative. Returns buf.
const char *fixed_fmt(char *buf, size_t n, int64_t v, int dec);

// v with its last `drop` decimals rounded off, halves away from zero:
// fixed_round(12345, 1) = 1235, fixed_round(-12345, 1) = -1235.
// (Plain v / 10^drop truncates instead.)
int64_t fixed_round(int64_t v, int drop);

// Integer square root, rounded down.
uint32_t isqrt64(uint64_t v);

#endif // _FIXED_H_
