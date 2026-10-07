#include "pmc_config.h"
#include "i2c_sdk_test.h"

#if PMC_I2C_SDK_TEST

#include "mcu.h"
#include "board.h"
#include "drv_i2c_master.h"
#include "lvgl.h"   // lv_snprintf

/* --------------------------------------------------------------------------
 * Bench check for why the mikroSDK I2C2 driver never saw the INA228.
 *
 * The SDK's "timeout" is a bare retry count with no delay: each wait for an
 * I2C event decrements it once per status read (~0.3 us at -O0). The old
 * clicks.c set it to 100, about 30 us, but the address byte alone takes
 * 90 us at 100 kHz, so every transfer timed out (error 1302) before the
 * INA228 could ACK. The driver also returns without sending STOP, so the
 * peripheral stays BUSY and every later call fails too.
 *
 * This reads the INA228 manufacturer ID (0x5449) through the SDK driver
 * twice, with timeout 100 and 10000, and reports both. Each run ends with a
 * STOP and a peripheral reset so the bit-banged driver starts clean.
 * ------------------------------------------------------------------------ */

#define PM_ADDR   0x4A
#define REG_MFR   0x3E

static void spin(uint32_t n)
{
    for (volatile uint32_t i = 0; i < n; i++);
}

static void try_read(uint16_t timeout, char *out, size_t n)
{
    i2c_master_t m;
    i2c_master_config_t cfg;
    uint8_t reg = REG_MFR, b[2] = { 0, 0 };

    i2c_master_configure_default(&cfg);
    cfg.scl = MIKROBUS_4_SCL;
    cfg.sda = MIKROBUS_4_SDA;
    cfg.addr = PM_ADDR;
    cfg.speed = I2C_MASTER_SPEED_STANDARD;
    if (i2c_master_open(&m, &cfg) == I2C_MASTER_ERROR) {
        lv_snprintf(out, n, "timeout %u: open failed", timeout);
        return;
    }
    i2c_master_set_timeout(&m, timeout);
    i2c_master_set_slave_address(&m, PM_ADDR);
    err_t e = i2c_master_write_then_read(&m, &reg, 1, b, 2);

    // Let any byte in flight finish, then STOP and reset I2C2.
    spin(200000);
    I2C2->CR1 |= I2C_CR1_STOP;
    spin(20000);
    i2c_master_close(&m);
    RCC->APB1RSTR |= RCC_APB1RSTR_I2C2RST;
    RCC->APB1RSTR &= ~RCC_APB1RSTR_I2C2RST;

    if (e == I2C_MASTER_SUCCESS)
        lv_snprintf(out, n, "timeout %u: OK, MFR 0x%02X%02X", timeout, b[0], b[1]);
    else
        lv_snprintf(out, n, "timeout %u: error %ld", timeout, (long)e);
}

static char result[160] = "";

void i2c_sdk_test_run(void)
{
    char a[64], b[64];
    try_read(100, a, sizeof a);
    try_read(10000, b, sizeof b);
    lv_snprintf(result, sizeof result, "I2C2 SDK test (INA228 0x4A): %s | %s", a, b);
}

const char *i2c_sdk_test_result(void)
{
    return result;
}

#else

void i2c_sdk_test_run(void) {}
const char *i2c_sdk_test_result(void) { return ""; }

#endif
