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
static char druck_status[STATUS_LEN];

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
 * LT8337 boost; output set by a TPL0501 100k digipot in the lower feedback
 * leg (MikroE boost10 driver):
 *   Vout = 1 + 620k / (56k + wiper * 100k / 256)
 * The digipot is write-only (no MISO) and powers up at mid-scale (~6.9 V).
 * There is no enable pin, so the output is live whenever the board is.
 *
 * At start-up the wiper is written to BOOST10_WIPER_SET (~10.0 V nominal,
 * ~9.7-10.4 V with the digipot's +/-20% tolerance), the Druck excitation.
 * clicks_boost10_set_mv() then lets the screen slider move it between
 * 9.0 and 11.0 V nominal; the wiper is clamped to BOOST10_WIPER_11V..
 * BOOST10_WIPER_9V whatever is asked. If the Power Monitor reads VBUS above BOOST10_TRIP_MV
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
    const char *pg = digital_in_read(&boost10_pg) == 0 ? "PG low = regulating"
                                                        : "PG high = NOT regulating";
    if (boost10_tripped)
        lv_snprintf(boost10_status, STATUS_LEN,
                    "S3 Boost 10: TRIPPED at %ld mV, now ~5 V, %s",
                    (long)boost10_trip_mv, pg);
    else
    {
        int32_t mv = boost10_wiper_to_mv(boost10_wiper);
        lv_snprintf(boost10_status, STATUS_LEN,
                    "S3 Boost 10: wiper %u (%ld.%02ld V nom), %s", boost10_wiper,
                    (long)(mv / 1000), (long)((mv % 1000 + 5) / 10), pg);
    }
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

#define INA228_REG_ADC_CONFIG 0x01
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

static bool pm_write16(uint8_t reg, uint16_t v)
{
    bool ok;

    bb_start();
    ok = bb_write_byte((uint8_t)(pm_addr << 1)) && bb_write_byte(reg) &&
         bb_write_byte((uint8_t)(v >> 8)) && bb_write_byte((uint8_t)v);
    bb_stop();
    return ok;
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
 * Calibration (zero and span) from the sensor's own cal certificate
 * (option CA, zero/span data):
 *   r0    = zero output / cert supply
 *   rspan = span output / cert supply   (output at FS minus zero)
 *   P     = (r - r0) / rspan * 1034.214 mbar   (15 psia)
 * Until the cert values are entered the nominal 0 mV and 100 mV at 10 V are
 * used, and the screen says "nominal cal".
 *
 * Integer maths: r in ppb (1 mV/V = 1 000 000 ppb).
 *   r = raw_shunt * 312.5 nV / (raw_bus * 195.3125 uV) = raw_shunt / raw_bus * 1.6e6 ppb
 * ------------------------------------------------------------------------ */
#define DRUCK_VEXC_MIN_MV    7000      // datasheet supply range 7-12 V

// Cal certificate values. Replace with the numbers on the Druck's cert.
#define DRUCK_CAL_EXC_MV     10000     // supply voltage the cert was taken at
#define DRUCK_CAL_ZERO_UV    0         // output at 0 mbar absolute, uV
#define DRUCK_CAL_SPAN_UV    100000    // output at full scale minus zero, uV
#define DRUCK_CAL_NOMINAL    1         // set to 0 once the cert values are in

#define DRUCK_FS_MMBAR       1034214   // 15 psia in 0.001 mbar

static char druck_value[24];

// Pressure in 0.001 mbar from the INA228 raw readings.
static int64_t druck_pressure_mmbar(int32_t raw_shunt, int32_t raw_bus, int64_t *r_ppb)
{
    const int64_t r0 = (int64_t)DRUCK_CAL_ZERO_UV * 1000000 / DRUCK_CAL_EXC_MV;
    const int64_t rspan = (int64_t)DRUCK_CAL_SPAN_UV * 1000000 / DRUCK_CAL_EXC_MV;

    *r_ppb = (int64_t)raw_shunt * 1600000 / raw_bus;
    return (*r_ppb - r0) * DRUCK_FS_MMBAR / rspan;
}

static void druck_update(int32_t raw_shunt, int32_t raw_bus, int32_t bus_mv)
{
    if (bus_mv < DRUCK_VEXC_MIN_MV) {
        lv_snprintf(druck_value, sizeof druck_value, "----");
        lv_snprintf(druck_status, STATUS_LEN,
                    "Druck: excitation %ld mV, below 7 V (VBUS not wired?)", (long)bus_mv);
        return;
    }

    int64_t r_ppb;
    int64_t p = druck_pressure_mmbar(raw_shunt, raw_bus, &r_ppb);

    // Round to 0.01 mbar.
    int64_t pc = (p >= 0 ? p + 5 : p - 5) / 10;
    int64_t pa = pc < 0 ? -pc : pc;
    // r in mV/V to 5 decimals (units of 10 ppb).
    int64_t r10 = r_ppb / 10;
    int64_t ra = r10 < 0 ? -r10 : r10;

    lv_snprintf(druck_value, sizeof druck_value, "%s%ld.%02ld",
                pc < 0 ? "-" : "", (long)(pa / 100), (long)(pa % 100));
    lv_snprintf(druck_status, STATUS_LEN,
                "Druck: %s mbar abs  r %s%ld.%05ld mV/V  exc %ld mV  %s",
                druck_value, r10 < 0 ? "-" : "", (long)(ra / 100000), (long)(ra % 100000),
                (long)bus_mv, DRUCK_CAL_NOMINAL ? "nominal cal" : "cert cal");
}

static void powermonitor_poll(void)
{
    uint8_t b[3];

    if (pm_addr == 0) {
        pm_ack_addr = 0;
        for (uint8_t a = 0x40; a <= 0x4F && pm_addr == 0; a++)
            if (pm_probe(a))
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
            else
                lv_snprintf(powermonitor_status, STATUS_LEN,
                            "S4 Power Monitor: NOT FOUND (no reply at 0x40-0x4F,\n"
                            "   idle SCL %d SDA %d)", bb_scl_idle, bb_sda_idle);
            lv_snprintf(druck_value, sizeof druck_value, "----");
            lv_snprintf(druck_status, STATUS_LEN, "Druck: no reading (Power Monitor not found)");
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

    boost10_check(bus_mv);
    druck_update(raw_shunt, raw_bus, bus_mv);

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
    lv_snprintf(druck_value, sizeof druck_value, "----");
    lv_snprintf(druck_status, STATUS_LEN, "Druck: no reading (Power Monitor lost)");
}

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

const char *clicks_stepper3_status(void)     { return stepper3_status; }
const char *clicks_boost10_status(void)      { return boost10_status; }
const char *clicks_powermonitor_status(void) { return powermonitor_status; }
const char *clicks_druck_status(void)        { return druck_status; }
const char *clicks_druck_value(void)         { return druck_value; }
bool clicks_druck_cal_nominal(void)          { return DRUCK_CAL_NOMINAL; }
