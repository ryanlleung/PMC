/*
 * Host test of baro.h: the BMP581 burst read (TEMP_DATA_XLSB on, six bytes,
 * LSB first) to 0.001 mbar and 0.001 degC, and the 500-1100 mbar window.
 * Raw values from the scaling in the Bosch BMP5 Sensor API: pressure in
 * 1/64 Pa, temperature in 1/65536 degC, two's complement.
 */
#include <stdio.h>
#include <stdbool.h>

#include "baro.h"

static int fails;
#define CHECK(c, ...) do { bool ok_ = (c); printf("%s ", ok_ ? "PASS" : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)

static void conv(uint32_t raw_t, uint32_t raw_p, int32_t *p, int32_t *t)
{
    uint8_t b[6] = { (uint8_t)raw_t, (uint8_t)(raw_t >> 8), (uint8_t)(raw_t >> 16),
                     (uint8_t)raw_p, (uint8_t)(raw_p >> 8), (uint8_t)(raw_p >> 16) };
    baro_convert(b, p, t);
}

int main(void)
{
    int32_t p, t;

    conv(25u * 65536, 101325u * 64, &p, &t);
    CHECK(p == 1013250 && t == 25000, "1013.25 mbar at 25 C: %ld, %ld", (long)p, (long)t);

    conv((uint32_t)(-10 * 65536) & 0xFFFFFF, 12500u * 64, &p, &t);
    CHECK(p == 125000 && t == -10000, "125 mbar at -10 C (negative temperature): %ld, %ld", (long)p, (long)t);

    conv(0, 100000u * 64 + 1, &p, &t);
    CHECK(p == 1000000, "1/64 Pa rounds: %ld", (long)p);

    conv(0x7FFFFF, 0xFFFFFF, &p, &t);
    CHECK(p == 2621440 && t == 127999, "full scale: %ld, %ld", (long)p, (long)t);

    CHECK(baro_plausible(500000) && baro_plausible(1013250) && baro_plausible(1100000), "500, 1013.25, 1100 mbar plausible");
    CHECK(!baro_plausible(499999) && !baro_plausible(1100001) && !baro_plausible(0), "below 500, above 1100 and 0 not");

    printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
