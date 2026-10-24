#include <stdlib.h>
#include <string.h>

#include "mcu.h"
#include "board.h"
#include "drv_digital_out.h"
#include "lvgl.h"   // lv_snprintf
#include "link.h"
#include "pmc_config.h"
#include "limit.h"
#include "supply.h"
#include "stepper3.h"

/* --------------------------------------------------------------------------
 * Hardware
 *
 * Stepper 3 in socket S1: AN PA4, RST PC2, CS PB12, PWM PD12.
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

static const out_pin_t out_pins[4] = {
    { GPIOA, 4 },    // MIKROBUS_1_AN  -> ULN2003 IN1
    { GPIOC, 2 },    // MIKROBUS_1_RST -> IN2
    { GPIOB, 12 },   // MIKROBUS_1_CS  -> IN3
    { GPIOD, 12 },   // MIKROBUS_1_PWM -> IN4
};
#define SOCKET_PINS { MIKROBUS_1_AN, MIKROBUS_1_RST, MIKROBUS_1_CS, MIKROBUS_1_PWM }

static digital_out_t sdk_pins[4];

// Half-step sequence, bit n = coil n on. Even entries one coil (wave),
// odd entries two coils (full step).
static const uint8_t seq[8] = { 0x1, 0x3, 0x2, 0x6, 0x4, 0xC, 0x8, 0x9 };

#define TICK_HZ        1000000UL
#define SETTLE_US      20000UL    // coils stay on this long after the last step
#define RATE_MIN       STEPPER3_RATE_MIN
#define RATE_MAX       STEPPER3_RATE_MAX
#define ACCEL_MIN      STEPPER3_ACCEL_MIN
#define ACCEL_MAX      STEPPER3_ACCEL_MAX

/* --------------------------------------------------------------------------
 * Settings (CFG SAVE keeps them). Defaults from the 26M048B2U bench test, 8 Oct 2026
 * (thread "26M motor torque-speed settings", /mnt/project-files/motor/):
 * clean at RUN 400 and START 400 with no ramp, steps missed at RUN 600,
 * so RUN 400 is about 2/3 of the measured limit.
 * ------------------------------------------------------------------------ */
static uint32_t rate_run   = 400;    // steps/s
static uint32_t rate_start = 200;    // steps/s, ramp starts and ends here
static uint32_t accel      = 3200;   // steps/s^2
static stepper3_mode_t mode = STEPPER3_FULL;
static bool hold;                    // coils on when idle
static bool reverse;                // flips the sequence direction. Normal (0) is the direction that was
                                    // "Reversed" before 0.11.19, the factory default (Ryan, 22 Oct)
static uint8_t order[4] = { 0, 2, 1, 3 };   // coil n -> output order[n]; 0213 found on the bench (8 Oct, 26M motor)

/* --------------------------------------------------------------------------
 * Motion state (written by the ISR while moving)
 * ------------------------------------------------------------------------ */
typedef enum { ST_IDLE, ST_MOVING, ST_SETTLE } run_state_t;

static volatile run_state_t run_state = ST_IDLE;
static volatile int32_t position;       // steps of the current mode
static volatile int32_t target;         // where the running move ends
static volatile bool stop_ramp;         // ramp down to the start rate, then stop
static volatile stepper3_stop_t last_stop = STEPPER3_STOP_NONE;
static volatile int8_t dir;             // +1 open, -1 close
static uint8_t phase;                   // index into seq[]
static bool energised;
static uint32_t v_fp;                   // current rate, steps/s * 256
static volatile uint32_t v_run_fp, v_start_fp;
static uint32_t n_acc;                  // steps spent accelerating
static uint32_t move_rate;              // run rate of this move, steps/s
static uint32_t deadline_ms;            // F-05: lv_tick_get() past this = timeout
static volatile uint32_t moves_ended;   // counts moves ended, for stepper3_move_seq()
static int32_t move_from;                // last move: start position and times
static uint32_t move_t0;
static volatile uint32_t move_t1;       // 0 while it runs
static void (*ref_cb)(const char *why);   // stepper3_on_reference_change
static bool (*move_cb)(const char **why);  // stepper3_on_move_start
static const char *refusal = "";
static const char *lock_why;            // MOT and screen moves refused while set

static char status_text[160];

/* --------------------------------------------------------------------------
 * Limit switch (one NC dry contact at the closed end of travel), read by
 * limit.c. Checked before a closing move starts and again before every
 * closing step (in the step interrupt). Open, unknown or a broken wire all
 * stop. Opening moves are never blocked, so the valve can always back off.
 * PMC_LIMIT_STOP 0 turns the stop off (state displayed only).
 * ------------------------------------------------------------------------ */
#define LIMIT_FITTED PMC_LIMIT_STOP

bool stepper3_limit_fitted(void) { return LIMIT_FITTED; }

/*
 * Override: MOT OVERRIDE (or the screen, later) lets the NEXT move close
 * past an open limit, for a switch that trips early (ice, a forced
 * actuator). One move only: it is used up by the next move in either
 * direction and ends when that move stops. RAM only, off at reset.
 */
static volatile bool override_armed, override_active;

bool stepper3_override(void) { return override_armed || override_active; }

void stepper3_override_arm(void)
{
    override_armed = true;
    link_printf("MOT override armed: the next closing move ignores the limit switch\r\n");
}

// True when a closing step must not be taken now.
bool stepper3_limit_open(void)
{
    return LIMIT_FITTED && limit_open_now() && !override_armed && !override_active;
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
    override_active = false;
    stop_ramp = false;
    last_stop = why;
    moves_ended++;
    move_t1 = lv_tick_get() | 1;
    run_state = ST_SETTLE;
    // Restart the count for the settle: with ARPE an ARR write alone only
    // takes effect after the current step period, so the coils dropped one
    // step after the last (0.11.38). URS keeps the UG from raising another
    // update interrupt.
    TIM7->CR1 = TIM_CR1_URS;
    TIM7->ARR = SETTLE_US - 1;
    TIM7->EGR = TIM_EGR_UG;
    TIM7->SR = 0;
    TIM7->CR1 = TIM_CR1_URS | TIM_CR1_ARPE | TIM_CR1_CEN;
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
    if (reverse == (dir < 0))
        inc = -inc;
    phase = (uint8_t)(phase + inc) & 7;
    write_coils(seq[phase]);
    position += dir;

    // Steps still to go this way; 0 = on the target, negative = past it
    // (the target was moved behind, or closer than the ramp-down needs).
    int32_t ahead = (target - position) * dir;

    // On the target and (all but) back at the start rate: done. Arriving
    // faster, it overshoots and comes back.
    if (ahead == 0 && n_acc <= 1 && !stop_ramp) {
        finish(STEPPER3_STOP_DONE);
        return;
    }
    // Back at the start rate: stop, or turn round for a target behind.
    // After a turn to closing, the limit is checked before the next step.
    if (n_acc == 0) {
        if (stop_ramp) {
            finish(STEPPER3_STOP_CMD);
            return;
        }
        if (ahead < 0) {
            dir = (int8_t)-dir;
            ahead = -ahead;
        }
    }

    // Linear ramp: dv = a * dt = a / v per step. Slow down once the steps
    // left are no more than the steps it took to speed up, when the target
    // is behind, for a ramped stop, or after the rate was lowered.
    uint32_t dv = (uint32_t)(((uint64_t)accel << 16) / v_fp);
    if (stop_ramp || ahead <= 0 || (uint32_t)ahead <= n_acc || v_fp > v_run_fp) {
        if (n_acc > 0)
            n_acc--;
        v_fp = (n_acc == 0 || v_fp <= v_start_fp + dv) ? v_start_fp : v_fp - dv;
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

// Holds off every interrupt (PRIMASK) while a running move is changed, so
// a step already pending cannot run on half-written values. A step that
// falls due meanwhile is taken as soon as it is released.
static uint32_t step_irq_hold(void)
{
    uint32_t pm;
    __asm volatile ("mrs %0, primask\n\tcpsid i" : "=r" (pm) :: "memory");
    return pm;
}
static void step_irq_release(uint32_t pm) { __asm volatile ("msr primask, %0" :: "r" (pm) : "memory"); }

static int32_t abs32(int32_t v) { return v < 0 ? -v : v; }

// F-05: the move should be done within |distance| / rate, plus the ramps
// (counted twice over, for a turn round), plus 2 s.
static void set_deadline(void)
{
    uint32_t steps = (uint32_t)abs32(target - position) + 2 * n_acc;
    uint32_t start = v_start_fp >> 8;
    uint32_t ramp_ms = (uint32_t)((uint64_t)(move_rate - start) * 1000 / accel);
    deadline_ms = lv_tick_get() + (uint32_t)((uint64_t)steps * 1000 / move_rate)
                + 4 * ramp_ms + STEPPER3_TIMEOUT_MARGIN_MS;
}

bool stepper3_goto_rate(int32_t to, uint32_t sps)
{
    if (sps == 0)
        sps = rate_run;
    refusal = "rate out of range";
    if (sps < RATE_MIN || sps > RATE_MAX)
        return false;
    int32_t dist = to - position;
    refusal = "target too far";
    if (dist > STEPPER3_MAX_MOVE || dist < -STEPPER3_MAX_MOVE)
        return false;

    if (run_state == ST_MOVING) {
        // Retarget: the interrupt carries on, turning round if needed.
        uint32_t pm = step_irq_hold();
        if (run_state != ST_MOVING) {
            // The move ended just now; start a new one below.
            step_irq_release(pm);
            goto from_rest;
        }
        target = to;
        move_rate = sps;
        v_run_fp = sps << 8;
        v_start_fp = (rate_start < sps ? rate_start : sps) << 8;
        stop_ramp = false;
        set_deadline();
        step_irq_release(pm);
        return true;
    }
from_rest:
    if (run_state == ST_SETTLE) {
        // Coils still on from the last move: start from here.
        timer_stop();
        run_state = ST_IDLE;
    }

    if (dist == 0) {
        last_stop = STEPPER3_STOP_DONE;
        if (!hold && energised)
            coils_off();
        return true;
    }
    if (dist < 0 && stepper3_limit_open()) {
        last_stop = STEPPER3_STOP_LIMIT;
        refusal = "limit open, closing move refused";
        return false;
    }
    // No motor supply: the count would run on with the valve still.
    if (supply_off()) {
        refusal = "Motor supply off";
        return false;
    }
    // The saved position is withdrawn before anything moves; if it cannot
    // be, nothing moves (a restart would offer a stale count).
    const char *why = NULL;
    if (move_cb && !move_cb(&why)) {
        if (!hold && energised)
            coils_off();
        refusal = why ? why : "refused";
        return false;
    }

    // An armed override is used up by this move, whichever way it goes.
    if (override_armed) {
        override_armed = false;
        override_active = dist < 0;
        if (override_active && LIMIT_FITTED && limit_open_now())
            link_printf("MOT override: closing %ld steps with the limit switch open\r\n", (long)-dist);
    }

    // Wave uses the one-coil entries, full the two-coil entries.
    if (mode == STEPPER3_WAVE)
        phase &= ~1u;
    else if (mode == STEPPER3_FULL)
        phase |= 1u;
    write_coils(seq[phase]);
    energised = true;

    dir = dist > 0 ? 1 : -1;
    target = to;
    move_rate = sps;
    move_from = position;
    move_t0 = lv_tick_get();
    move_t1 = 0;
    v_start_fp = (rate_start < sps ? rate_start : sps) << 8;
    v_run_fp = sps << 8;
    v_fp = v_start_fp;
    n_acc = 0;
    stop_ramp = false;
    last_stop = STEPPER3_STOP_NONE;
    set_deadline();
    run_state = ST_MOVING;
    // First step one start-rate period after the coils are energised.
    timer_start(period_us(v_fp));
    return true;
}

bool stepper3_goto(int32_t to)
{
    return stepper3_goto_rate(to, 0);
}

bool stepper3_move(int32_t steps)
{
    if (steps == 0 || steps > STEPPER3_MAX_MOVE || steps < -STEPPER3_MAX_MOVE)
        return false;
    // During a move, relative to where that move ends.
    return stepper3_goto((run_state == ST_MOVING ? target : position) + steps);
}

void stepper3_stop_ramped(void)
{
    if (run_state == ST_MOVING)
        stop_ramp = true;
}

void stepper3_poll(void)
{
    if (run_state != ST_MOVING || (int32_t)(lv_tick_get() - deadline_ms) <= 0)
        return;
    stepper3_stop();
    last_stop = STEPPER3_STOP_TIMEOUT;
    link_printf("F-05 motion timeout: stopped at %ld, target %ld\r\n",
                (long)position, (long)target);
}

int32_t stepper3_target(void) { return run_state == ST_MOVING ? target : position; }
uint32_t stepper3_move_seq(void) { return moves_ended; }

// Allowed while the coils settle after a move: calibration sets 0 % the
// moment the switch stops the creep, inside that 20 ms.
bool stepper3_set_position(int32_t p)
{
    if (run_state == ST_MOVING)
        return false;
    position = p;
    return true;
}

void stepper3_lock(const char *why) { lock_why = why; }
const char *stepper3_locked(void)   { return lock_why; }

void stepper3_stop(void)
{
    timer_stop();
    override_active = false;
    stop_ramp = false;
    if (run_state == ST_MOVING) {
        last_stop = STEPPER3_STOP_CMD;
        moves_ended++;
        move_t1 = lv_tick_get() | 1;
    }
    run_state = ST_IDLE;
    if (!hold)
        coils_off();
}

// Disarms the override. A closing move already using it is stopped at once
// (it would otherwise go on past the open switch); returns true then.
bool stepper3_override_cancel(void)
{
    override_armed = false;
    if (!override_active)
        return false;
    stepper3_stop();
    link_printf("MOT override cancelled: stopped at %ld\r\n", (long)position);
    return true;
}

void stepper3_off(void)
{
    stepper3_stop();
    coils_off();
}

bool stepper3_busy(void)               { return run_state != ST_IDLE; }
int32_t stepper3_position(void)        { return position; }
stepper3_stop_t stepper3_last_stop(void) { return last_stop; }

void stepper3_on_reference_change(void (*cb)(const char *why)) { ref_cb = cb; }
void stepper3_on_move_start(bool (*cb)(const char **why))     { move_cb = cb; }
const char *stepper3_refusal(void)                             { return refusal; }

static void ref_changed(const char *why)
{
    if (ref_cb)
        ref_cb(why);
}

void stepper3_zero(void)
{
    if (run_state != ST_IDLE)
        return;
    if (position != 0)
        ref_changed("motor count zeroed");
    position = 0;
}

const char *stepper3_status(void)
{
    const char *st = run_state == ST_MOVING ? (dir > 0 ? "opening" : "closing")
                   : energised ? "idle, coils on" : "idle, coils off";
    char to[24] = "";
    if (run_state == ST_MOVING)
        lv_snprintf(to, sizeof to, " to %ld", (long)target);
    lv_snprintf(status_text, sizeof status_text,
                "S1 Stepper 3: %s, pos %ld%s, %s %lu sps, limit %s",
                st, (long)position, to, stepper3_mode_name(mode),
                (unsigned long)(run_state == ST_MOVING ? move_rate : rate_run),
                !LIMIT_FITTED ? "stop off (PMC_LIMIT_STOP 0)"
                : stepper3_override() ? (limit_open_now() ? "OPEN, override" : "ok, override armed")
                : stepper3_limit_open() ? "OPEN" : "ok");
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

int32_t stepper3_scale(void) { return mode == STEPPER3_HALF ? 2 : 1; }

bool stepper3_set_mode(stepper3_mode_t nm)
{
    // Refused in AUTO and while homing (lock_why): the controller and the
    // homing hold positions in the old step size.
    if (run_state != ST_IDLE || nm > STEPPER3_HALF || lock_why)
        return false;
    // Keep the position in the new step size; the homing stays (since
    // 0.11.33, Ryan 23 Oct), the valve settings being in full steps. HALF to
    // FULL or WAVE drops an odd half step, and WAVE and FULL rest half a
    // step apart: at most one half step, against 16512 full steps a turn.
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
    if (on != reverse)
        ref_changed("motor direction changed");
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
    if (memcmp(order, o, sizeof order) != 0)
        ref_changed("coil order changed");
    memcpy(order, o, sizeof order);
    if (energised)
        coils_off();
    return true;
}

bool stepper3_last_move(int32_t *from, uint32_t *ms)
{
    if (!move_t0)
        return false;
    *from = move_from;
    uint32_t t1 = move_t1;
    *ms = (t1 ? t1 : lv_tick_get()) - move_t0;
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
    case STEPPER3_STOP_TIMEOUT: return "F-05 timeout";
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

// MOT GOTO <position> [rate]: absolute, also while a move runs.
static void goto_command(const char *s)
{
    char *end;
    while (*s == ' ')
        s++;
    long to = strtol(s, &end, 10);
    long sps = 0;
    bool ok = end != s;
    s = end;
    while (*s == ' ')
        s++;
    if (ok && *s) {
        sps = strtol(s, &end, 10);
        ok = end != s && sps >= RATE_MIN && sps <= RATE_MAX;
        s = end;
        while (*s == ' ')
            s++;
        ok = ok && !*s;
    }
    if (!ok || to < -10 * STEPPER3_MAX_MOVE || to > 10 * STEPPER3_MAX_MOVE) {
        link_printf("ERR expected: MOT GOTO <position> [rate %d-%d]\r\n", RATE_MIN, RATE_MAX);
        return;
    }
    bool moving = run_state == ST_MOVING;
    if (!stepper3_goto_rate((int32_t)to, (uint32_t)sps)) {
        if (to - position > STEPPER3_MAX_MOVE || position - to > STEPPER3_MAX_MOVE)
            link_printf("ERR more than %d steps from %ld\r\n", STEPPER3_MAX_MOVE, (long)position);
        else
            link_printf("ERR %s\r\n", stepper3_refusal());
        return;
    }
    if (moving)
        link_printf("OK new target %ld, at %ld\r\n", to, (long)position);
    else if (to == position)
        link_printf("OK already at %ld\r\n", to);
    else
        link_printf("OK moving to %ld from %ld at %lu sps\r\n", to, (long)position, (unsigned long)move_rate);
}

static void show(void)
{
    link_printf("MOT state %s, pos %ld, target %ld, last stop %s\r\n",
                run_state == ST_MOVING ? (stop_ramp ? "stopping" : "moving")
                : run_state == ST_SETTLE ? "settling" : "idle",
                (long)position, (long)stepper3_target(), stepper3_stop_name(last_stop));
    link_printf("MOT mode %s, rate %lu sps, start %lu sps, accel %lu sps/s\r\n",
                stepper3_mode_name(mode), (unsigned long)rate_run, (unsigned long)rate_start,
                (unsigned long)accel);
    link_printf("MOT hold %d, dir %d, order %d%d%d%d, coils %s, limit %s\r\n",
                hold, reverse, order[0], order[1], order[2], order[3],
                energised ? "on" : "off",
                !LIMIT_FITTED ? "stop off (PMC_LIMIT_STOP 0)" : stepper3_override() ? "OVERRIDE (next close ignores it)" :
                stepper3_limit_open() ? "OPEN" : "closed (ok)");
    link_printf("OK\r\n");
}

// The MOT commands that change the step timing, sequence or count.
static bool is_setting(const char *a)
{
    static const char *const names[] = { " ZERO", " RATE", " START", " ACCEL", " MODE", " HOLD", " DIR", " ORDER" };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strncmp(a, names[i], strlen(names[i])) == 0)
            return true;
    return false;
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
    } else if (strcmp(a, " STOP RAMP") == 0) {
        if (run_state == ST_MOVING) {
            stepper3_stop_ramped();
            link_printf("OK ramping down from %ld\r\n", (long)position);
        } else {
            link_printf("OK not moving, at %ld\r\n", (long)position);
        }
    } else if (strcmp(a, " OFF") == 0) {
        stepper3_off();
        link_printf("OK coils off at %ld\r\n", (long)position);
    } else if (lock_why && (strncmp(a, " MOVE", 5) == 0 || strncmp(a, " GOTO", 5) == 0 ||
                            strncmp(a, " OVERRIDE", 9) == 0 || strcmp(a, " ZERO") == 0 ||
                            strncmp(a, " MODE ", 6) == 0)) {
        link_printf("ERR %s\r\n", lock_why);
    } else if (strncmp(a, " MOVE", 5) == 0) {
        if (!parse_int(a + 5, -STEPPER3_MAX_MOVE, STEPPER3_MAX_MOVE, &v) || v == 0)
            link_printf("ERR expected: MOT MOVE <steps>, 1 to %d either way\r\n", STEPPER3_MAX_MOVE);
        else if (!stepper3_move((int32_t)v))
            link_printf("ERR %s\r\n", stepper3_refusal());
        else
            link_printf("OK moving to %ld from %ld\r\n", (long)stepper3_target(), (long)position);
    } else if (strncmp(a, " GOTO", 5) == 0) {
        goto_command(a + 5);
    } else if (strcmp(a, " OVERRIDE") == 0) {
        if (!LIMIT_FITTED) {
            link_printf("ERR limit stop is off (PMC_LIMIT_STOP 0), nothing to override\r\n");
        } else if (run_state != ST_IDLE) {
            link_printf("ERR busy, MOT STOP first\r\n");
        } else {
            stepper3_override_arm();
            link_printf("OK\r\n");
        }
    } else if (strcmp(a, " OVERRIDE OFF") == 0) {
        stepper3_override_cancel();
        link_printf("OK override cleared\r\n");
    } else if (run_state != ST_IDLE && is_setting(a)) {
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
