#include <stdarg.h>
#include <string.h>

#include "lvgl.h"   // lv_vsnprintf
#include "pmc_config.h"
#include "usb_serial.h"
#include "ethlink.h"
#include "link.h"

#if PMC_LINK_ETHERNET

void link_init(void)                         { ethlink_init(); }
void link_task(void)                         { ethlink_task(); }
bool link_getline(char *buf, size_t n)       { return ethlink_getline(buf, n); }
const char *link_status(void)                { return ethlink_status(); }
bool link_chip_ok(void)                      { return ethlink_link_up() && ethlink_ip()[0]; }

const char *link_chip_text(void)            { return ethlink_chip_text(); }
bool link_take_new_client(void)              { return ethlink_take_new_client(); }

void link_printf(const char *fmt, ...)
{
    char buf[256];
    va_list args;
    va_start(args, fmt);
    lv_vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    ethlink_printf("%s", buf);
}

#else

void link_init(void)                         { usb_serial_init(); }
void link_task(void)                         { usb_serial_task(); }
bool link_getline(char *buf, size_t n)       { return usb_serial_getline(buf, n); }
const char *link_status(void)                { return usb_serial_status(); }
// "USB rx <bytes received>": shows on the screen whether commands arrive.
const char *link_chip_text(void)
{
    static char t[24];
    lv_snprintf(t, sizeof t, "USB rx %lu", (unsigned long)usb_serial_rx_bytes());
    return t;
}

bool link_take_new_client(void)
{
    static bool was_open;
    bool open = strstr(usb_serial_status(), "port open") != NULL;
    bool v = open && !was_open;
    was_open = open;
    return v;
}
bool link_chip_ok(void)                      { return !strstr(usb_serial_status(), "not enumerated"); }

void link_printf(const char *fmt, ...)
{
    char buf[256];
    va_list args;
    va_start(args, fmt);
    lv_vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    usb_serial_printf("%s", buf);
}

#endif
