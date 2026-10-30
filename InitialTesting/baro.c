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
static uint8_t chip_id;
static int fails;
static int reprobe_s;
static int check_s;       // seconds to the next mode check
static int32_t p_mmbar, t_mc;
static char status[96];
static const char *why = "no reply at 0x46";   // why the last start failed

static bool rd(uint8_t reg, uint8_t *b, size_t n) { return clicks_i2c_read(BARO_ADDR, reg, b, n); }
static bool wr(uint8_t reg, uint8_t v) { return clicks_i2c_write(BARO_ADDR, reg, &v, 1); }

// The Bosch start-up order: chip ID, soft reset, reset complete and NVM
// ready, standby, oversampling, then normal mode.
static bool start(void)
{
    uint8_t b;
    if (!rd(REG_CHIP_ID, &b, 1))                 { why = "no reply at 0x46"; return false; }
    if (b != 0x50 && b != 0x51)                  { why = "0x46 answered but is not a BMP581"; return false; }
    chip_id = b;
    if (!wr(REG_CMD, CMD_SOFT_RESET))            { why = "soft reset not taken"; return false; }
    Delay_ms(3);   // 2 ms
    if (!rd(REG_INT_STATUS, &b, 1) || !(b & INT_POR_DONE)) { why = "soft reset did not complete"; return false; }
    if (!rd(REG_STATUS, &b, 1) || !(b & STATUS_NVM_RDY) || (b & STATUS_NVM_ERR)) { why = "NVM not ready"; return false; }
    if (!wr(REG_ODR_CONFIG, ODR_1HZ_STANDBY))    { why = "config write failed"; return false; }
    Delay_ms(3);   // 2.5 ms to standby
    if (!wr(REG_OSR_CONFIG, OSR_P64_T4))         { why = "config write failed"; return false; }
    if (!wr(REG_ODR_CONFIG, ODR_1HZ_NORMAL))     { why = "config write failed"; return false; }
    if (!rd(REG_OSR_EFF, &b, 1) || !(b & OSR_EFF_VALID)) { why = "oversampling too long for 1 Hz"; return false; }
    fails = 0;
    return true;
}

static void set_status(void)
{
    char p[16], t[16];
    switch (st) {
    case B_NOT_FITTED:
        lv_snprintf(status, sizeof status, "S1 Pressure 21: not fitted (%s)", why);
        break;
    case B_STARTING:
        lv_snprintf(status, sizeof status, "S1 Pressure 21: BMP58%d @0x%02X, waiting for the first reading",
                    chip_id == 0x51 ? 5 : 1, BARO_ADDR);
        break;
    case B_LOST:
        lv_snprintf(status, sizeof status, "S1 Pressure 21: lost (I2C read failed), retry every %d s", REPROBE_S);
        break;
    default:
        lv_snprintf(status, sizeof status, "S1 Pressure 21: BMP58%d @0x%02X  %s mbar  %s C%s", chip_id == 0x51 ? 5 : 1,
                    BARO_ADDR, fixed_fmt(p, sizeof p, fixed_round(p_mmbar, 1), 2),
                    fixed_fmt(t, sizeof t, fixed_round(t_mc, 1), 2),
                    st == B_IMPLAUSIBLE ? "  outside 500-1100 mbar" : "");
        break;
    }
}

void baro_init(void)
{
    fitted = start();
    st = fitted ? B_STARTING : B_NOT_FITTED;
    set_status();
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
        set_status();
    }
    // A BMP581 reset on its own (a supply glitch) comes back in standby and
    // keeps its last data: every 10 s, check it is still in normal mode.
    uint8_t m;
    if (++check_s >= REPROBE_S) {
        check_s = 0;
        if (rd(REG_ODR_CONFIG, &m, 1) && m != ODR_1HZ_NORMAL && start()) {
            st = B_STARTING;
            set_status();
            return;
        }
    }
    uint8_t b[6];
    if (!rd(REG_TEMP_XLSB, b, sizeof b)) {
        if (st == B_OK)
            st = B_STARTING;   // not used until it reads again
        if (++fails >= LOST_AFTER)
            st = B_LOST;
        set_status();
        return;
    }
    fails = 0;
    if (!b[3] && !b[4] && !b[5])
        return;   // no conversion finished yet (the first second after start)
    baro_convert(b, &p_mmbar, &t_mc);
    st = baro_plausible(p_mmbar) ? B_OK : B_IMPLAUSIBLE;
    set_status();
}

bool baro_reading(int32_t *p, int32_t *t)
{
    if (st != B_OK)
        return false;
    *p = p_mmbar;
    *t = t_mc;
    return true;
}

bool baro_fitted(void) { return fitted; }

#else

void baro_init(void) {}
void baro_poll(void) {}
bool baro_reading(int32_t *p, int32_t *t) { (void)p; (void)t; return false; }
bool baro_fitted(void) { return false; }
static const char status[] = "S1 Pressure 21: off in this build (PMC_BARO 0)";

#endif

const char *baro_status(void) { return status; }

#if PMC_ENG_COMMANDS
// BARO STATUS? (engineering): the Click's status line. BARO? (the reading)
// is in the remote set (remote.c).
bool baro_command(const char *line)
{
    if (strcmp(line, "BARO STATUS?") != 0)
        return false;
    link_printf("%s\r\nOK\r\n", status);
    return true;
}
#endif
