/* The simulated board. main.c is compiled in here unchanged (its main() is
 * renamed and not used); sim_run() is its main loop, one pass per
 * simulated millisecond, with the TIM7 step interrupt, the 1 ms tick and
 * the bench model run in between. The page (simulator/index.html) calls
 * the exported sim_* functions. */
#include "mcu.h"
#include "delays.h"
// main.c includes InitialTesting's 1ms_Timer.h, which defines nothing here.
static inline void timerInit(void) {}
#define INTERRUPT_ROUTINE void sim_unused_tick_isr(void)
#define CLEAR_FLAG ((void)0)
#define main fw_main_unused
#include "../../InitialTesting/main.c"
#undef main

#include <stdarg.h>
#include <time.h>

#include "usb_serial.h"
#include "extflash.h"
#include "bench.h"

#define EXPORT(name) __attribute__((export_name(#name)))
#define IMPORT(name) __attribute__((import_module("env"), import_name(#name)))

IMPORT(js_halt) void js_halt(const char *why);

/* ---- registers ------------------------------------------------------------ */
GPIO_TypeDef sim_gpio[7];
RCC_TypeDef sim_rcc;
TIM_TypeDef sim_tim7;
ADC_TypeDef sim_adc1;
ADC_Common_TypeDef sim_adc;
RTC_TypeDef sim_rtc;
PWR_TypeDef sim_pwr;
IWDG_TypeDef sim_iwdg;
volatile uint32_t sim_nvic_dummy;
const uint16_t sim_vrefint_cal = 1500;   // VREFINT 1.209 V at VDDA 3.3 V

void sim_halt(const char *why)
{
    js_halt(why);
    __builtin_trap();
}

/* ---- serial flash: 8 MB in RAM, kept by the page between visits ---------- */
#define FLASH_SIZE (8u * 1024 * 1024)
static uint8_t flash[FLASH_SIZE];
static int flash_dirty;

bool extflash_init(void) { return true; }
uint32_t extflash_jedec_id(void) { return 0xBF2643; }
void extflash_read(uint32_t addr, void *buf, size_t len)
{
    for (size_t i = 0; i < len; i++)
        ((uint8_t *)buf)[i] = flash[(addr + i) % FLASH_SIZE];
}
bool extflash_erase_4k(uint32_t addr)
{
    memset(flash + (addr & ~0xFFFu) % FLASH_SIZE, 0xFF, 4096);
    flash_dirty = 1;
    return true;
}
bool extflash_write(uint32_t addr, const void *buf, size_t len)
{
    for (size_t i = 0; i < len; i++)
        flash[(addr + i) % FLASH_SIZE] &= ((const uint8_t *)buf)[i];   // NOR: only 1 -> 0
    flash_dirty = 1;
    return true;
}

/* ---- USB serial: the page's console -------------------------------------- */
#define TX_SIZE 65536
static char tx[TX_SIZE];
static uint32_t tx_len;
static char rx[512];
static uint32_t rx_head, rx_tail;
static link_line_t rx_line;
static uint32_t rx_bytes;

void usb_serial_init(void) {}
void usb_serial_task(void) {}
const char *usb_serial_status(void) { return "USB: port open (simulator console)"; }
size_t usb_serial_tx_room(void) { return TX_SIZE - tx_len; }
uint32_t usb_serial_rx_bytes(void) { return rx_bytes; }
void usb_serial_printf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = lv_vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof buf) n = sizeof buf - 1;
    if (tx_len + (uint32_t)n > TX_SIZE) return;   // the page has not read: dropped, as a full USB buffer
    memcpy(tx + tx_len, buf, (size_t)n);
    tx_len += (uint32_t)n;
}
bool usb_serial_getline(char *buf, size_t n)
{
    while (rx_tail != rx_head) {
        char c = rx[rx_tail];
        rx_tail = (rx_tail + 1) % sizeof rx;
        if (link_line_feed(&rx_line, c, buf, n))
            return true;
    }
    return false;
}

/* ---- display and touch ---------------------------------------------------- */
#define W 480
#define H 272
static uint16_t fb[W * H];
static uint8_t draw_buf[W * 25 * 2] __attribute__((aligned(4)));   // 25 rows, as the board (0.11.18)
static uint32_t frames;
static int touch_x, touch_y, touch_down;
static uint32_t touch_presses;
static bool touch_was;

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    const uint16_t *p = (const uint16_t *)px;
    for (int y = a->y1; y <= a->y2; y++)
        for (int x = a->x1; x <= a->x2; x++)
            fb[y * W + x] = *p++;
    disp_flush_px += (uint32_t)lv_area_get_size(a);
    frames++;
    lv_display_flush_ready(d);
}

uint32_t disp_flush_ms, disp_flush_px;

void lv_port_disp_init(void)
{
    lv_display_t *d = lv_display_create(W, H);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d, draw_buf, NULL, sizeof draw_buf, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d, flush_cb);
}

static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point.x = (int32_t)touch_x;
    data->point.y = (int32_t)touch_y;
    data->state = touch_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    if (touch_down && !touch_was) touch_presses++;
    touch_was = touch_down;
}

void lv_port_indev_init(void)
{
    lv_indev_t *in = lv_indev_create();
    lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(in, touch_read);
}

void touch_poll(void) {}

bool touch_command(const char *line)
{
    if (strcmp(line, "TOUCH?") != 0)
        return false;
    link_printf("TOUCH reads=0 errors=0 last_err=0 presses=%lu now=%s last=%d,%d\r\nOK\r\n",
                (unsigned long)touch_presses, touch_down ? "pressed" : "released", touch_x, touch_y);
    return true;
}

/* ---- the step timer, the coils and the limit and supply inputs ------------ */
void TIM7_IRQHandler(void);
static uint32_t tim_cnt, tim_arr_active;

// Pin state from the last BSRR write on each of the four Stepper 3 outputs.
static void coils_from_pins(void)
{
    static GPIO_TypeDef *const port[4] = { &sim_gpio[0], &sim_gpio[2], &sim_gpio[1], &sim_gpio[3] };
    static const uint8_t pin[4] = { 4, 2, 12, 12 };
    uint8_t out = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t bsrr = port[i]->BSRR;
        if (bsrr & (1UL << pin[i])) port[i]->ODR |= 1UL << pin[i];
        if (bsrr & (1UL << (pin[i] + 16))) port[i]->ODR &= ~(1UL << pin[i]);
        port[i]->BSRR = 0;
        if (port[i]->ODR & (1UL << pin[i])) out |= 1u << i;
    }
    // The bench motor is wired as found on 8 Oct: coil n on output 0213[n].
    static const uint8_t wiring[4] = { 0, 2, 1, 3 };
    uint8_t coils = 0;
    for (int c = 0; c < 4; c++)
        if (out & (1u << wiring[c])) coils |= 1u << c;
    if (coils != B.coils || coils)
        bench_coils(coils);
}

static void inputs_update(void)
{
    if (bench_limit_pin_high()) sim_gpio[3].IDR |= 1UL << 13;
    else sim_gpio[3].IDR &= ~(1UL << 13);
}

// ADC1: IN3 is the motor supply, IN17 VREFINT; a conversion is done at once.
ADC_TypeDef *sim_adc1_access(void)
{
    sim_adc1.SR |= ADC_SR_EOC;
    sim_adc1.DR = sim_adc1.SQR3 == 17 ? sim_vrefint_cal : (uint32_t)bench_supply_adc();
    return &sim_adc1;
}

// One simulated millisecond of TIM7 (1 MHz count). Update events run the
// step interrupt; EGR UG reloads at once; ARR takes effect at the next
// update when ARPE is set.
static void tim7_ms(void)
{
    uint32_t left = 1000;
    for (int guard = 0; guard < 2000; guard++) {
        if (sim_tim7.EGR & TIM_EGR_UG) {
            sim_tim7.EGR = 0;
            tim_cnt = 0;
            tim_arr_active = sim_tim7.ARR;
        }
        if (!(sim_tim7.CR1 & TIM_CR1_CEN) || !(sim_tim7.DIER & TIM_DIER_UIE))
            return;
        if (!(sim_tim7.CR1 & TIM_CR1_ARPE))
            tim_arr_active = sim_tim7.ARR;
        uint32_t to_update = tim_arr_active + 1 - tim_cnt;
        if (to_update > left) {
            tim_cnt += left;
            return;
        }
        left -= to_update;
        tim_cnt = 0;
        tim_arr_active = sim_tim7.ARR;
        inputs_update();   // the ISR reads the limit pin before a closing step
        TIM7_IRQHandler();
        coils_from_pins();
    }
}

/* ---- the RTC: one second a second from the page's clock ------------------- */
static int bcd2(uint32_t v) { return (int)((v >> 4) & 0xF) * 10 + (int)(v & 0xF); }
static uint32_t bcd(int v) { return (uint32_t)(((v / 10) << 4) | (v % 10)); }

static void rtc_set(time_t t)
{
    struct tm *tm = gmtime(&t);
    sim_rtc.TR = bcd(tm->tm_hour) << 16 | bcd(tm->tm_min) << 8 | bcd(tm->tm_sec);
    sim_rtc.DR = bcd(tm->tm_year - 100) << 16 | (uint32_t)(tm->tm_wday ? tm->tm_wday : 7) << 13 |
                 bcd(tm->tm_mon + 1) << 8 | bcd(tm->tm_mday);
}

static void rtc_second(void)
{
    struct tm tm = { 0 };
    tm.tm_hour = bcd2(sim_rtc.TR >> 16 & 0x3F);
    tm.tm_min = bcd2(sim_rtc.TR >> 8 & 0x7F);
    tm.tm_sec = bcd2(sim_rtc.TR & 0x7F);
    tm.tm_year = bcd2(sim_rtc.DR >> 16 & 0xFF) + 100;
    tm.tm_mon = bcd2(sim_rtc.DR >> 8 & 0x1F) - 1;
    tm.tm_mday = bcd2(sim_rtc.DR & 0x3F);
    rtc_set(timegm(&tm) + 1);
}

/* ---- the page's calls ----------------------------------------------------- */
static uint32_t ms_count, sim_ms;
static uint32_t sense_ms;

EXPORT(sim_flash) uint8_t *sim_flash(void) { return flash; }
EXPORT(sim_flash_size) uint32_t sim_flash_size(void) { return FLASH_SIZE; }
EXPORT(sim_flash_take_dirty) int sim_flash_take_dirty(void) { int d = flash_dirty; flash_dirty = 0; return d; }
EXPORT(sim_flash_erase_all) void sim_flash_erase_all(void) { memset(flash, 0xFF, sizeof flash); }

EXPORT(sim_fb) uint16_t *sim_fb(void) { return fb; }
EXPORT(sim_frames) uint32_t sim_frames(void) { return frames; }
EXPORT(sim_touch) void sim_touch(int x, int y, int down) { touch_x = x; touch_y = y; touch_down = down; }

EXPORT(sim_rx_buf) char *sim_rx_buf(void) { static char b[256]; return b; }
EXPORT(sim_rx) void sim_rx(int n)
{
    const char *b = sim_rx_buf();
    for (int i = 0; i < n && i < 256; i++) {
        uint32_t next = (rx_head + 1) % sizeof rx;
        if (next == rx_tail) break;
        rx[rx_head] = b[i];
        rx_head = next;
        rx_bytes++;
    }
}
EXPORT(sim_tx) char *sim_tx(void) { return tx; }
EXPORT(sim_tx_len) uint32_t sim_tx_len(void) { return tx_len; }
EXPORT(sim_tx_clear) void sim_tx_clear(void) { tx_len = 0; }

EXPORT(sim_ms) uint32_t sim_ms_now(void) { return sim_ms; }

// Bench values for the page: get and set by number (see index.html).
EXPORT(sim_get) double sim_get(int id)
{
    switch (id) {
    case 0: return B.P;
    case 1: return B.T;
    case 2: return B.mL;
    case 3: return B.he;
    case 4: return B.pump;
    case 5: return B.needle_half;
    case 6: return B.coupled;
    case 7: return B.limit_fault;
    case 8: return B.druck;
    case 9: return B.pm;
    case 10: return B.exc_v;
    case 11: return B.vent;
    case 12: return B.supply_v;
    case 13: return B.die_c;
    case 14: return B.lim_raw;
    case 15: return B.coils;
    case 16: return B.slipped;
    case 17: return B.stalled;
    case 18: return B.early_half;
    case 19: return B.room_mbar;
    case 30: return stepper3_position();
    case 31: return valve_pct_milli();
    default: return 0;
    }
}
EXPORT(sim_set) void sim_set(int id, double v)
{
    switch (id) {
    case 3: B.he = v != 0; break;
    case 4: B.pump = v != 0; break;
    case 5: B.needle_half = (int32_t)v; break;
    case 6: B.coupled = v != 0; break;
    case 7: B.limit_fault = (int)v; break;
    case 8: B.druck = v != 0; break;
    case 9: B.pm = v != 0; break;
    case 10: B.exc_v = v; break;
    case 11: B.vent = v != 0; break;
    case 12: B.supply_v = v; break;
    case 13: B.die_c = v; break;
    case 18: B.early_half = (int32_t)v; break;
    case 19: B.room_mbar = v; break;
    case 40: B.P = v; break;
    case 41: B.T = v; break;
    case 42: B.mL = v; break;
    default: break;
    }
}

// Power on. reset: 0 power-on, 1 watchdog. now: the page's clock, s since 1970 (local time).
EXPORT(sim_bench_init) void sim_bench_init(void) { bench_init(); }
EXPORT(sim_boot) void sim_boot(int reset, double now)
{
    sim_rcc.CSR = reset ? RCC_CSR_IWDGRSTF | RCC_CSR_PINRSTF : RCC_CSR_PORRSTF | RCC_CSR_PINRSTF;
    sim_rcc.CFGR = RCC_CFGR_SWS_HSE;   // only read for the timer clock: 25 MHz, PSC 24
    sim_rcc.BDCR = RCC_BDCR_LSERDY | RCC_BDCR_LSEON | RCC_BDCR_RTCEN | (1UL << 8);
    sim_rtc.ISR = RTC_ISR_INITS | RTC_ISR_RSF | RTC_ISR_INITF;
    rtc_set((time_t)now);
    sim_gpio[3].IDR = 0;
    inputs_update();
    application_init();
    sense_ms = lv_tick_get();
    sim_ms = 0;
}

// The main loop, ms passes of it.
EXPORT(sim_run) void sim_run(int ms)
{
    char line[96];
    for (int i = 0; i < ms; i++) {
        tim7_ms();
        if (++ms_count == 5) {
            ms_count = 0;
            lv_tick_inc(5);
        }
        sim_ms++;
        if (sim_ms % 10 == 0) bench_tick_10ms();
        if (sim_ms % 1000 == 0) rtc_second();
        inputs_update();

        // main.c's loop body.
        link_task();
        touch_poll();
        rtclock_poll();
        limit_poll();
        supply_poll();
        stepper3_poll();
        valve_task();
        valvepos_task();
        trend_task();
        if (lv_tick_elaps(sense_ms) >= 1000) {
            sense_ms += 1000;
            if (lv_tick_elaps(sense_ms) >= 1000)
                sense_ms = lv_tick_get();
            sense_second();
        }
        bool got = link_getline(line, sizeof line);
        if (got)
            link_printf("> %s\r\n", line);
        if (got && !help_command(line) && !cfg_command(line) && !cal_command(line) && !sysinfo_command(line) && !trend_command(line) && !rtclock_command(line) && !touch_command(line) && !perf_command(line) &&
            !limit_command(line) && !supply_command(line) && !valvepos_command(line) && !valve_command(line) && !stepper3_command(line) &&
            !sense_data_command(line))
            link_printf("ERR unknown command\r\n");
        uint32_t t0 = lv_tick_get();
        lv_timer_handler();
        perf_note(t0);
        coils_from_pins();   // a stop or coils-off from the main loop
    }
}

EXPORT(sim_version) const char *sim_version(void) { return PMC_FW_VERSION; }
