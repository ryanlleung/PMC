#include <string.h>

#include "mcu.h"
#include "lvgl.h"   // lv_snprintf
#include "pmc_config.h"
#include "link.h"
#include "sysinfo.h"

static const char *reason = "unknown";
static bool abnormal;
static char line[96];

void sysinfo_init(void)
{
    uint32_t csr = RCC->CSR;

    // A power-on also sets the pin and brown-out flags, so check the most
    // specific causes first. mikroBootloader runs before the application;
    // if it ever clears these flags the result is "unknown".
    if (csr & RCC_CSR_IWDGRSTF)       { reason = "watchdog"; abnormal = true; }
    else if (csr & RCC_CSR_WWDGRSTF)  { reason = "window watchdog"; abnormal = true; }
    else if (csr & RCC_CSR_LPWRRSTF)  { reason = "low-power"; abnormal = true; }
    else if (csr & RCC_CSR_SFTRSTF)   reason = "software";
    else if (csr & RCC_CSR_PORRSTF)   reason = "power-on";
    else if (csr & RCC_CSR_BORRSTF)   { reason = "brown-out"; abnormal = true; }
    else if (csr & RCC_CSR_PINRSTF)   reason = "reset pin";

    RCC->CSR |= RCC_CSR_RMVF;

    lv_snprintf(line, sizeof line, "PMC firmware %s, last reset: %s",
                sysinfo_version(), reason);
}

const char *sysinfo_version(void)
{
    return PMC_FW_VERSION " (" __DATE__ " " __TIME__ ")";
}

const char *sysinfo_reset_reason(void) { return reason; }
bool sysinfo_reset_abnormal(void)      { return abnormal; }
const char *sysinfo_line(void)         { return line; }

bool sysinfo_command(const char *cmd)
{
    if (strcmp(cmd, "VER?") != 0)
        return false;
    link_printf("%s\r\nOK\r\n", line);
    return true;
}
