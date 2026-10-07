#include <stddef.h>
#include <string.h>

#include "lvgl.h"        // lv_snprintf
#include "link.h"
#include "extflash.h"
#include "cal.h"

/* --------------------------------------------------------------------------
 * Stored record. Little-endian, fixed layout, CRC32 (IEEE) over everything
 * before the crc field. Lives in the last 4 KB sector of the serial flash.
 * ------------------------------------------------------------------------ */
#define CAL_FLASH_ADDR  0x7FF000UL
#define CAL_MAGIC       0x4C414350UL  // "PCAL"
#define CAL_VERSION     1

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t npts;
    char id[CAL_ID_LEN];
    int32_t r_ppb[CAL_MAX_POINTS];     // ratio, ppb, strictly ascending
    int32_t p_mmbar[CAL_MAX_POINTS];   // pressure, 0.001 mbar
    uint32_t crc;
} cal_rec_t;

// Nominal: 0 mV and 100 mV at 10 V = 0 and 10 mV/V over 0-15 psia.
static const cal_rec_t cal_default = {
    .magic = CAL_MAGIC, .version = CAL_VERSION, .npts = 2,
    .id = "nominal",
    .r_ppb = { 0, 10000000 },
    .p_mmbar = { 0, 1034214 },
};

static cal_rec_t active;
static cal_rec_t staging;
static bool active_is_default = true;
static bool flash_ok;

static uint32_t crc32(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t c = 0xFFFFFFFFUL;
    while (len--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320UL & (0UL - (c & 1)));
    }
    return ~c;
}

static uint32_t rec_crc(const cal_rec_t *r)
{
    return crc32(r, offsetof(cal_rec_t, crc));
}

// NULL if the table is usable, else the reason.
static const char *rec_check(const cal_rec_t *r)
{
    if (r->magic != CAL_MAGIC || r->version != CAL_VERSION)
        return "bad header";
    if (r->npts < 2 || r->npts > CAL_MAX_POINTS)
        return "need 2 to 32 points";
    for (uint32_t i = 1; i < r->npts; i++)
        if (r->r_ppb[i] <= r->r_ppb[i - 1])
            return "ratios must be strictly ascending";
    return NULL;
}

void cal_init(void)
{
    cal_rec_t r;

    active = cal_default;
    active_is_default = true;

    flash_ok = extflash_init();
    if (!flash_ok)
        return;

    extflash_read(CAL_FLASH_ADDR, &r, sizeof r);
    if (rec_check(&r) == NULL && r.crc == rec_crc(&r)) {
        r.id[CAL_ID_LEN - 1] = '\0';
        active = r;
        active_is_default = false;
    }
}

int64_t cal_pressure_mmbar(int64_t r)
{
    const cal_rec_t *c = &active;
    uint32_t i = 0;

    // Segment containing r; the end segments extend past the table.
    while (i + 2 < c->npts && r > c->r_ppb[i + 1])
        i++;

    int64_t r0 = c->r_ppb[i], r1 = c->r_ppb[i + 1];
    int64_t p0 = c->p_mmbar[i], p1 = c->p_mmbar[i + 1];
    return p0 + (r - r0) * (p1 - p0) / (r1 - r0);
}

bool cal_is_default(void)
{
    return active_is_default;
}

const char *cal_id(void)
{
    return active.id;
}

/* --------------------------------------------------------------------------
 * Commands
 * ------------------------------------------------------------------------ */

// Parses a plain decimal ("-12.345") into an integer scaled by 10^dec.
// Extra decimals are truncated. Advances *s past the number.
static bool parse_fixed(const char **s, int dec, int64_t *out)
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

static void print_fixed(char *buf, size_t n, int64_t v, int dec)
{
    int64_t scale = 1;
    for (int k = 0; k < dec; k++) scale *= 10;
    int64_t a = v < 0 ? -v : v;
    char frac[12];
    int64_t f = a % scale;
    // Zero-padded fraction without relying on "%0*ld" support in lv_snprintf.
    for (int k = dec - 1; k >= 0; k--) { frac[k] = (char)('0' + f % 10); f /= 10; }
    frac[dec] = '\0';
    lv_snprintf(buf, n, "%s%ld.%s", v < 0 ? "-" : "", (long)(a / scale), frac);
}

static void show(void)
{
    char r[20], p[20];
    link_printf("CAL source=%s id=%s points=%lu flash=%s\r\n",
                      active_is_default ? "default" : "flash", active.id,
                      (unsigned long)active.npts, flash_ok ? "ok" : "missing");
    for (uint32_t i = 0; i < active.npts; i++) {
        print_fixed(r, sizeof r, active.r_ppb[i], 6);
        print_fixed(p, sizeof p, active.p_mmbar[i], 3);
        link_printf("CAL PT %s %s\r\n", r, p);
    }
    link_printf("OK\r\n");
}

static void save(void)
{
    cal_rec_t back;

    if (!flash_ok) { link_printf("ERR serial flash not found\r\n"); return; }
    if (active_is_default) { link_printf("ERR nothing applied (CAL NEW, PT, APPLY first)\r\n"); return; }
    active.crc = rec_crc(&active);
    if (!extflash_erase_4k(CAL_FLASH_ADDR) ||
        !extflash_write(CAL_FLASH_ADDR, &active, sizeof active)) {
        link_printf("ERR flash write timed out\r\n");
        return;
    }
    extflash_read(CAL_FLASH_ADDR, &back, sizeof back);
    if (memcmp(&back, &active, sizeof back) != 0) {
        link_printf("ERR flash verify failed\r\n");
        return;
    }
    active_is_default = false;
    link_printf("OK saved %s\r\n", active.id);
}

static void erase(void)
{
    if (!flash_ok) { link_printf("ERR serial flash not found\r\n"); return; }
    if (!extflash_erase_4k(CAL_FLASH_ADDR)) { link_printf("ERR erase timed out\r\n"); return; }
    active = cal_default;
    active_is_default = true;
    link_printf("OK erased, using nominal\r\n");
}

bool cal_command(const char *line)
{
    if (strncmp(line, "CAL", 3) != 0)
        return false;
    const char *a = line + 3;

    if (strcmp(a, "?") == 0) {
        show();
    } else if (strncmp(a, " NEW", 4) == 0) {
        const char *id = a + 4;
        while (*id == ' ') id++;
        memset(&staging, 0, sizeof staging);
        staging.magic = CAL_MAGIC;
        staging.version = CAL_VERSION;
        lv_strlcpy(staging.id, *id ? id : "unnamed", CAL_ID_LEN);
        link_printf("OK new %s\r\n", staging.id);
    } else if (strncmp(a, " PT ", 4) == 0) {
        const char *s = a + 4;
        int64_t r, p;
        if (staging.magic != CAL_MAGIC) { link_printf("ERR CAL NEW first\r\n"); return true; }
        if (staging.npts >= CAL_MAX_POINTS) { link_printf("ERR table full (32)\r\n"); return true; }
        if (!parse_fixed(&s, 6, &r) || !parse_fixed(&s, 3, &p) ||
            r < -50000000 || r > 50000000 || p < -10000000 || p > 100000000) {
            link_printf("ERR expected: CAL PT <mV/V> <mbar>\r\n");
            return true;
        }
        staging.r_ppb[staging.npts] = (int32_t)r;
        staging.p_mmbar[staging.npts] = (int32_t)p;
        staging.npts++;
        link_printf("OK pt %lu\r\n", (unsigned long)staging.npts);
    } else if (strcmp(a, " APPLY") == 0) {
        const char *why = rec_check(&staging);
        if (why) { link_printf("ERR %s\r\n", why); return true; }
        active = staging;
        active_is_default = false;
        link_printf("OK applied %s (not saved)\r\n", active.id);
    } else if (strcmp(a, " SAVE") == 0) {
        save();
    } else if (strcmp(a, " ERASE") == 0) {
        erase();
    } else {
        link_printf("ERR unknown CAL command\r\n");
    }
    return true;
}
