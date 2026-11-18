#include <string.h>

#include "mcu.h"
#include "lvgl.h"   // lv_snprintf
#include "sysinfo.h"

static const char *reason = "unknown";

void sysinfo_init(void)
{
    uint32_t csr = RCC->CSR;

    // A power-on also sets the pin and brown-out flags, so check the most
    // specific causes first. mikroBootloader runs before the application;
    // if it ever clears these flags the result is "unknown".
    if (csr & RCC_CSR_IWDGRSTF)       reason = "watchdog";
    else if (csr & RCC_CSR_WWDGRSTF)  reason = "window watchdog";
    else if (csr & RCC_CSR_LPWRRSTF)  reason = "low-power";
    else if (csr & RCC_CSR_SFTRSTF)   reason = "software";
    else if (csr & RCC_CSR_PORRSTF)   reason = "power-on";
    else if (csr & RCC_CSR_BORRSTF)   reason = "brown-out";
    else if (csr & RCC_CSR_PINRSTF)   reason = "reset pin";

    RCC->CSR |= RCC_CSR_RMVF;
}

const char *sysinfo_reset_reason(void) { return reason; }

void sysinfo_restart(void)
{
#ifndef __wasm__
    // SCB->AIRCR: VECTKEY 0x05FA and SYSRESETREQ (the CMSIS NVIC helpers
    // are not linked in the mikroSDK build).
    __asm volatile ("dsb" ::: "memory");
    *(volatile uint32_t *)0xE000ED0CUL = (0x05FAUL << 16) | (1UL << 2);
    __asm volatile ("dsb" ::: "memory");
    for (;;)
        ;
#endif
}

// The STM32's 96-bit unique ID as 24 hex digits: the serial in *IDN?
// (remote.c).
const char *sysinfo_chip_id(void)
{
    static char id[25];
    if (id[0])
        return id;
#ifdef __wasm__
    static const uint8_t uid[12] = { 0 };   // the simulator: no chip ID to read
#else
    const volatile uint8_t *uid = (const volatile uint8_t *)0x1FFF7A10UL;
#endif
    for (int i = 0; i < 12; i++)
        lv_snprintf(id + 2 * i, 3, "%02X", uid[i]);
    return id;
}

