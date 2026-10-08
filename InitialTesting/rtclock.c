#include <string.h>

#include "mcu.h"
#include "lvgl.h"   // lv_snprintf
#include "link.h"
#include "rtclock.h"

#define BDCR_RTCSEL_LSE  (1UL << 8)

static bool lse_ok;      // LSE running and selected as the RTC clock
static char text[32];

void rtclock_init(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    (void)RCC->APB1ENR;
    PWR->CR |= PWR_CR_DBP;               // backup domain writable

    uint32_t bdcr = RCC->BDCR;
    if ((bdcr & RCC_BDCR_RTCEN) && (bdcr & RCC_BDCR_RTCSEL) == BDCR_RTCSEL_LSE &&
        (bdcr & RCC_BDCR_LSERDY)) {
        lse_ok = true;                   // kept running through the reset
        return;
    }
    // RTC clock source can only change after a backup domain reset, which
    // also clears the time; only done when it is not already the LSE.
    if ((bdcr & RCC_BDCR_RTCSEL) != 0 && (bdcr & RCC_BDCR_RTCSEL) != BDCR_RTCSEL_LSE) {
        RCC->BDCR |= RCC_BDCR_BDRST;
        RCC->BDCR &= ~RCC_BDCR_BDRST;
    }
    RCC->BDCR |= RCC_BDCR_LSEON;         // takes up to ~2 s; rtclock_poll finishes
}

void rtclock_poll(void)
{
    if (lse_ok || !(RCC->BDCR & RCC_BDCR_LSERDY))
        return;
    RCC->BDCR = (RCC->BDCR & ~RCC_BDCR_RTCSEL) | BDCR_RTCSEL_LSE;
    RCC->BDCR |= RCC_BDCR_RTCEN;
    lse_ok = true;
}

bool rtclock_valid(void)
{
    // INITS: the calendar has been initialised (year is not 0). RSF: the
    // shadow registers have synced since reset, so TR/DR read correctly.
    return lse_ok && (RTC->ISR & RTC_ISR_INITS) && (RTC->ISR & RTC_ISR_RSF);
}

static int bcd2(uint32_t v) { return (int)((v >> 4) & 0xF) * 10 + (int)(v & 0xF); }
static uint32_t to_bcd(int v) { return (uint32_t)(((v / 10) << 4) | (v % 10)); }

// Reads the shadow registers; TR first locks DR until DR is read.
static void read_now(int *y, int *mo, int *d, int *h, int *mi, int *s)
{
    uint32_t tr = RTC->TR;
    uint32_t dr = RTC->DR;
    *h  = bcd2(tr >> 16 & 0x3F);
    *mi = bcd2(tr >> 8 & 0x7F);
    *s  = bcd2(tr & 0x7F);
    *y  = 2000 + bcd2(dr >> 16 & 0xFF);
    *mo = bcd2(dr >> 8 & 0x1F);
    *d  = bcd2(dr & 0x3F);
}

const char *rtclock_screen_text(void)
{
    static const char *mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    int y, mo, d, h, mi, s;
    if (!rtclock_valid())
        return "time not set";
    read_now(&y, &mo, &d, &h, &mi, &s);
    if (mo < 1 || mo > 12) mo = 1;
    lv_snprintf(text, sizeof text, "%02d:%02d  %02d %s", h, mi, d, mon[mo - 1]);
    return text;
}

const char *rtclock_iso(void)
{
    int y, mo, d, h, mi, s;
    if (!rtclock_valid())
        return "";
    read_now(&y, &mo, &d, &h, &mi, &s);
    lv_snprintf(text, sizeof text, "%04d-%02d-%02d %02d:%02d:%02d", y, mo, d, h, mi, s);
    return text;
}

// Day of the week, 1 = Monday (RTC WDU), Sakamoto's method.
static int weekday(int y, int m, int d)
{
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y--;
    int w = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;   // 0 = Sunday
    return w == 0 ? 7 : w;
}

static bool set_time(int y, int mo, int d, int h, int mi, int s)
{
    RTC->WPR = 0xCA;
    RTC->WPR = 0x53;
    RTC->ISR |= RTC_ISR_INIT;
    for (uint32_t n = 0; !(RTC->ISR & RTC_ISR_INITF); n++)
        if (n > 1000000) { RTC->WPR = 0xFF; return false; }
    // 32768 / (127 + 1) / (255 + 1) = 1 Hz; synchronous first.
    RTC->PRER = 255;
    RTC->PRER = (127UL << 16) | 255;
    RTC->CR &= ~RTC_CR_FMT;              // 24 h
    RTC->TR = to_bcd(h) << 16 | to_bcd(mi) << 8 | to_bcd(s);
    RTC->DR = to_bcd(y - 2000) << 16 | (uint32_t)weekday(y, mo, d) << 13 |
              to_bcd(mo) << 8 | to_bcd(d);
    RTC->ISR &= ~RTC_ISR_INIT;
    RTC->WPR = 0xFF;
    return true;
}

bool rtclock_command(const char *line)
{
    int y, mo, d, h, mi, s;

    if (strcmp(line, "TIME?") == 0) {
        if (rtclock_valid())
            link_printf("TIME %s\r\nOK\r\n", rtclock_iso());
        else
            link_printf("TIME not set%s\r\nOK\r\n", lse_ok ? "" : " (32 kHz crystal not running yet)");
        return true;
    }
    if (strncmp(line, "TIME", 4) != 0)
        return false;
    if (strncmp(line, "TIME SET ", 9) != 0) {
        link_printf("ERR expected: TIME? or TIME SET YYYY-MM-DD HH:MM:SS\r\n");
        return true;
    }

    // Fixed layout: YYYY-MM-DD HH:MM:SS
    const char *p = line + 9;
    if (strlen(p) != 19 || p[4] != '-' || p[7] != '-' || p[10] != ' ' || p[13] != ':' || p[16] != ':') {
        link_printf("ERR expected: TIME SET YYYY-MM-DD HH:MM:SS\r\n");
        return true;
    }
    for (int i = 0; i < 19; i++)
        if (i != 4 && i != 7 && i != 10 && i != 13 && i != 16 && (p[i] < '0' || p[i] > '9')) {
            link_printf("ERR expected: TIME SET YYYY-MM-DD HH:MM:SS\r\n");
            return true;
        }
#define NUM2(i) ((p[i] - '0') * 10 + (p[(i) + 1] - '0'))
    y = NUM2(0) * 100 + NUM2(2); mo = NUM2(5); d = NUM2(8);
    h = NUM2(11); mi = NUM2(14); s = NUM2(17);
#undef NUM2
    if (y < 2000 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 59) {
        link_printf("ERR date or time out of range\r\n");
        return true;
    }
    if (!lse_ok) {
        link_printf("ERR 32 kHz crystal not running\r\n");
        return true;
    }
    if (!set_time(y, mo, d, h, mi, s)) {
        link_printf("ERR RTC did not enter init mode\r\n");
        return true;
    }
    link_printf("OK time set\r\n");
    return true;
}
