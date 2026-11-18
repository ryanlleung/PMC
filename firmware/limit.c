#include <string.h>

#include "mcu.h"
#include "lvgl.h"   // lv_tick_get
#include "link.h"
#include "limit.h"
#include "ipc_config.h"

/*
 * Valve limit switch: one dry NC contact between PD5 (S1 TX) and GND. On
 * the Adapter Click that is IDC10 pad 8, printed RX: the Adapter names its
 * UART pads from the Click's side, so its RX pad is the socket's TX (PD5)
 * and its TX pad (pad 7) is the socket's RX (PA3). USART2 is never
 * enabled, so PD5 is a plain GPIO here. GND is pad 10. Read with the MCU's
 * internal pull-up. Contact closed pulls the pin low (healthy, valve away
 * from the limit); open (at the limit, or a broken wire) lets it float
 * high. IPC_LIMIT_OPEN_LEVEL flips this for an inverting interface such as
 * an Opto 2 Click.
 *
 * Treated as the closing stop (fail-safe):
 *  - a trip counts at once: the first OPEN sample makes the state OPEN;
 *  - only the release is debounced: CLOSED again needs 20 ms of closed
 *    samples, so a bouncing or intermittent contact stays OPEN;
 *  - until the first steady read after start-up the state is unknown and
 *    treated as OPEN;
 *  - limit_open_now() also reads the pin directly, so the step interrupt
 *    checks the contact itself before every closing step, not only the
 *    main-loop state (which a long screen redraw can delay).
 * With IPC_LIMIT_STOP 1 stepper3.c refuses and stops closing moves while
 * this reports open; with 0 the same state is only displayed.
 */
#define IPC_LIMIT_PORT      GPIOD
#define IPC_LIMIT_PIN       5

#define SAMPLE_MS   5
#define STABLE_N    4          // 20 ms of closed samples before a release counts

static volatile limit_state_t state = LIMIT_UNKNOWN;   // read by the step ISR
static uint8_t same;
static uint32_t changes;
static uint32_t last_sample;

static bool read_open(void)
{
    bool high = (IPC_LIMIT_PORT->IDR >> IPC_LIMIT_PIN) & 1u;
    return high == (IPC_LIMIT_OPEN_LEVEL != 0);
}

void limit_init(void)
{
    if (IPC_LIMIT_PORT == GPIOA)
        RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    else if (IPC_LIMIT_PORT == GPIOD)
        RCC->AHB1ENR |= RCC_AHB1ENR_GPIODEN;
    else if (IPC_LIMIT_PORT == GPIOG)
        RCC->AHB1ENR |= RCC_AHB1ENR_GPIOGEN;
    (void)RCC->AHB1ENR;
    // Input (MODER 00), pull-up (PUPDR 01) or none (00).
    IPC_LIMIT_PORT->MODER &= ~(3UL << (2 * IPC_LIMIT_PIN));
    IPC_LIMIT_PORT->PUPDR = (IPC_LIMIT_PORT->PUPDR & ~(3UL << (2 * IPC_LIMIT_PIN))) |
                            ((IPC_LIMIT_PULLUP ? 1UL : 0UL) << (2 * IPC_LIMIT_PIN));
    same = 0;
    last_sample = lv_tick_get();
}

void limit_poll(void)
{
    if (lv_tick_get() - last_sample < SAMPLE_MS)
        return;
    last_sample = lv_tick_get();

    limit_state_t s;
    if (read_open()) {
        same = 0;                      // trip at once, restart the release count
        s = LIMIT_OPEN;
    } else {
        if (same < STABLE_N)
            same++;
        if (same < STABLE_N)
            return;                    // release not yet steady
        s = LIMIT_CLOSED;
    }
    if (s != state) {
        if (state != LIMIT_UNKNOWN)
            changes++;
        state = s;
    }
}

limit_state_t limit_state(void) { return state; }

bool limit_open_now(void)
{
    return state != LIMIT_CLOSED || read_open();
}

bool limit_pin_open(void) { return read_open(); }

