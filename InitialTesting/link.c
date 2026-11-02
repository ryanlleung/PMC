#include <stdarg.h>
#include <string.h>

#include "lvgl.h"   // lv_vsnprintf
#include "pmc_config.h"
#include "usb_serial.h"
#include "ethlink.h"
#include "link.h"

// Spaces and tabs at either end are dropped, so "STOP " is STOP.
static bool line_take(link_line_t *l, char *out, size_t n)
{
    size_t a = 0, b = l->len;
    if (l->over) {
        l->over = false;
        l->len = 0;
        lv_strlcpy(out, LINK_LINE_LONG, n);
        return true;
    }
    while (a < b && (l->text[a] == ' ' || l->text[a] == '\t'))
        a++;
    while (b > a && (l->text[b - 1] == ' ' || l->text[b - 1] == '\t'))
        b--;
    l->text[b] = '\0';
    lv_strlcpy(out, l->text + a, n);
    l->len = 0;
    return b > a;
}

bool link_line_feed(link_line_t *l, char c, char *out, size_t n)
{
    if (c == '\r' || c == '\n')
        return l->len > 0 && line_take(l, out, n);
    if (l->len < sizeof l->text - 1)
        l->text[l->len++] = c;
    else
        l->over = true;
    l->last_ms = lv_tick_get();
    return false;
}

bool link_line_idle(link_line_t *l, uint32_t idle_ms, char *out, size_t n)
{
    return l->len > 0 && lv_tick_elaps(l->last_ms) >= idle_ms && line_take(l, out, n);
}


static void link_send(const char *s);

void link_write(const char *s) { link_send(s); }

#if PMC_LINK_ETHERNET

void link_init(void)                         { ethlink_init(); }
void link_task(void)                         { ethlink_task(); }
bool link_getline(char *buf, size_t n)       { return ethlink_getline(buf, n); }
const char *link_status(void)                { return ethlink_status(); }
bool link_chip_ok(void)                      { return ethlink_link_up() && ethlink_ip()[0]; }

const char *link_chip_text(void)            { return ethlink_chip_text(); }
size_t link_tx_room(void)                    { return ethlink_tx_room(); }

static void link_send(const char *s)        { ethlink_printf("%s", s); }

#else

void link_init(void)                         { usb_serial_init(); }
void link_task(void)                         { usb_serial_task(); }
bool link_getline(char *buf, size_t n)       { return usb_serial_getline(buf, n); }
const char *link_status(void)                { return usb_serial_status(); }
const char *link_chip_text(void)              { return "USB"; }
size_t link_tx_room(void)                    { return usb_serial_tx_room(); }

bool link_chip_ok(void)                      { return !strstr(usb_serial_status(), "not enumerated"); }

static void link_send(const char *s)        { usb_serial_printf("%s", s); }

#endif
