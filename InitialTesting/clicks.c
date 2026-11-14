#include <string.h>
#include <stdbool.h>

#include "mcu.h"
#include "board.h"
#include "drv_digital_in.h"
#include "drv_digital_out.h"
#include "clicks.h"
#include "cal.h"
#include "drv_i2c_master.h"
#include "stepper3.h"
#include "pmc_config.h"
#include "baro.h"

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
 * It stays there: nothing moves it. If the Power Monitor reads VBUS above BOOST10_TRIP_MV
 * the wiper goes to 0xFF (~5 V) and stays there until reset. That check
 * only works with VBUS wired to the Boost 10 output.
 *
 * SPI is bit-banged on the SPI1 pins (PA5 SCK, PB5 MOSI), mode 0, MSB first;
 * the TPL0501 latches the byte on CS rising.
 * ------------------------------------------------------------------------ */
#define BOOST10_WIPER_SET  33    // 1 + 620 / (56 + 12.9) = 10.0 V nominal
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
    // regulation (100k pull-up on the Click, schematic v101). The bench saw PG
    // high with VBUS on the setpoint, so high = regulating.
    state.boost_pg = digital_in_read(&boost10_pg) != 0;
    state.boost_tripped = boost10_tripped;
    state.boost_trip_mv = boost10_trip_mv;
    state.boost_set_mv = boost10_wiper_to_mv(boost10_wiper);
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
 * sends STOP, closes the driver, resets I2C2 and opens it again.
 * The timeout is the driver default, 10000 status reads (a few ms); a short
 * one (100) times out before the 90 us address byte is even sent.
 *
 * Real detection: manufacturer ID 0x5449 ("TI") and device ID 0x228x, at
 * 0x4A, the address the A0/A1 jumpers are fitted for. No other address is
 * tried (0.16.28): each one that does not answer costs a timeout and a bus
 * reset in the main loop.
 * ------------------------------------------------------------------------ */
#define PM_SCL_PIN 1  // PF1, read directly to spot a bus held low
#define PM_ADDR_FITTED 0x4A

#define INA228_REG_ADC_CONFIG 0x01
#define INA228_REG_VSHUNT   0x04
#define INA228_REG_VBUS     0x05
#define INA228_REG_DIETEMP  0x06
#define INA228_REG_MFR_ID   0x3E
#define INA228_REG_DEV_ID   0x3F

// ADC_CONFIG fields (INA228 datasheet SLYS021A): MODE 15-12, VBUSCT 11-9, VSHCT 8-6,
// VTCT 5-3, AVG 2-0. CT codes 4 = 540 us, 5 = 1052 us, 6 = 2074 us; AVG 4 = 128.
#define PM_CFG(mode, vbusct, vshct, vtct, avg) \
    (uint16_t)(((mode) << 12) | ((vbusct) << 9) | ((vshct) << 6) | ((vtct) << 3) | (avg))
// Continuous shunt, bus and temperature (MODE 0xF), 128 averages.
// Shunt 2074 us, bus 540 us, temperature 2074 us: 600.06 ms, 30.00 cycles
// of 50 Hz mains, so mains pickup averages out.
#define INA228_ADC_CONFIG_RUN PM_CFG(0xF, 4, 6, 6, 4)  // 0xF9B4, 600 ms
#define INA228_MFR_TI       0x5449

static i2c_master_t pm_i2c;
static bool pm_open;
static uint8_t pm_addr;  // 0 = not found; tried every second until it is

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
        pm_i2c_open();   // retried at each probe if it failed before
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

bool clicks_i2c_read(uint8_t addr, uint8_t reg, uint8_t *buf, size_t len)
{
    return pm_read_at(addr, reg, buf, len);
}

bool clicks_i2c_write(uint8_t addr, uint8_t reg, const uint8_t *data, size_t len)
{
    uint8_t b[8];

    if (len + 1 > sizeof b)
        return false;
    if (!pm_open)
        pm_i2c_open();
    if (!pm_open)
        return false;
    b[0] = reg;
    memcpy(b + 1, data, len);
    i2c_master_set_slave_address(&pm_i2c, addr);
    if (i2c_master_write(&pm_i2c, b, len + 1) == I2C_MASTER_SUCCESS)
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

/* A connected Druck gives about 0.01-0.13 mV/V at vacuum (cert 13825224
 * zero 1.337 mV at 10 V; the bench unit 5880156 gives 0.13 mV, 0.013 mV/V)
 * up to about 10.2 mV/V at 15 psia. Unplugged, the open inputs read about
 * 0 mV (bench, 14 Oct 2026), so anything under 0.005 mV/V is taken as no
 * sensor. That leaves 5880156 about 0.9 mbar of zero drift at vacuum
 * (spec +/-0.41 mbar); a sensor whose zero is under 0.005 mV/V would read
 * as unplugged at vacuum. A shorted output also reads 0 mV and is caught
 * the same way. The check is on the raw ratio, not the pressure, so it
 * does not depend on the cal and CAL VAC still works with a wrong cal. */
#define DRUCK_R_MIN_PPB      5000      // 0.005 mV/V
#define DRUCK_R_MAX_PPB      11000000  // 11 mV/V

// The ratio-to-pressure table lives in cal.c (stored in serial flash).
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
    if (bus_mv < DRUCK_VEXC_MIN_MV)
        return;   // excitation below 7 V (VBUS not wired?)

    int64_t r_ppb;
    int64_t p = druck_pressure_mmbar(raw_shunt, raw_bus, &r_ppb);
    state.r_ppb = (int32_t)r_ppb;
    if (r_ppb < DRUCK_R_MIN_PPB || r_ppb > DRUCK_R_MAX_PPB) {
        state.druck_absent = true;   // outside 0.005 to 11 mV/V: not connected?
        return;
    }
    state.reading_ok = true;
    state.p_mmbar = (int32_t)p;
}

static void powermonitor_poll(void)
{
    uint8_t b[3];

    if (pm_addr == 0) {
        // With SCL held low (wiring fault) the probe would time out and
        // recover every second, starving the UI and network: not tried.
        bool scl_low = !((GPIOF->IDR >> PM_SCL_PIN) & 1);
        if (!scl_low && pm_probe(PM_ADDR_FITTED))
            pm_addr = PM_ADDR_FITTED;
        // Continuous readings, 128 averages over 600 ms. Default is
        // 0xFB68, no averaging.
        if (pm_addr != 0 && !pm_write16(INA228_REG_ADC_CONFIG, INA228_ADC_CONFIG_RUN))
            pm_addr = 0;
        if (pm_addr == 0) {
            state.pm_found = false;
            state.reading_ok = false;
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

    int32_t bus_mv = (int32_t)(((int64_t)raw_bus * 1953125) / 10000000);

    state.pm_found = true;
    state.shunt_nv = (int32_t)(((int64_t)raw_shunt * 3125) / 10);
    state.bus_mv = bus_mv;
    state.die_mc = (raw_temp * 125) / 16;   // 7.8125 m degC per LSB
    boost10_check(bus_mv);
    druck_update(raw_shunt, raw_bus, bus_mv);
    return;

lost:
    pm_addr = 0;   // an I2C read failed: probed again next second
    state.pm_found = false;
    state.reading_ok = false;
}

/* --------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------ */
void clicks_init(void)
{
    stepper3_init();
    powermonitor_init();
    boost10_init();
    baro_init();
    clicks_poll();
}

void clicks_poll(void)
{
    powermonitor_poll();  // may trip the Boost 10, so first
    boost10_poll();
    baro_poll();
    state.baro_ok = baro_reading(&state.baro_mmbar, &state.baro_mc);
}

const clicks_state_t *clicks_state(void)
{
    state.cal_nominal = cal_is_default();
    return &state;
}
