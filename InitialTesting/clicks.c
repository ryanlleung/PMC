#include <stdbool.h>
#include <string.h>

#include "mcu.h"
#include "board.h"
#include "drv_digital_in.h"
#include "drv_digital_out.h"
#include "lvgl.h"  // lv_snprintf
#include "clicks.h"
#include "cal.h"
#include "fixed.h"
#include "drv_i2c_master.h"
#include "stepper3.h"
#include "link.h"
#include "pmc_config.h"

#define STATUS_LEN 192

static char boost10_status[STATUS_LEN];
static char powermonitor_status[STATUS_LEN];
static char druck_status[STATUS_LEN];
static clicks_state_t state;

/* --------------------------------------------------------------------------
 * Boost 10 (S3)
 *
 * LT8337 boost; output set by a TPL0501 100k digipot in the lower feedback
 * leg (MikroE boost10 driver):
 *   Vout = 1 + 620k / (56k + wiper * 100k / 256)
 * The digipot is write-only (no MISO) and powers up at mid-scale (~6.9 V).
 * There is no enable pin, so the output is live whenever the board is.
 *
 * At start-up the wiper is written to BOOST10_WIPER_SET (~10.0 V nominal,
 * ~9.7-10.4 V with the digipot's +/-20% tolerance), the Druck excitation.
 * It stays there: the screen has no setpoint control. Only PM DIAG moves it
 * (clicks_boost10_set_mv(), 9.0-11.0 V nominal, wiper clamped to
 * BOOST10_WIPER_11V..BOOST10_WIPER_9V) and puts it back afterwards. If the Power Monitor reads VBUS above BOOST10_TRIP_MV
 * the wiper goes to 0xFF (~5 V) and stays there until reset. That check
 * only works with VBUS wired to the Boost 10 output.
 *
 * SPI is bit-banged on the SPI1 pins (PA5 SCK, PB5 MOSI), mode 0, MSB first;
 * the TPL0501 latches the byte on CS rising.
 * ------------------------------------------------------------------------ */
#define BOOST10_WIPER_SET  33    // 1 + 620 / (56 + 12.9) = 10.0 V nominal
#define BOOST10_WIPER_11V  15    // 1 + 620 / (56 + 5.9)  = 11.0 V nominal
#define BOOST10_WIPER_9V   55    // 1 + 620 / (56 + 21.5) = 9.0 V nominal
#define BOOST10_WIPER_MIN_V 0xFF // 1 + 620 / (56 + 99.6) = 5.0 V
#define BOOST10_TRIP_MV    11500

#define B10_SCK  5  // PA5
#define B10_MOSI 5  // PB5

static digital_out_t boost10_cs;
static digital_in_t boost10_pg;
static uint8_t boost10_wiper;
static bool boost10_tripped;
static int32_t boost10_trip_mv;

static void b10_delay(void)
{
    for (volatile int i = 0; i < 50; i++);
}

static void boost10_write_wiper(uint8_t w)
{
    digital_out_low(&boost10_cs);
    b10_delay();
    for (int i = 7; i >= 0; i--) {
        GPIOB->BSRR = ((w >> i) & 1) ? (1UL << B10_MOSI) : (1UL << (B10_MOSI + 16));
        b10_delay();
        GPIOA->BSRR = 1UL << B10_SCK;          // sample on rising edge
        b10_delay();
        GPIOA->BSRR = 1UL << (B10_SCK + 16);
    }
    b10_delay();
    digital_out_high(&boost10_cs);             // latch
    boost10_wiper = w;
}

static void boost10_init(void)
{
    digital_out_init(&boost10_cs, MIKROBUS_3_CS);
    digital_out_high(&boost10_cs);

    // PA5 SCK and PB5 MOSI as push-pull outputs, idle low.
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOBEN;
    GPIOA->BSRR = 1UL << (B10_SCK + 16);
    GPIOB->BSRR = 1UL << (B10_MOSI + 16);
    GPIOA->OTYPER &= ~(1UL << B10_SCK);
    GPIOB->OTYPER &= ~(1UL << B10_MOSI);
    GPIOA->MODER = (GPIOA->MODER & ~(3UL << (2 * B10_SCK))) | (1UL << (2 * B10_SCK));
    GPIOB->MODER = (GPIOB->MODER & ~(3UL << (2 * B10_MOSI))) | (1UL << (2 * B10_MOSI));

    digital_in_init(&boost10_pg, MIKROBUS_3_INT);
    // PG2 pull-up (PUPDR = 01).
    GPIOG->PUPDR = (GPIOG->PUPDR & ~(3UL << (2 * 2))) | (1UL << (2 * 2));

    boost10_write_wiper(BOOST10_WIPER_SET);
}

// Nominal output in mV for a wiper value: 1000 + 620000 / (56 + w * 100 / 256).
static int32_t boost10_wiper_to_mv(uint8_t w)
{
    return 1000 + 158720000L / (14336L + 100L * w);
}

int32_t clicks_boost10_set_mv(int32_t mv)
{
    if (boost10_tripped)
        return -1;
    if (mv < 9000) mv = 9000;
    if (mv > 11000) mv = 11000;

    // Inverse of boost10_wiper_to_mv, rounded.
    int32_t w = (158720000L / (mv - 1000) - 14336L + 50) / 100;
    if (w < BOOST10_WIPER_11V) w = BOOST10_WIPER_11V;
    if (w > BOOST10_WIPER_9V) w = BOOST10_WIPER_9V;

    if ((uint8_t)w != boost10_wiper)
        boost10_write_wiper((uint8_t)w);
    return boost10_wiper_to_mv(boost10_wiper);
}

// Called with the latest VBUS reading, or -1 if the Power Monitor is absent.
static void boost10_check(int32_t vbus_mv)
{
    if (!boost10_tripped && vbus_mv > BOOST10_TRIP_MV) {
        boost10_write_wiper(BOOST10_WIPER_MIN_V);
        boost10_tripped = true;
        boost10_trip_mv = vbus_mv;
    }
}

static void boost10_poll(void)
{
    // LT8337 PG is open drain, pulled low when the output is out of
    // regulation (100k pull-up on the Click, schematic v101). PM DIAG saw PG
    // high with VBUS on the setpoint, so high = regulating.
    state.boost_pg = digital_in_read(&boost10_pg) != 0;
    state.boost_tripped = boost10_tripped;
    state.boost_trip_mv = boost10_trip_mv;
    state.boost_set_mv = boost10_wiper_to_mv(boost10_wiper);

    const char *pg = state.boost_pg ? "PG high = regulating" : "PG low = NOT regulating";
    if (boost10_tripped)
        lv_snprintf(boost10_status, STATUS_LEN,
                    "S3 Boost 10: TRIPPED at %ld mV, now ~5 V, %s",
                    (long)boost10_trip_mv, pg);
    else
    {
        char v[16];
        fixed_fmt(v, sizeof v, fixed_round(boost10_wiper_to_mv(boost10_wiper), 1), 2);
        lv_snprintf(boost10_status, STATUS_LEN,
                    "S3 Boost 10: wiper %u (%s V nom), %s", boost10_wiper, v, pg);
    }
}

/* --------------------------------------------------------------------------
 * Power Monitor (S4)
 *
 * INA228 on the shield I2C lines (PF1 SCL, PF0 SDA), I2C2 through the
 * mikroSDK I2C master driver at 100 kHz.
 *
 * The driver (mikroSDK 2.19.3, STM32F4 hal_ll) returns from any failed
 * transfer without sending STOP or clearing the NACK flag, so I2C2 stays
 * BUSY and every later transfer fails too. After a failure pm_recover()
 * sends STOP, closes the driver, resets I2C2 and opens it again. This is
 * what made it look dead before: a scan from 0x40 left I2C2 stuck at the
 * first empty address, so the INA228 at 0x4A was never reached.
 * The timeout is the driver default, 10000 status reads (a few ms); a short
 * one (100) times out before the 90 us address byte is even sent.
 *
 * Real detection: manufacturer ID 0x5449 ("TI") and device ID 0x228x. The
 * address depends on the A0/A1 jumpers (0x40-0x4F): 0x4A, the one fitted,
 * is tried first, then the rest.
 * ------------------------------------------------------------------------ */
#define PM_SCL_PIN 1  // PF1, read directly to spot a bus held low
#define PM_SDA_PIN 0  // PF0
#define PM_ADDR_FITTED 0x4A

#define INA228_REG_ADC_CONFIG 0x01
#define INA228_REG_VSHUNT   0x04
#define INA228_REG_VBUS     0x05
#define INA228_REG_DIETEMP  0x06
#define INA228_REG_MFR_ID   0x3E
#define INA228_REG_DEV_ID   0x3F
#define INA228_MFR_TI       0x5449

static i2c_master_t pm_i2c;
static bool pm_open;
static uint8_t pm_addr;  // 0 = not found

// Scan diagnostics: first address that answered and the ID it returned.
static uint8_t pm_ack_addr;
static uint16_t pm_ack_mfr;

static void pm_spin(uint32_t n)
{
    for (volatile uint32_t i = 0; i < n; i++);
}

static void pm_i2c_open(void)
{
    i2c_master_config_t cfg;

    i2c_master_configure_default(&cfg);   // timeout 10000
    cfg.scl = MIKROBUS_4_SCL;
    cfg.sda = MIKROBUS_4_SDA;
    cfg.speed = I2C_MASTER_SPEED_STANDARD;
    // Returns the acquire code: 1 on the first open, -1 on failure.
    pm_open = i2c_master_open(&pm_i2c, &cfg) != I2C_MASTER_ERROR;
}

// After a failed transfer: let a byte in flight finish (90 us at 100 kHz),
// STOP, then a fresh I2C2 so the next transfer does not find it BUSY.
static void pm_recover(void)
{
    pm_spin(20000);
    I2C2->CR1 |= I2C_CR1_STOP;
    pm_spin(20000);
    if (pm_open)
        i2c_master_close(&pm_i2c);
    RCC->APB1RSTR |= RCC_APB1RSTR_I2C2RST;
    RCC->APB1RSTR &= ~RCC_APB1RSTR_I2C2RST;
    pm_i2c_open();
}

static bool pm_read_at(uint8_t addr, uint8_t reg, uint8_t *buf, size_t len)
{
    if (!pm_open)
        pm_i2c_open();   // retried at each scan if it failed before
    if (!pm_open)
        return false;
    i2c_master_set_slave_address(&pm_i2c, addr);
    if (i2c_master_write_then_read(&pm_i2c, &reg, 1, buf, len) == I2C_MASTER_SUCCESS)
        return true;
    pm_recover();
    return false;
}

static bool pm_write16(uint8_t reg, uint16_t v)
{
    uint8_t b[3] = { reg, (uint8_t)(v >> 8), (uint8_t)v };

    if (!pm_open)
        return false;
    i2c_master_set_slave_address(&pm_i2c, pm_addr);
    if (i2c_master_write(&pm_i2c, b, sizeof b) == I2C_MASTER_SUCCESS)
        return true;
    pm_recover();
    return false;
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
    // PF1/PF0 are read directly to spot a bus held low, so the port clock
    // must be on even if the driver open fails.
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOFEN;
    pm_i2c_open();
}

/* --------------------------------------------------------------------------
 * Druck UNIK 5000 PDCR50G1, 15 psia absolute, mV linearised, 10 mV/V
 *
 * Wiring: Boost 10 VOUT -> Druck pin 1 (+Supply) and Power Monitor VBUS;
 * pin 2 (+Out) -> IN+, pin 4 (-Out) -> IN-; pin 3 (-Supply), Boost 10 GND
 * and Power Monitor GND common.
 *
 * Excitation compensation: the output is ratiometric to the supply, so the
 * reading is the ratio r = Vshunt / Vbus, which cancels Boost 10 drift and
 * the INA228's shared reference. Both channels use the same 128 averages.
 *
 * Calibration: r is turned into pressure by cal.c, a table of r against
 * pressure (a cert zero/span is the two-point case) stored in the on-board
 * serial flash and loaded at start-up. With nothing stored the nominal
 * 0 mV and 100 mV at 10 V are used, and the screen says "nominal cal".
 *
 * Integer maths: r in ppb (1 mV/V = 1 000 000 ppb).
 *   r = raw_shunt * 312.5 nV / (raw_bus * 195.3125 uV) = raw_shunt / raw_bus * 1.6e6 ppb
 * ------------------------------------------------------------------------ */
#define DRUCK_VEXC_MIN_MV    7000      // datasheet supply range 7-12 V

/* A connected Druck gives about 0.13 mV/V at vacuum (cert zero 1.337 mV at
 * 10 V, less the ~4 % input loading) up to about 10.2 mV/V at 15 psia. Well
 * outside that, the sensor is unplugged or faulty. The limits leave room
 * for other sensors' zero spread and some overrange. Not tested with the
 * Druck unplugged yet: if the open inputs happen to float inside this
 * window, the check will not catch it. */
#define DRUCK_R_MIN_PPB      50000     // 0.05 mV/V
#define DRUCK_R_MAX_PPB      11000000  // 11 mV/V

// The ratio-to-pressure table lives in cal.c (stored in serial flash).
static char druck_value[24];

// Pressure in 0.001 mbar from the INA228 raw readings.
static int64_t druck_pressure_mmbar(int32_t raw_shunt, int32_t raw_bus, int64_t *r_ppb)
{
    *r_ppb = (int64_t)raw_shunt * 1600000 / raw_bus;
    return cal_pressure_mmbar(*r_ppb);
}

static void druck_update(int32_t raw_shunt, int32_t raw_bus, int32_t bus_mv)
{
    state.reading_ok = false;
    state.druck_absent = false;
    if (bus_mv < DRUCK_VEXC_MIN_MV) {
        lv_snprintf(druck_value, sizeof druck_value, "----");
        lv_snprintf(druck_status, STATUS_LEN,
                    "Druck: excitation %ld mV, below 7 V (VBUS not wired?)", (long)bus_mv);
        return;
    }

    int64_t r_ppb;
    int64_t p = druck_pressure_mmbar(raw_shunt, raw_bus, &r_ppb);
    state.r_ppb = (int32_t)r_ppb;
    if (r_ppb < DRUCK_R_MIN_PPB || r_ppb > DRUCK_R_MAX_PPB) {
        char r[16];
        state.druck_absent = true;
        lv_snprintf(druck_value, sizeof druck_value, "----");
        lv_snprintf(druck_status, STATUS_LEN,
                    "Druck: signal %s mV/V, outside 0.05-11 mV/V (not connected?)",
                    fixed_fmt(r, sizeof r, r_ppb / 1000, 3));
        return;
    }
    state.reading_ok = true;
    state.p_mmbar = (int32_t)p;

    // Pressure rounded to 0.01 mbar; r in mV/V cut to 5 decimals.
    char r[16];
    fixed_fmt(druck_value, sizeof druck_value, fixed_round(p, 1), 2);
    lv_snprintf(druck_status, STATUS_LEN,
                "Druck: %s mbar abs  r %s mV/V  exc %ld mV  %s",
                druck_value, fixed_fmt(r, sizeof r, r_ppb / 10, 5),
                (long)bus_mv, cal_is_default() ? "nominal cal" : cal_id());
}

#if PMC_PM_DIAG
static int pmdiag_step = -1;  // -1 = not running
static void pmdiag_run_step(void);
#endif

static void powermonitor_poll(void)
{
    uint8_t b[3];

    if (pm_addr == 0) {
        pm_ack_addr = 0;
        // With SCL held low (wiring fault) every probe would time out and
        // recover, 16 times a second, starving the UI and network.
        if (!((GPIOF->IDR >> PM_SCL_PIN) & 1)) {
            lv_snprintf(powermonitor_status, STATUS_LEN,
                        "S4 Power Monitor: SCL held low (I2C wiring fault?)");
            lv_snprintf(druck_value, sizeof druck_value, "----");
            lv_snprintf(druck_status, STATUS_LEN, "Druck: no reading (Power Monitor not found)");
            state.pm_found = false;
            state.reading_ok = false;
            return;
        }
        if (pm_probe(PM_ADDR_FITTED))
            pm_addr = PM_ADDR_FITTED;
        for (uint8_t a = 0x40; a <= 0x4F && pm_addr == 0; a++)
            if (a != PM_ADDR_FITTED && pm_probe(a))
                pm_addr = a;
        // Continuous shunt, bus and temperature, 1052 us each, 128 averages
        // (~0.4 s per result). Default is 0xFB68, no averaging.
        if (pm_addr != 0 && !pm_write16(INA228_REG_ADC_CONFIG, 0xFB6C))
            pm_addr = 0;
        if (pm_addr == 0) {
            if (pm_ack_addr)
                lv_snprintf(powermonitor_status, STATUS_LEN,
                            "S4 Power Monitor: 0x%02X answered but ID 0x%04X is not an INA228",
                            pm_ack_addr, pm_ack_mfr);
            else if (!pm_open)
                lv_snprintf(powermonitor_status, STATUS_LEN,
                            "S4 Power Monitor: I2C2 open failed");
            else
                lv_snprintf(powermonitor_status, STATUS_LEN,
                            "S4 Power Monitor: NOT FOUND (no reply at 0x40-0x4F,\n"
                            "   SCL %d SDA %d)", (int)((GPIOF->IDR >> PM_SCL_PIN) & 1),
                            (int)((GPIOF->IDR >> PM_SDA_PIN) & 1));
            lv_snprintf(druck_value, sizeof druck_value, "----");
            lv_snprintf(druck_status, STATUS_LEN, "Druck: no reading (Power Monitor not found)");
            state.pm_found = false;
            state.reading_ok = false;
            return;
        }
    }

#if PMC_PM_DIAG
    // The diagnostic owns the INA228 while it runs; readings hold their last value.
    if (pmdiag_step >= 0) {
        pmdiag_run_step();
        return;
    }
#endif

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

    state.pm_found = true;
    state.shunt_nv = (int32_t)(((int64_t)raw_shunt * 3125) / 10);
    state.bus_mv = bus_mv;
    state.die_mc = (raw_temp * 125) / 16;   // 7.8125 m degC per LSB
    boost10_check(bus_mv);
    druck_update(raw_shunt, raw_bus, bus_mv);

    char sh[16], tc[16];
    lv_snprintf(powermonitor_status, STATUS_LEN,
                "S4 Power Monitor: INA228 found @0x%02X\n"
                "   Vshunt %s uV   Vbus %ld mV   die %s C",
                pm_addr, fixed_fmt(sh, sizeof sh, shunt_0u1, 1), (long)bus_mv,
                fixed_fmt(tc, sizeof tc, temp_d1, 1));
    return;

lost:
    pm_addr = 0;
    lv_snprintf(powermonitor_status, STATUS_LEN,
                "S4 Power Monitor: lost (I2C read failed)");
    lv_snprintf(druck_value, sizeof druck_value, "----");
    lv_snprintf(druck_status, STATUS_LEN, "Druck: no reading (Power Monitor lost)");
    state.pm_found = false;
    state.reading_ok = false;
}

#if PMC_PM_DIAG
/* --------------------------------------------------------------------------
 * PM DIAG: Power Monitor input diagnostic (COM3 / TCP command)
 *
 * The board reads the Druck ~4.4 % below a DMM at the Click IN terminals.
 * The Click has nothing between IN1 and the INA228 pins (schematic v100),
 * so this checks the INA228 itself and how it samples the Druck output:
 *   step 0      register readback (ADCRANGE, MEMSTAT trim checksum, ...)
 *   steps 1-8   single-shot shunt conversions at each conversion time,
 *               32 each: mean, min, max, sd. A mean that moves with the
 *               conversion time, or a large sd, means interference is
 *               being aliased; a mean that stays put means it is not.
 *   steps 9-11  on-chip averaged readings: shunt only, and the normal
 *               bus+shunt+temp sequence (does the bus conversion upset
 *               the shunt one).
 *   steps 12-14 excitation 9, 10, 11 V: the ratio should not move.
 * One step per second from clicks_poll(), one line per step; then the
 * excitation and the normal continuous setting are put back.
 * ------------------------------------------------------------------------ */
#define INA228_REG_CONFIG       0x00
#define INA228_REG_SHUNT_CAL    0x02
#define INA228_REG_SHUNT_TEMPCO 0x03
#define INA228_REG_DIAG_ALRT    0x0B
#define INA228_ADC_CONFIG_RUN   0xFB6C

#define PM_CFG(mode, vbusct, vshct, vtct, avg) \
    (uint16_t)(((mode) << 12) | ((vbusct) << 9) | ((vshct) << 6) | ((vtct) << 3) | (avg))

typedef struct {
    const char *what;
    uint16_t cfg;      // ADC_CONFIG for one triggered conversion
    uint8_t n;         // conversions taken
    int16_t exc_mv;    // excitation to set first, 0 = leave
} pmdiag_step_t;

static const pmdiag_step_t pmdiag_steps[] = {
    { "registers", 0, 0, 0 },
    { "shunt 50us x1",     PM_CFG(2, 5, 0, 5, 0), 32, 0 },
    { "shunt 84us x1",     PM_CFG(2, 5, 1, 5, 0), 32, 0 },
    { "shunt 150us x1",    PM_CFG(2, 5, 2, 5, 0), 32, 0 },
    { "shunt 280us x1",    PM_CFG(2, 5, 3, 5, 0), 32, 0 },
    { "shunt 540us x1",    PM_CFG(2, 5, 4, 5, 0), 32, 0 },
    { "shunt 1052us x1",   PM_CFG(2, 5, 5, 5, 0), 32, 0 },
    { "shunt 2074us x1",   PM_CFG(2, 5, 6, 5, 0), 32, 0 },
    { "shunt 4120us x1",   PM_CFG(2, 5, 7, 5, 0), 32, 0 },
    { "shunt only 1052us x128",      PM_CFG(2, 5, 5, 5, 4), 2, 0 },
    { "shunt only 4120us x64",       PM_CFG(2, 5, 7, 5, 3), 1, 0 },
    { "bus+shunt+temp 1052us x128",  PM_CFG(7, 5, 5, 5, 4), 1, 0 },
    { "exc 9V shunt+bus 1052us x128",  PM_CFG(3, 5, 5, 5, 4), 1, 9000 },
    { "exc 10V shunt+bus 1052us x128", PM_CFG(3, 5, 5, 5, 4), 1, 10000 },
    { "exc 11V shunt+bus 1052us x128", PM_CFG(3, 5, 5, 5, 4), 1, 11000 },
};
#define PMDIAG_STEPS ((int)(sizeof pmdiag_steps / sizeof pmdiag_steps[0]))

static uint8_t pmdiag_saved_wiper;

static void pmdiag_kick(void)
{
    IWDG->KR = 0xAAAA;  // a step can block for up to ~1 s
}

static bool pm_read16(uint8_t reg, uint16_t *v)
{
    uint8_t b[2];
    if (!pm_read(reg, b, 2))
        return false;
    *v = (uint16_t)((b[0] << 8) | b[1]);
    return true;
}

// One triggered conversion set (cfg), then VSHUNT and, if the mode has the
// bus in it, VBUS. Raw 20-bit values.
static bool pm_triggered(uint16_t cfg, int32_t *shunt, int32_t *bus)
{
    uint8_t b[3];
    uint16_t d;

    if (!pm_write16(INA228_REG_ADC_CONFIG, cfg))  // starts it, clears CNVRF
        return false;
    uint32_t t0 = lv_tick_get();
    for (;;) {
        if (!pm_read16(INA228_REG_DIAG_ALRT, &d))
            return false;
        if (d & 0x0002)  // CNVRF
            break;
        if (lv_tick_elaps(t0) > 1000)
            return false;
        pmdiag_kick();
    }
    if (!pm_read(INA228_REG_VSHUNT, b, 3))
        return false;
    *shunt = (int32_t)(((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8)) >> 12;
    *bus = 0;
    if ((cfg >> 12) & 1) {
        if (!pm_read(INA228_REG_VBUS, b, 3))
            return false;
        *bus = (int32_t)((((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | b[2]) >> 4);
    }
    return true;
}

static void pmdiag_registers(void)
{
    uint16_t cfg = 0, adc = 0, cal = 0, tc = 0, dg = 0, id = 0;
    bool ok = pm_read16(INA228_REG_CONFIG, &cfg) && pm_read16(INA228_REG_ADC_CONFIG, &adc) &&
              pm_read16(INA228_REG_SHUNT_CAL, &cal) && pm_read16(INA228_REG_SHUNT_TEMPCO, &tc) &&
              pm_read16(INA228_REG_DIAG_ALRT, &dg) && pm_read16(INA228_REG_DEV_ID, &id);
    if (!ok) {
        link_printf("PMDIAG 0 registers: I2C read failed\r\n");
        return;
    }
    link_printf("PMDIAG 0 registers: CONFIG 0x%04X (ADCRANGE %d TEMPCOMP %d) ADC_CONFIG 0x%04X "
                "SHUNT_CAL 0x%04X TEMPCO 0x%04X DIAG_ALRT 0x%04X (MEMSTAT %d, 1 = trim OK) DEV_ID 0x%04X\r\n",
                cfg, (cfg >> 4) & 1, (cfg >> 5) & 1, adc, cal, tc, dg, dg & 1, id);
}

static void pmdiag_measure(int k)
{
    const pmdiag_step_t *st = &pmdiag_steps[k];
    char a[16], b[16], c[16], d[16];
    int32_t sh, bus = 0, first = 0, mn = 0, mx = 0;
    int64_t sum = 0, sumsq = 0, sumbus = 0;

    if (st->exc_mv) {
        if (clicks_boost10_set_mv(st->exc_mv) < 0) {
            link_printf("PMDIAG %d %s: skipped, Boost 10 tripped\r\n", k, st->what);
            return;
        }
        uint32_t t0 = lv_tick_get();   // let the Boost and the Druck settle
        while (lv_tick_elaps(t0) < 300)
            pmdiag_kick();
    }

    for (int i = 0; i < st->n; i++) {
        if (!pm_triggered(st->cfg, &sh, &bus)) {
            link_printf("PMDIAG %d %s: conversion %d failed (I2C or no CNVRF)\r\n", k, st->what, i);
            return;
        }
        if (i == 0) { first = sh; mn = sh; mx = sh; }
        if (sh < mn) mn = sh;
        if (sh > mx) mx = sh;
        sum += sh;
        sumsq += (int64_t)(sh - first) * (sh - first);
        sumbus += bus;
    }

    int n = st->n;
    int64_t dsum = sum - (int64_t)first * n;
    int64_t var_raw_n2 = sumsq * n - dsum * dsum;           // n^2 x variance, raw^2
    if (var_raw_n2 < 0) var_raw_n2 = 0;
    // sd in 0.1 uV: sqrt(var) x 3.125
    int32_t sd = (int32_t)(isqrt64((uint64_t)var_raw_n2 * 625 / 64) / (uint32_t)n);  // x 9.765625
    int32_t mean = (int32_t)(sum * 3125 / (1000LL * n));    // 0.1 uV

    if ((st->cfg >> 12) & 1) {  // mode includes the bus
        int32_t raw_bus = (int32_t)(sumbus / n);
        int32_t bus_mv = (int32_t)(((int64_t)raw_bus * 1953125) / 10000000);
        int64_t r_ppb = raw_bus ? sum * 1600000 / ((int64_t)raw_bus * n) : 0;
        boost10_check(bus_mv);
        link_printf("PMDIAG %d %s: signal %s uV  exc %ld mV  ratio %s mV/V\r\n",
                    k, st->what, fixed_fmt(a, sizeof a, mean, 1), (long)bus_mv,
                    fixed_fmt(b, sizeof b, r_ppb, 6));
    } else if (n < 4) {  // on-chip averaged: the mean is the result
        link_printf("PMDIAG %d %s: signal %s uV (n=%d)\r\n", k, st->what, fixed_fmt(a, sizeof a, mean, 1), n);
    } else {
        link_printf("PMDIAG %d %s: n=%d mean %s min %s max %s sd %s uV\r\n", k, st->what, n,
                    fixed_fmt(a, sizeof a, mean, 1),
                    fixed_fmt(b, sizeof b, (int32_t)(mn * 3125 / 1000), 1),
                    fixed_fmt(c, sizeof c, (int32_t)(mx * 3125 / 1000), 1),
                    fixed_fmt(d, sizeof d, sd, 1));
    }
}

static void pmdiag_run_step(void)
{
    int k = pmdiag_step;

    lv_snprintf(powermonitor_status, STATUS_LEN,
                "S4 Power Monitor: PM DIAG step %d of %d (see COM3)", k, PMDIAG_STEPS - 1);
    if (k == 0)
        pmdiag_registers();
    else
        pmdiag_measure(k);

    if (++pmdiag_step < PMDIAG_STEPS)
        return;

    // Put things back.
    pmdiag_step = -1;
    if (!boost10_tripped)
        boost10_write_wiper(pmdiag_saved_wiper);
    if (!pm_write16(INA228_REG_ADC_CONFIG, INA228_ADC_CONFIG_RUN))
        pm_addr = 0;  // rescan next second
    link_printf("PMDIAG done, excitation back to %ld mV nominal, normal readings resume\r\n",
                (long)boost10_wiper_to_mv(boost10_wiper));
}

bool clicks_command(const char *line)
{
    if (strcmp(line, "PM DIAG") != 0)
        return false;
    if (pmdiag_step >= 0) {
        link_printf("ERR PM DIAG already running\r\n");
    } else if (pm_addr == 0) {
        link_printf("ERR Power Monitor not found\r\n");
    } else {
        pmdiag_saved_wiper = boost10_wiper;
        pmdiag_step = 0;
        link_printf("OK PM DIAG: %d steps, one a second, excitation moves 9-11 V and is put back\r\n",
                    PMDIAG_STEPS);
    }
    return true;
}
#else
bool clicks_command(const char *line) { (void)line; return false; }
#endif

/* --------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------ */
void clicks_init(void)
{
    stepper3_init();
    powermonitor_init();
    boost10_init();
    clicks_poll();
}

void clicks_poll(void)
{
    powermonitor_poll();  // may trip the Boost 10, so first
    boost10_poll();
}

const char *clicks_stepper3_status(void)     { return stepper3_status(); }
const char *clicks_boost10_status(void)      { return boost10_status; }
const char *clicks_powermonitor_status(void) { return powermonitor_status; }
const char *clicks_druck_status(void)        { return druck_status; }
const char *clicks_druck_value(void)         { return druck_value; }
bool clicks_druck_cal_nominal(void)          { return cal_is_default(); }
const clicks_state_t *clicks_state(void)
{
    state.cal_nominal = cal_is_default();
#if PMC_PM_DIAG
    state.pm_diag = pmdiag_step >= 0;
#endif
    return &state;
}
