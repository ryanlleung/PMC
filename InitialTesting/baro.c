#include <string.h>

#include "delays.h"
#include "lvgl.h"   // lv_snprintf
#include "pmc_config.h"
#include "clicks.h"
#include "fixed.h"
#include "link.h"
#include "baro.h"

#if PMC_BARO

#define REG_CHIP_ID    0x01
#define REG_TEMP_XLSB  0x1D   // 6 bytes: temperature, then pressure
#define REG_INT_STATUS 0x27
#define REG_STATUS     0x28
#define REG_OSR_CONFIG 0x36
#define REG_ODR_CONFIG 0x37
#define REG_OSR_EFF    0x38
#define REG_CMD        0x7E

#define CMD_SOFT_RESET 0xB6
#define INT_POR_DONE   0x10   // INT_STATUS: power-on or soft reset complete
#define STATUS_NVM_RDY 0x02
#define STATUS_NVM_ERR 0x04
#define OSR_EFF_VALID  0x80   // the ODR fits the oversampling

// ODR_CONFIG: deep_dis (bit 7) set, ODR 1 Hz (0x1C in bits 6-2), power
// mode in bits 1-0 (0 standby, 1 normal).
#define ODR_1HZ_STANDBY ((uint8_t)(0x80 | (0x1C << 2) | 0))
#define ODR_1HZ_NORMAL  ((uint8_t)(0x80 | (0x1C << 2) | 1))
// OSR_CONFIG: press_en (bit 6), pressure x64 (6 in bits 5-3), temperature
// x4 (2 in bits 2-0).
#define OSR_P64_T4      ((uint8_t)(0x40 | (6 << 3) | 2))

#define LOST_AFTER     3       // failed reads in a row
#define REPROBE_S      10

typedef enum { B_NOT_FITTED, B_STARTING, B_OK, B_IMPLAUSIBLE, B_LOST } bstate_t;

static bstate_t st = B_NOT_FITTED;
static bool fitted;
static int fails;
static int reprobe_s;
static int check_s;       // seconds to the next mode check
static int32_t p_mmbar, t_mc;

static bool rd(uint8_t reg, uint8_t *b, size_t n) { return clicks_i2c_read(BARO_ADDR, reg, b, n); }
static bool wr(uint8_t reg, uint8_t v) { return clicks_i2c_write(BARO_ADDR, reg, &v, 1); }

// The Bosch start-up order: chip ID, soft reset, reset complete and NVM
// ready, standby, oversampling, then normal mode.
static bool start(void)
{
    uint8_t b;
    if (!rd(REG_CHIP_ID, &b, 1))                 return false;   // no reply at 0x46
    if (b != 0x50 && b != 0x51)                  return false;   // 0x46 answered but is not a BMP581
    if (!wr(REG_CMD, CMD_SOFT_RESET))            return false;   // soft reset not taken
    Delay_ms(3);   // 2 ms
    if (!rd(REG_INT_STATUS, &b, 1) || !(b & INT_POR_DONE)) return false;   // soft reset did not complete
    if (!rd(REG_STATUS, &b, 1) || !(b & STATUS_NVM_RDY) || (b & STATUS_NVM_ERR)) return false;   // NVM not ready
    if (!wr(REG_ODR_CONFIG, ODR_1HZ_STANDBY))    return false;   // config write failed
    Delay_ms(3);   // 2.5 ms to standby
    if (!wr(REG_OSR_CONFIG, OSR_P64_T4))         return false;   // config write failed
    if (!wr(REG_ODR_CONFIG, ODR_1HZ_NORMAL))     return false;   // config write failed
    if (!rd(REG_OSR_EFF, &b, 1) || !(b & OSR_EFF_VALID)) return false;   // oversampling too long for 1 Hz
    fails = 0;
    return true;
}

void baro_init(void)
{
    fitted = start();
    st = fitted ? B_STARTING : B_NOT_FITTED;
}

void baro_poll(void)
{
    if (!fitted)
        return;   // not found at start-up: leave the bus alone
    if (st == B_LOST && fails >= LOST_AFTER) {
        if (++reprobe_s < REPROBE_S)
            return;
        reprobe_s = 0;
        if (!start())
            return;
        st = B_STARTING;
    }
    // A BMP581 reset on its own (a supply glitch) comes back in standby and
    // keeps its last data: every 10 s, check it is still in normal mode.
    uint8_t m;
    if (++check_s >= REPROBE_S) {
        check_s = 0;
        if (rd(REG_ODR_CONFIG, &m, 1) && m != ODR_1HZ_NORMAL && start()) {
            st = B_STARTING;
            return;
        }
    }
    uint8_t b[6];
    if (!rd(REG_TEMP_XLSB, b, sizeof b)) {
        if (st == B_OK)
            st = B_STARTING;   // not used until it reads again
        if (++fails >= LOST_AFTER)
            st = B_LOST;
        return;
    }
    fails = 0;
    if (!b[3] && !b[4] && !b[5])
        return;   // no conversion finished yet (the first second after start)
    baro_convert(b, &p_mmbar, &t_mc);
    st = baro_plausible(p_mmbar) ? B_OK : B_IMPLAUSIBLE;
}

bool baro_reading(int32_t *p, int32_t *t)
{
    if (st != B_OK)
        return false;
    *p = p_mmbar;
    *t = t_mc;
    return true;
}

#else

void baro_init(void) {}
void baro_poll(void) {}
bool baro_reading(int32_t *p, int32_t *t) { (void)p; (void)t; return false; }

#endif

