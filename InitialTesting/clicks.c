#include <stdbool.h>
#include <string.h>

#include "mcu.h"
#include "board.h"
#include "drv_digital_in.h"
#include "drv_digital_out.h"
#include "drv_i2c_master.h"
#include "lvgl.h"  // lv_snprintf
#include "clicks.h"

#define STATUS_LEN 128

static char stepper3_status[STATUS_LEN];
static char boost10_status[STATUS_LEN];
static char powermonitor_status[STATUS_LEN];

/* --------------------------------------------------------------------------
 * Stepper 3 (S2)
 *
 * ULN2003 Darlington array: four inputs, no outputs back to the MCU, so the
 * board cannot be detected. All four inputs are driven low, which leaves
 * every coil off.
 * ------------------------------------------------------------------------ */
static digital_out_t stepper3_in[4];

static void stepper3_init(void)
{
    const pin_name_t pins[4] = { MIKROBUS_2_AN, MIKROBUS_2_RST,
                                 MIKROBUS_2_CS, MIKROBUS_2_PWM };

    for (int i = 0; i < 4; i++) {
        digital_out_init(&stepper3_in[i], pins[i]);
        digital_out_low(&stepper3_in[i]);
    }

    lv_snprintf(stepper3_status, STATUS_LEN,
                "S2 Stepper 3: pins set, coils off (no readback)");
}

/* --------------------------------------------------------------------------
 * Boost 10 (S3)
 *
 * The TPL0501 digipot is write-only, so the only signal back is PG (LT8337
 * power good, active low). There is no enable pin: the output is on
 * whenever the board is powered, at the digipot's power-up setting. CS is
 * held high so the digipot never sees a write. PG gets the MCU pull-up so a
 * missing board reads as "not good" rather than floating.
 * ------------------------------------------------------------------------ */
static digital_out_t boost10_cs;
static digital_in_t boost10_pg;

static void boost10_init(void)
{
    digital_out_init(&boost10_cs, MIKROBUS_3_CS);
    digital_out_high(&boost10_cs);

    digital_in_init(&boost10_pg, MIKROBUS_3_INT);
    // PG2 pull-up (PUPDR = 01).
    GPIOG->PUPDR = (GPIOG->PUPDR & ~(3UL << (2 * 2))) | (1UL << (2 * 2));
}

static void boost10_poll(void)
{
    if (digital_in_read(&boost10_pg) == 0)
        lv_snprintf(boost10_status, STATUS_LEN,
                    "S3 Boost 10: PG low = output in regulation");
    else
        lv_snprintf(boost10_status, STATUS_LEN,
                    "S3 Boost 10: PG high = not in regulation or no board");
}

/* --------------------------------------------------------------------------
 * Power Monitor (S4)
 *
 * INA228 on I2C2. Real detection: manufacturer ID 0x5449 ("TI") and device
 * ID 0x228x. The address depends on the A0/A1 jumpers (0x40-0x4F), so all
 * 16 are tried. The INA228 powers up converting shunt, bus and temperature
 * continuously, so the readings are live without writing any register.
 * ------------------------------------------------------------------------ */
#define INA228_REG_VSHUNT   0x04
#define INA228_REG_VBUS     0x05
#define INA228_REG_DIETEMP  0x06
#define INA228_REG_MFR_ID   0x3E
#define INA228_REG_DEV_ID   0x3F
#define INA228_MFR_TI       0x5449

static i2c_master_t pm_i2c;
static bool pm_i2c_open;
static uint8_t pm_addr;  // 0 = not found

static bool pm_read(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_write_then_read(&pm_i2c, &reg, 1, buf, len) == I2C_MASTER_SUCCESS;
}

static bool pm_probe(uint8_t addr)
{
    uint8_t buf[2];

    i2c_master_set_slave_address(&pm_i2c, addr);
    if (!pm_read(INA228_REG_MFR_ID, buf, 2))
        return false;
    if (((buf[0] << 8) | buf[1]) != INA228_MFR_TI)
        return false;
    if (!pm_read(INA228_REG_DEV_ID, buf, 2))
        return false;
    return ((buf[0] << 4) | (buf[1] >> 4)) == 0x228;
}

static void powermonitor_init(void)
{
    i2c_master_config_t cfg;

    i2c_master_configure_default(&cfg);
    cfg.scl = MIKROBUS_4_SCL;
    cfg.sda = MIKROBUS_4_SDA;
    cfg.speed = I2C_MASTER_SPEED_STANDARD;
    // i2c_master_open() returns the HAL acquire code: 1 on the first open,
    // 0 when already open, -1 on failure. Only -1 is an error.
    pm_i2c_open = (i2c_master_open(&pm_i2c, &cfg) != I2C_MASTER_ERROR);
    if (pm_i2c_open)
        i2c_master_set_timeout(&pm_i2c, 100);
}

static void powermonitor_poll(void)
{
    uint8_t b[3];

    if (!pm_i2c_open) {
        lv_snprintf(powermonitor_status, STATUS_LEN,
                    "S4 Power Monitor: I2C2 open failed");
        return;
    }

    if (pm_addr == 0) {
        for (uint8_t a = 0x40; a <= 0x4F && pm_addr == 0; a++)
            if (pm_probe(a))
                pm_addr = a;
        if (pm_addr == 0) {
            lv_snprintf(powermonitor_status, STATUS_LEN,
                        "S4 Power Monitor: NOT FOUND (no INA228 at 0x40-0x4F)");
            return;
        }
    }

    i2c_master_set_slave_address(&pm_i2c, pm_addr);

    // VSHUNT: 20-bit signed in bits 23..4, 312.5 nV/LSB (ADCRANGE 0).
    // VBUS: 20-bit in bits 23..4, 195.3125 uV/LSB.
    // DIETEMP: 16-bit signed, 7.8125 m degC/LSB.
    int32_t raw_shunt, raw_bus, raw_temp;

    if (!pm_read(INA228_REG_VSHUNT, b, 3)) goto lost;
    raw_shunt = (int32_t)(((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8)) >> 12;
    if (!pm_read(INA228_REG_VBUS, b, 3)) goto lost;
    raw_bus = (int32_t)((((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | b[2]) >> 4);
    if (!pm_read(INA228_REG_DIETEMP, b, 2)) goto lost;
    raw_temp = (int16_t)((b[0] << 8) | b[1]);

    // Integer formatting (lv_snprintf has no float): shunt in 0.1 uV,
    // bus in mV, temperature in 0.1 degC.
    int32_t shunt_0u1 = (raw_shunt * 3125) / 1000;  // 3.125 x 0.1 uV per LSB
    int32_t bus_mv = (int32_t)(((int64_t)raw_bus * 1953125) / 10000000);
    int32_t temp_d1 = (raw_temp * 5) / 64;           // 0.078125 x 0.1 C per LSB

    lv_snprintf(powermonitor_status, STATUS_LEN,
                "S4 Power Monitor: INA228 found @0x%02X\n"
                "   Vshunt %s%ld.%ld uV   Vbus %ld mV   die %s%ld.%ld C",
                pm_addr,
                shunt_0u1 < 0 ? "-" : "", (long)(LV_ABS(shunt_0u1) / 10), (long)(LV_ABS(shunt_0u1) % 10),
                (long)bus_mv,
                temp_d1 < 0 ? "-" : "", (long)(LV_ABS(temp_d1) / 10), (long)(LV_ABS(temp_d1) % 10));
    return;

lost:
    pm_addr = 0;
    lv_snprintf(powermonitor_status, STATUS_LEN,
                "S4 Power Monitor: lost (I2C read failed)");
}

/* --------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------ */
void clicks_init(void)
{
    stepper3_init();
    boost10_init();
    powermonitor_init();
    clicks_poll();
}

void clicks_poll(void)
{
    boost10_poll();
    powermonitor_poll();
}

const char *clicks_stepper3_status(void)     { return stepper3_status; }
const char *clicks_boost10_status(void)      { return boost10_status; }
const char *clicks_powermonitor_status(void) { return powermonitor_status; }
