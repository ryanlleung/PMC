#include <stdarg.h>
#include <string.h>

#include "lvgl.h"   // lv_vsnprintf
#include "pmc_config.h"
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
    return false;
}


static uint32_t n_in, n_out, last_ms;
static bool counted;

void link_count(bool out)
{
    if (out)
        n_out++;
    else
        n_in++;
    last_ms = lv_tick_get();
    counted = true;
}

void link_traffic(uint32_t *in, uint32_t *out)
{
    *in = n_in;
    *out = n_out;
}

uint32_t link_quiet_ms(void)
{
    return counted ? lv_tick_elaps(last_ms) : UINT32_MAX;
}

void link_write(const char *s)
{
    link_count(true);
    link_send(s);
}

#if PMC_LINK_ETHERNET

void link_init(void)                         { ethlink_init(); }
void link_task(void)                         { ethlink_task(); }
bool link_getline(char *buf, size_t n)       { return ethlink_getline(buf, n); }
size_t link_tx_room(void)                    { return ethlink_tx_room(); }
bool link_client(void)                       { return ethlink_client_connected() || ethlink_modbus_clients() > 0; }
void link_send(const char *s)                { ethlink_printf("%s", s); }

#endif
