#include <stdlib.h>
#include <string.h>

#include "mcu.h"
#include "board.h"
#include "drv_digital_out.h"
#include "lvgl.h"   // lv_snprintf
#include "link.h"
#include "pmc_config.h"
#include "stepper3.h"

/* --------------------------------------------------------------------------
 * Hardware
 *
 * Stepper 3 in socket PMC_STEPPER3_SOCKET (pmc_config.h):
 *   S1: AN PA4, RST PC2, CS PB12, PWM PD12
 *   S2: AN PB0, RST PC3, CS PA15, PWM PD13
 * These drive ULN2003 inputs 1-4 (high = that output pulls its coil end to ground). The motor centre
 * taps go to the COM terminal, which J1 connects to the external supply
 * (VCC-EXT, 12 V for the Portescap motors).
 *
 * The pins are set up through mikroSDK once, then written directly through
 * BSRR from the timer interrupt.
 *
 * TIM7 (basic timer, unused elsewhere) counts at 1 MHz; one update
 * interrupt per step. Its input clock is read back from RCC at each move,
 * because the Ethernet build changes SYSCLK (168 -> 150 MHz) after start-up.
 * ------------------------------------------------------------------------ */

typedef struct {
    GPIO_TypeDef *port;
    uint16_t pin;
} out_pin_t;

#if PMC_STEPPER3_SOCKET == 1
static const out_pin_t out_pins[4] = {
    { GPIOA, 4 },    // MIKROBUS_1_AN  -> ULN2003 IN1
    { GPIOC, 2 },    // MIKROBUS_1_RST -> IN2
    { GPIOB, 12 },   // MIKROBUS_1_CS  -> IN3
    { GPIOD, 12 },   // MIKROBUS_1_PWM -> IN4
};
#define SOCKET_PINS { MIKROBUS_1_AN, MIKROBUS_1_RST, MIKROBUS_1_CS, MIKROBUS_1_PWM }
#elif PMC_STEPPER3_SOCKET == 2
static const out_pin_t out_pins[4] = {
    { GPIOB, 0 },    // MIKROBUS_2_AN  -> ULN2003 IN1
    { GPIOC, 3 },    // MIKROBUS_2_RST -> IN2
    { GPIOA, 15 },   // MIKROBUS_2_CS  -> IN3
    { GPIOD, 13 },   // MIKROBUS_2_PWM -> IN4
};
#define SOCKET_PINS { MIKROBUS_2_AN, MIKROBUS_2_RST, MIKROBUS_2_CS, MIKROBUS_2_PWM }
#else
#error "PMC_STEPPER3_SOCKET must be 1 or 2"
#endif

static digital_out_t sdk_pins[4];

// Half-step sequence, bit n = coil n on. Even entries one coil (wave),
// odd entries two coils (full step).
static const uint8_t seq[8] = { 0x1, 0x3, 0x2, 0x6, 0x4, 0xC, 0x8, 0x9 };

#define TICK_HZ        1000000UL
#define SETTLE_US      20000UL    // coils stay on this long after the last step
#define RATE_MIN       16         // 1 MHz / 65536, the 16-bit ARR limit
#define RATE_MAX       1000
#define ACCEL_MIN      10
#define ACCEL_MAX      20000

/* --------------------------------------------------------------------------
 * Settings (RAM only; defaults from the control logic: run 200, creep 25)
 * ------------------------------------------------------------------------ */
static uint32_t rate_run   = 200;    // steps/s
static uint32_t rate_start = 25;     // steps/s, ramp starts and ends here
static uint32_t accel      = 400;    // steps/s^2
static stepper3_mode_t mode = STEPPER3_FULL;
static bool hold;                    // coils on when idle
static bool reverse;                 // flips the sequence direction
static uint8_t order[4] = { 0, 2, 1, 3 };   // coil n -> output order[n]; 0213 found on the bench (8 Oct, 26M motor)

/* --------------------------------------------------------------------------
 * Motion state (written by the ISR while moving)
 * ------------------------------------------------------------------------ */
typedef enum { ST_IDLE, ST_MOVING, ST_SETTLE } run_state_t;

static volatile run_state_t run_state = ST_IDLE;
static volatile int32_t position;       // steps of the current mode
static volatile uint32_t remaining;
static volatile stepper3_stop_t last_stop = STEPPER3_STOP_NONE;
static int8_t dir;                      // +1 open, -1 close
static uint8_t phase;                   // index into seq[]
static bool energised;
static uint32_t v_fp;                   // current rate, steps/s * 256
static uint32_t v_run_fp, v_start_fp;
static uint32_t n_acc;                  // steps spent accelerating

static char status_text[160];

/* --------------------------------------------------------------------------
 * Limit switch (one NC dry contact at the closed end of travel)
 *
 * To fit: read DIGI IN IN1 here and set LIMIT_FITTED to 1. An open contact
 * (at the limit, or a broken wire) must return true.
 * ------------------------------------------------------------------------ */
#define LIMIT_FITTED 0

bool stepper3_limit_fitted(void) { return LIMIT_FITTED; }

bool stepper3_limit_open(void)
{
    return false;
}

/* --------------------------------------------------------------------------
 * Coil outputs
 * ------------------------------------------------------------------------ */
static void write_coils(uint8_t coils)
{
    uint8_t out = 0;
    for (int c = 0; c < 4; c++)
        if (coils & (1u << c))
            out |= 1u << order[c];
    for (int i = 0; i < 4; i++) {
        uint32_t bit = 1UL << out_pins[i].pin;
        out_pins[i].port->BSRR = (out & (1u << i)) ? bit : bit << 16;
    }
}

static void coils_off(void)
{
    write_coils(0);
    energised = false;
}

/* --------------------------------------------------------------------------
 * TIM7
 * ------------------------------------------------------------------------ */
static uint32_t apb1_timer_hz(void)
{
    static const uint16_t ahb_div[16] = { 1, 1, 1, 1, 1, 1, 1, 1, 2, 4, 8, 16, 64, 128, 256, 512 };
    static const uint8_t apb_div[8] = { 1, 1, 1, 1, 2, 4, 8, 16 };
    uint32_t cfgr = RCC->CFGR;
    uint32_t sys;

    switch (cfgr & RCC_CFGR_SWS) {
    case RCC_CFGR_SWS_HSI:
        sys = 16000000UL;
        break;
    case RCC_CFGR_SWS_HSE:
        sys = 25000000UL;          // board crystal
        break;
    default: {
        uint32_t pll = RCC->PLLCFGR;
        uint64_t in = (pll & RCC_PLLCFGR_PLLSRC) ? 25000000ULL : 16000000ULL;
        uint32_t m = (pll & RCC_PLLCFGR_PLLM_Msk) >> RCC_PLLCFGR_PLLM_Pos;
        uint32_t n = (pll & RCC_PLLCFGR_PLLN_Msk) >> RCC_PLLCFGR_PLLN_Pos;
        uint32_t p = (((pll & RCC_PLLCFGR_PLLP_Msk) >> RCC_PLLCFGR_PLLP_Pos) + 1) * 2;
        sys = (uint32_t)(in * n / m / p);
        break;
    }
    }
    uint32_t hclk = sys / ahb_div[(cfgr & RCC_CFGR_HPRE_Msk) >> RCC_CFGR_HPRE_Pos];
    uint32_t d1 = apb_div[(cfgr & RCC_CFGR_PPRE1_Msk) >> RCC_CFGR_PPRE1_Pos];
    // Timers on APB1 run at twice PCLK1 when the APB1 divider is not 1.
    return d1 == 1 ? hclk : hclk / d1 * 2;
}

static inline uint32_t period_us(uint32_t vfp)
{
    return (uint32_t)((256ULL * TICK_HZ) / vfp);
}

// NVIC by address: the mikroSDK headers do not bring in the CMSIS NVIC helpers.
#define NVIC_ISER(n)  (*(volatile uint32_t *)(0xE000E100UL + 4 * (n)))
#define NVIC_ICPR(n)  (*(volatile uint32_t *)(0xE000E280UL + 4 * (n)))
#define NVIC_IPR8(n)  (*(volatile uint8_t  *)(0xE000E400UL + (n)))
#define TIM7_IRQ      TIM7_IRQn
#define TIM7_PRIO     6       // 4 priority bits on the F407

static void timer_stop(void)
{
    TIM7->DIER = 0;
    TIM7->CR1 = 0;
    TIM7->SR = 0;
    NVIC_ICPR(TIM7_IRQ / 32) = 1UL << (TIM7_IRQ % 32);
}

static void timer_start(uint32_t us)
{
    RCC->APB1ENR |= RCC_APB1ENR_TIM7EN;
    (void)RCC->APB1ENR;
    TIM7->CR1 = 0;
    TIM7->PSC = apb1_timer_hz() / TICK_HZ - 1;
    TIM7->ARR = us - 1;
    TIM7->EGR = TIM_EGR_UG;          // load PSC and ARR now
    TIM7->SR = 0;
    TIM7->DIER = TIM_DIER_UIE;
    NVIC_IPR8(TIM7_IRQ) = TIM7_PRIO << 4;
    NVIC_ISER(TIM7_IRQ / 32) = 1UL << (TIM7_IRQ % 32);
    TIM7->CR1 = TIM_CR1_ARPE | TIM_CR1_CEN;
}

static void finish(stepper3_stop_t why)
{
    last_stop = why;
    run_state = ST_SETTLE;
    TIM7->ARR = SETTLE_US - 1;
}

void TIM7_IRQHandler(void)
{
    TIM7->SR = 0;

    if (run_state == ST_SETTLE) {
        timer_stop();
        if (!hold)
            coils_off();
        run_state = ST_IDLE;
        return;
    }
    if (run_state != ST_MOVING) {
        timer_stop();
        return;
    }

    if (dir < 0 && stepper3_limit_open()) {
        finish(STEPPER3_STOP_LIMIT);
        return;
    }

    int8_t inc = (mode == STEPPER3_HALF) ? 1 : 2;
    if (reverse != (dir < 0))
        inc = -inc;
    phase = (uint8_t)(phase + inc) & 7;
    write_coils(seq[phase]);
    position += dir;

    if (--remaining == 0) {
        finish(STEPPER3_STOP_DONE);
        return;
    }

    // Linear ramp: dv = a * dt = a / v per step. Decelerate once the steps
    // left are no more than the steps it took to accelerate.
    uint32_t dv = (uint32_t)(((uint64_t)accel << 16) / v_fp);
    if (remaining <= n_acc) {
        v_fp = (v_fp > v_start_fp + dv) ? v_fp - dv : v_start_fp;
        n_acc--;
    } else if (v_fp < v_run_fp) {
        v_fp += dv;
        if (v_fp > v_run_fp)
            v_fp = v_run_fp;
        n_acc++;
    }
    TIM7->ARR = period_us(v_fp) - 1;
}

/* --------------------------------------------------------------------------
 * API
 * ------------------------------------------------------------------------ */
void stepper3_init(void)
{
    const pin_name_t pins[4] = SOCKET_PINS;

    for (int i = 0; i < 4; i++) {
        digital_out_init(&sdk_pins[i], pins[i]);
        digital_out_low(&sdk_pins[i]);
    }
    coils_off();
}

bool stepper3_move(int32_t steps)
{
    if (run_state != ST_IDLE || steps == 0 ||
        steps > STEPPER3_MAX_MOVE || steps < -STEPPER3_MAX_MOVE)
        return false;
    if (steps < 0 && stepper3_limit_open()) {
        last_stop = STEPPER3_STOP_LIMIT;
        return false;
    }

    // Wave uses the one-coil entries, full the two-coil entries.
    if (mode == STEPPER3_WAVE)
        phase &= ~1u;
    else if (mode == STEPPER3_FULL)
        phase |= 1u;
    write_coils(seq[phase]);
    energised = true;

    dir = steps > 0 ? 1 : -1;
    remaining = (uint32_t)(steps > 0 ? steps : -steps);
    v_start_fp = (rate_start < rate_run ? rate_start : rate_run) << 8;
    v_run_fp = rate_run << 8;
    v_fp = v_start_fp;
    n_acc = 0;
    last_stop = STEPPER3_STOP_NONE;
    run_state = ST_MOVING;
    // First step one start-rate period after the coils are energised.
    timer_start(period_us(v_fp));
    return true;
}

void stepper3_stop(void)
{
    timer_stop();
    if (run_state == ST_MOVING)
        last_stop = STEPPER3_STOP_CMD;
    run_state = ST_IDLE;
    if (!hold)
        coils_off();
}

void stepper3_off(void)
{
    stepper3_stop();
    coils_off();
}

bool stepper3_busy(void)               { return run_state != ST_IDLE; }
int32_t stepper3_position(void)        { return position; }
stepper3_stop_t stepper3_last_stop(void) { return last_stop; }

void stepper3_zero(void)
{
    if (run_state == ST_IDLE)
        position = 0;
}

const char *stepper3_status(void)
{
    const char *st = run_state == ST_MOVING ? (dir > 0 ? "opening" : "closing")
                   : energised ? "idle, coils on" : "idle, coils off";
    lv_snprintf(status_text, sizeof status_text,
                "S%d Stepper 3: %s, pos %ld, %s %lu sps, limit %s", PMC_STEPPER3_SOCKET,
                st, (long)position, stepper3_mode_name(mode), (unsigned long)rate_run,
                !LIMIT_FITTED ? "not fitted" : stepper3_limit_open() ? "OPEN" : "ok");
    return status_text;
}

/* --------------------------------------------------------------------------
 * Settings, shared by the MOT commands and the motor screen. Each setter
 * refuses (returns false) while a move is running or for a value out of
 * range, so the step timing and sequence never change mid-move.
 * ------------------------------------------------------------------------ */
bool stepper3_set_rate(uint32_t sps)
{
    if (run_state != ST_IDLE || sps < RATE_MIN || sps > RATE_MAX)
        return false;
    rate_run = sps;
    return true;
}

bool stepper3_set_start(uint32_t sps)
{
    if (run_state != ST_IDLE || sps < RATE_MIN || sps > RATE_MAX)
        return false;
    rate_start = sps;
    return true;
}

bool stepper3_set_accel(uint32_t sps2)
{
    if (run_state != ST_IDLE || sps2 < ACCEL_MIN || sps2 > ACCEL_MAX)
        return false;
    accel = sps2;
    return true;
}

bool stepper3_set_mode(stepper3_mode_t nm)
{
    if (run_state != ST_IDLE || nm > STEPPER3_HALF)
        return false;
    // Keep the position in the new step size.
    if (mode == STEPPER3_HALF && nm != STEPPER3_HALF)
        position /= 2;
    else if (mode != STEPPER3_HALF && nm == STEPPER3_HALF)
        position *= 2;
    mode = nm;
    if (energised)
        coils_off();     // re-energised on the right entry at the next move
    return true;
}

bool stepper3_set_hold(bool on)
{
    if (run_state != ST_IDLE)
        return false;
    hold = on;
    if (!hold)
        coils_off();
    return true;
}

bool stepper3_set_reverse(bool on)
{
    if (run_state != ST_IDLE)
        return false;
    reverse = on;
    return true;
}

bool stepper3_set_order(const uint8_t o[4])
{
    uint8_t seen = 0;
    if (run_state != ST_IDLE)
        return false;
    for (int i = 0; i < 4; i++) {
        if (o[i] > 3 || (seen & (1u << o[i])))
            return false;
        seen |= 1u << o[i];
    }
    memcpy(order, o, sizeof order);
    if (energised)
        coils_off();
    return true;
}

uint32_t stepper3_rate(void)            { return rate_run; }
uint32_t stepper3_start_rate(void)      { return rate_start; }
uint32_t stepper3_accel(void)           { return accel; }
stepper3_mode_t stepper3_mode(void)     { return mode; }
bool stepper3_hold(void)                { return hold; }
bool stepper3_reverse(void)             { return reverse; }
bool stepper3_energised(void)           { return energised; }
int8_t stepper3_direction(void)         { return run_state == ST_MOVING ? dir : 0; }
void stepper3_order(uint8_t o[4])       { memcpy(o, order, sizeof order); }
const char *stepper3_mode_name(stepper3_mode_t m)
{
    return m == STEPPER3_WAVE ? "WAVE" : m == STEPPER3_FULL ? "FULL" : "HALF";
}
const char *stepper3_stop_name(stepper3_stop_t s)
{
    switch (s) {
    case STEPPER3_STOP_DONE:  return "done";
    case STEPPER3_STOP_CMD:   return "stopped";
    case STEPPER3_STOP_LIMIT: return "LIMIT";
    default:                  return "none";
    }
}

/* --------------------------------------------------------------------------
 * MOT commands
 * ------------------------------------------------------------------------ */
static bool parse_int(const char *s, long lo, long hi, long *out)
{
    char *end;
    while (*s == ' ')
        s++;
    if (!*s)
        return false;
    long v = strtol(s, &end, 10);
    while (*end == ' ')
        end++;
    if (*end || v < lo || v > hi)
        return false;
    *out = v;
    return true;
}

static void show(void)
{
    link_printf("MOT state %s, pos %ld, remaining %lu, last stop %s\r\n",
                run_state == ST_MOVING ? "moving" : run_state == ST_SETTLE ? "settling" : "idle",
                (long)position, (unsigned long)remaining, stepper3_stop_name(last_stop));
    link_printf("MOT mode %s, rate %lu sps, start %lu sps, accel %lu sps/s\r\n",
                stepper3_mode_name(mode), (unsigned long)rate_run, (unsigned long)rate_start,
                (unsigned long)accel);
    link_printf("MOT hold %d, dir %d, order %d%d%d%d, coils %s, limit %s\r\n",
                hold, reverse, order[0], order[1], order[2], order[3],
                energised ? "on" : "off",
                !LIMIT_FITTED ? "not fitted" : stepper3_limit_open() ? "OPEN" : "closed (ok)");
    link_printf("OK\r\n");
}

bool stepper3_command(const char *line)
{
    if (strncmp(line, "MOT", 3) != 0)
        return false;
    const char *a = line + 3;
    long v;

    if (strcmp(a, "?") == 0) {
        show();
    } else if (strcmp(a, " STOP") == 0) {
        stepper3_stop();
        link_printf("OK stopped at %ld\r\n", (long)position);
    } else if (strcmp(a, " OFF") == 0) {
        stepper3_off();
        link_printf("OK coils off at %ld\r\n", (long)position);
    } else if (strncmp(a, " MOVE", 5) == 0) {
        if (!parse_int(a + 5, -STEPPER3_MAX_MOVE, STEPPER3_MAX_MOVE, &v) || v == 0)
            link_printf("ERR expected: MOT MOVE <steps>, 1 to %d either way\r\n", STEPPER3_MAX_MOVE);
        else if (run_state != ST_IDLE)
            link_printf("ERR busy, MOT STOP first\r\n");
        else if (!stepper3_move((int32_t)v))
            link_printf("ERR limit open, closing move refused\r\n");
        else
            link_printf("OK moving %ld from %ld\r\n", v, (long)(position));
    } else if (run_state != ST_IDLE) {
        // Settings below change the step timing or sequence mid-move.
        link_printf("ERR busy, MOT STOP first\r\n");
    } else if (strcmp(a, " ZERO") == 0) {
        stepper3_zero();
        link_printf("OK\r\n");
    } else if (strncmp(a, " RATE", 5) == 0) {
        if (!parse_int(a + 5, RATE_MIN, RATE_MAX, &v)) {
            link_printf("ERR expected: MOT RATE <%d-%d>\r\n", RATE_MIN, RATE_MAX);
        } else {
            stepper3_set_rate((uint32_t)v);
            link_printf("OK\r\n");
        }
    } else if (strncmp(a, " START", 6) == 0) {
        if (!parse_int(a + 6, RATE_MIN, RATE_MAX, &v)) {
            link_printf("ERR expected: MOT START <%d-%d>\r\n", RATE_MIN, RATE_MAX);
        } else {
            stepper3_set_start((uint32_t)v);
            link_printf("OK\r\n");
        }
    } else if (strncmp(a, " ACCEL", 6) == 0) {
        if (!parse_int(a + 6, ACCEL_MIN, ACCEL_MAX, &v)) {
            link_printf("ERR expected: MOT ACCEL <%d-%d>\r\n", ACCEL_MIN, ACCEL_MAX);
        } else {
            stepper3_set_accel((uint32_t)v);
            link_printf("OK\r\n");
        }
    } else if (strncmp(a, " MODE ", 6) == 0) {
        const char *m = a + 6;
        stepper3_mode_t nm;
        if (strcmp(m, "WAVE") == 0)      nm = STEPPER3_WAVE;
        else if (strcmp(m, "FULL") == 0) nm = STEPPER3_FULL;
        else if (strcmp(m, "HALF") == 0) nm = STEPPER3_HALF;
        else { link_printf("ERR expected: MOT MODE WAVE|FULL|HALF\r\n"); return true; }
        stepper3_set_mode(nm);
        link_printf("OK pos %ld\r\n", (long)position);
    } else if (strncmp(a, " HOLD", 5) == 0) {
        if (!parse_int(a + 5, 0, 1, &v)) {
            link_printf("ERR expected: MOT HOLD 0|1\r\n");
        } else {
            stepper3_set_hold(v);
            link_printf("OK\r\n");
        }
    } else if (strncmp(a, " DIR", 4) == 0) {
        if (!parse_int(a + 4, 0, 1, &v)) {
            link_printf("ERR expected: MOT DIR 0|1\r\n");
        } else {
            stepper3_set_reverse(v);
            link_printf("OK\r\n");
        }
    } else if (strncmp(a, " ORDER ", 7) == 0) {
        const char *o = a + 7;
        uint8_t n[4], seen = 0;
        bool ok = strlen(o) == 4;
        for (int i = 0; ok && i < 4; i++) {
            ok = o[i] >= '0' && o[i] <= '3' && !(seen & (1u << (o[i] - '0')));
            if (ok) {
                n[i] = (uint8_t)(o[i] - '0');
                seen |= 1u << n[i];
            }
        }
        if (!ok) {
            link_printf("ERR expected: MOT ORDER <abcd>, digits 0-3 once each\r\n");
        } else {
            stepper3_set_order(n);
            link_printf("OK\r\n");
        }
    } else {
        link_printf("ERR unknown MOT command\r\n");
    }
    return true;
}
