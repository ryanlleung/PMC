#include <stdbool.h>
#include <string.h>

#include "mcu.h"
#include "board.h"
#include "drv_digital_in.h"
#include "drv_digital_out.h"
#include "lvgl.h"  // lv_snprintf
#include "clicks.h"

#define STATUS_LEN 192

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
 * INA228 on the shield I2C lines (PF1 SCL, PF0 SDA). Driven by a bit-banged
 * I2C master: on the bench the mikroSDK I2C2 driver never got a reply, while
 * a bit-banged scan on the same pins found the INA228 at 0x4A, so the wiring
 * is good and the fault is in the driver path (cause not found).
 *
 * Real detection: manufacturer ID 0x5449 ("TI") and device ID 0x228x. The
 * address depends on the A0/A1 jumpers (0x40-0x4F), so all 16 are tried.
 * The INA228 powers up converting shunt, bus and temperature continuously,
 * so the readings are live without writing any register.
 * ------------------------------------------------------------------------ */
#define BB_SCL 1  // PF1
#define BB_SDA 0  // PF0

#define INA228_REG_VSHUNT   0x04
#define INA228_REG_VBUS     0x05
#define INA228_REG_DIETEMP  0x06
#define INA228_REG_MFR_ID   0x3E
#define INA228_REG_DEV_ID   0x3F
#define INA228_MFR_TI       0x5449

static int bb_scl_idle, bb_sda_idle;
static uint8_t pm_addr;  // 0 = not found

// Scan diagnostics: first address that answered and the ID it returned.
static uint8_t pm_ack_addr;
static uint16_t pm_ack_mfr;

static void bb_delay(void)
{
    for (volatile int i = 0; i < 200; i++);  // ~5 us half period at 168 MHz
}

static int bb_read(int pin)
{
    return (GPIOF->IDR >> pin) & 1;
}

// Open drain: 1 = release (pulled up by the Click), 0 = drive low.
static void bb_pin(int pin, int level)
{
    GPIOF->BSRR = level ? (1UL << pin) : (1UL << (pin + 16));
    bb_delay();
    if (level && pin == BB_SCL) {
        // Allow clock stretching, with a limit.
        for (int t = 0; t < 1000 && !bb_read(BB_SCL); t++)
            bb_delay();
    }
}

static void bb_start(void) { bb_pin(BB_SDA, 1); bb_pin(BB_SCL, 1); bb_pin(BB_SDA, 0); bb_pin(BB_SCL, 0); }
static void bb_stop(void)  { bb_pin(BB_SDA, 0); bb_pin(BB_SCL, 1); bb_pin(BB_SDA, 1); }

static bool bb_write_byte(uint8_t v)  // true on ACK
{
    for (int i = 7; i >= 0; i--) {
        bb_pin(BB_SDA, (v >> i) & 1);
        bb_pin(BB_SCL, 1);
        bb_pin(BB_SCL, 0);
    }
    bb_pin(BB_SDA, 1);
    bb_pin(BB_SCL, 1);
    bool ack = !bb_read(BB_SDA);
    bb_pin(BB_SCL, 0);
    return ack;
}

static uint8_t bb_read_byte(bool ack)
{
    uint8_t v = 0;

    bb_pin(BB_SDA, 1);
    for (int i = 0; i < 8; i++) {
        bb_pin(BB_SCL, 1);
        v = (uint8_t)((v << 1) | bb_read(BB_SDA));
        bb_pin(BB_SCL, 0);
    }
    bb_pin(BB_SDA, ack ? 0 : 1);
    bb_pin(BB_SCL, 1);
    bb_pin(BB_SCL, 0);
    bb_pin(BB_SDA, 1);
    return v;
}

static bool pm_read_at(uint8_t addr, uint8_t reg, uint8_t *buf, size_t len)
{
    bb_start();
    if (!bb_write_byte((uint8_t)(addr << 1)) || !bb_write_byte(reg)) {
        bb_stop();
        return false;
    }
    bb_start();  // repeated start
    if (!bb_write_byte((uint8_t)((addr << 1) | 1))) {
        bb_stop();
        return false;
    }
    for (size_t i = 0; i < len; i++)
        buf[i] = bb_read_byte(i + 1 < len);
    bb_stop();
    return true;
}

static bool pm_read(uint8_t reg, uint8_t *buf, size_t len)
{
    return pm_read_at(pm_addr, reg, buf, len);
}

static bool pm_probe(uint8_t addr)
{
    uint8_t buf[2];

    if (!pm_read_at(addr, INA228_REG_MFR_ID, buf, 2))
        return false;
    if (pm_ack_addr == 0) {
        pm_ack_addr = addr;
        pm_ack_mfr = (uint16_t)((buf[0] << 8) | buf[1]);
    }
    if (((buf[0] << 8) | buf[1]) != INA228_MFR_TI)
        return false;
    if (!pm_read_at(addr, INA228_REG_DEV_ID, buf, 2))
        return false;
    return ((buf[0] << 4) | (buf[1] >> 4)) == 0x228;
}

static void powermonitor_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOFEN;

    // Inputs, no pull: read the idle levels set by the Click's pull-ups.
    GPIOF->MODER &= ~((3UL << (2 * BB_SCL)) | (3UL << (2 * BB_SDA)));
    GPIOF->PUPDR &= ~((3UL << (2 * BB_SCL)) | (3UL << (2 * BB_SDA)));
    bb_delay();
    bb_scl_idle = bb_read(BB_SCL);
    bb_sda_idle = bb_read(BB_SDA);

    // Open-drain outputs, released high.
    GPIOF->BSRR = (1UL << BB_SCL) | (1UL << BB_SDA);
    GPIOF->OTYPER |= (1UL << BB_SCL) | (1UL << BB_SDA);
    GPIOF->MODER |= (1UL << (2 * BB_SCL)) | (1UL << (2 * BB_SDA));
}

static void powermonitor_poll(void)
{
    uint8_t b[3];

    if (pm_addr == 0) {
        pm_ack_addr = 0;
        for (uint8_t a = 0x40; a <= 0x4F && pm_addr == 0; a++)
            if (pm_probe(a))
                pm_addr = a;
        if (pm_addr == 0) {
            if (pm_ack_addr)
                lv_snprintf(powermonitor_status, STATUS_LEN,
                            "S4 Power Monitor: 0x%02X answered but ID 0x%04X is not an INA228",
                            pm_ack_addr, pm_ack_mfr);
            else
                lv_snprintf(powermonitor_status, STATUS_LEN,
                            "S4 Power Monitor: NOT FOUND (no reply at 0x40-0x4F,\n"
                            "   idle SCL %d SDA %d)", bb_scl_idle, bb_sda_idle);
            return;
        }
    }

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
