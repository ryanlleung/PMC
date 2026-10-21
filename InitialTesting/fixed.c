#include "lvgl.h"   // lv_snprintf
#include "fixed.h"

const char *fixed_fmt(char *buf, size_t n, int64_t v, int dec)
{
    int64_t scale = 1;
    for (int k = 0; k < dec; k++) scale *= 10;
    int64_t a = v < 0 ? -v : v;
    char frac[12];
    int64_t f = a % scale;
    // Zero-padded fraction without relying on "%0*ld" support in lv_snprintf.
    for (int k = dec - 1; k >= 0; k--) { frac[k] = (char)('0' + f % 10); f /= 10; }
    frac[dec] = '\0';
    lv_snprintf(buf, n, "%s%ld%s%s", v < 0 ? "-" : "", (long)(a / scale), dec ? "." : "", frac);
    return buf;
}

int64_t fixed_round(int64_t v, int drop)
{
    int64_t scale = 1;
    for (int k = 0; k < drop; k++) scale *= 10;
    return (v >= 0 ? v + scale / 2 : v - scale / 2) / scale;
}

uint32_t isqrt64(uint64_t v)
{
    uint64_t r = 0, bit = 1ULL << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return (uint32_t)r;
}

// Parses a plain decimal ("-12.345") into an integer scaled by 10^dec.
// Extra decimals are truncated. Advances *s past the number.
bool fixed_parse(const char **s, int dec, int64_t *out)
{
    const char *p = *s;
    int64_t v = 0;
    int neg = 0, digits = 0, frac = -1;

    while (*p == ' ' || *p == '\t') p++;
    if (*p == '-' || *p == '+') neg = (*p++ == '-');
    for (; *p; p++) {
        if (*p == '.' && frac < 0) { frac = 0; continue; }
        if (*p < '0' || *p > '9') break;
        if (frac >= 0) {
            if (frac >= dec) { digits++; continue; }
            frac++;
        }
        if (v > 100000000000LL) return false;    // 1e11: room for the 10^dec scaling
        v = v * 10 + (*p - '0');
        digits++;
    }
    if (!digits) return false;
    if (*p && *p != ' ' && *p != '\t') return false;   // "1.2.3", "5x"
    for (int k = frac < 0 ? 0 : frac; k < dec; k++) v *= 10;
    *out = neg ? -v : v;
    *s = p;
    return true;
}
