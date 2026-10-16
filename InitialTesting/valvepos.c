#include <stddef.h>
#include <string.h>

#include "lvgl.h"        // lv_tick_get
#include "link.h"
#include "extflash.h"
#include "fixed.h"
#include "stepper3.h"
#include "valve.h"
#include "valvepos.h"

#define VP_BASE      0x7EC000UL
#define VP_SECTORS   16
#define VP_SECTOR    4096UL
#define VP_PER       (VP_SECTOR / sizeof(vp_entry_t))
#define VP_SAFE      0x45464153UL   // "SAFE"
#define VP_UNSAFE    0UL
#define STILL_MS     2000

typedef struct {
    uint32_t seq;        // +1 each entry, never 0xFFFFFFFF
    int32_t count;
    uint32_t state;      // VP_SAFE or VP_UNSAFE
    uint32_t crc;
} vp_entry_t;
_Static_assert(sizeof(vp_entry_t) == 16, "16-byte entries");

static bool ok;                 // flash found
static uint32_t sector, at;  // where the next entry goes
static vp_entry_t last;         // newest good entry (seq 0: none)
static bool offer;
static int32_t boot_count;
static uint32_t still_since;
static bool still;             // standing since still_since
static uint32_t writes;
static bool warned;            // withdraw failure logged

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

static bool good(const vp_entry_t *e)
{
    return e->seq != 0xFFFFFFFFUL && e->seq != 0 && e->crc == crc32(e, offsetof(vp_entry_t, crc));
}

static uint32_t addr(uint32_t s, uint32_t i)
{
    return VP_BASE + s * VP_SECTOR + i * sizeof(vp_entry_t);
}

// Writes one entry and reads it back. False if it did not take: last is
// then unchanged, so the next call tries again in the next slot.
static bool put(int32_t count, bool safe)
{
    if (at >= VP_PER) {
        sector = (sector + 1) % VP_SECTORS;
        at = 0;
    }
    if (at == 0 && !extflash_erase_4k(addr(sector, 0)))
        return false;
    vp_entry_t e = { last.seq + 1, count, safe ? VP_SAFE : VP_UNSAFE, 0 }, back;
    if (e.seq == 0xFFFFFFFFUL)
        e.seq = 1;
    e.crc = crc32(&e, offsetof(vp_entry_t, crc));
    uint32_t a = addr(sector, at);
    at++;                       // the slot is used whatever happens
    writes++;
    extflash_write(a, &e, sizeof e);
    extflash_read(a, &back, sizeof back);
    if (memcmp(&e, &back, sizeof e) != 0)
        return false;
    last = e;
    return true;
}

static bool safe_on_flash(void)
{
    return last.seq && last.state == VP_SAFE;
}

// Withdraws a safe entry, two tries. False if both failed: the flash may
// still hold it.
static bool withdraw(int32_t pos)
{
    if (!safe_on_flash())
        return true;
    if (put(pos, false) || put(pos, false)) {
        warned = false;
        return true;
    }
    // Once per failure, not every main-loop pass.
    if (!warned)
        valve_event("REF", "Saved position could not be withdrawn (flash write failed), moves refused");
    warned = true;
    return false;
}

// stepper3.c, just before a move starts from rest: the entry goes before
// the motor moves, so a reset mid-move cannot leave a safe one behind. If
// it cannot be withdrawn the move is refused; the entry then still matches
// the valve, which has not moved.
static bool before_move(const char **why)
{
    if (!ok)
        return true;
    offer = false;
    still = false;
    if (withdraw(stepper3_position()))
        return true;
    *why = "saved position could not be withdrawn (flash write failed)";
    return false;
}

static bool erased(const vp_entry_t *e)
{
    const uint8_t *b = (const uint8_t *)e;
    for (size_t i = 0; i < sizeof *e; i++)
        if (b[i] != 0xFF)
            return false;
    return true;
}

void valvepos_init(void)
{
    static vp_entry_t buf[16];
    stepper3_on_move_start(before_move);
    ok = extflash_init();
    if (!ok)
        return;
    // Every slot is read: a failed write can leave a blank or spoilt slot
    // with good entries after it (put() goes on to the next slot), so a
    // blank slot does not end the log. The newest good entry wins; the
    // next entry goes after the last slot used in its sector.
    // As after a reset, also when called again (the host tests).
    memset(&last, 0, sizeof last);
    offer = false;
    still = false;
    warned = false;
    writes = 0;
    sector = 0;
    at = 0;
    for (uint32_t s = 0; s < VP_SECTORS; s++) {
        bool newer = false;
        uint32_t used = 0;
        for (uint32_t i = 0; i < VP_PER; i++) {
            // 64 KB in 256-byte reads, about 0.2 s at start-up.
            if (i % 16 == 0)
                extflash_read(addr(s, i), buf, sizeof buf);
            const vp_entry_t *e = &buf[i % 16];
            if (erased(e))
                continue;
            used = i + 1;     // a torn entry is skipped, not written over
            if (good(e) && (!last.seq || e->seq >= last.seq)) {
                last = *e;
                newer = true;
            }
        }
        if (newer) {
            sector = s;
            at = used;
        }
    }
    if (last.seq && last.state == VP_SAFE) {
        offer = true;
        boot_count = last.count;
        char p[16];
        valve_event("REF", "Saved position on offer: count %ld (%s %%), use it or calibrate",
                    (long)boot_count, fixed_fmt(p, sizeof p,
                    fixed_round((int64_t)boot_count * 10000 / valve_pos_max(), 1), 1));
    }
}

void valvepos_task(void)
{
    if (!ok)
        return;
    bool moving = stepper3_busy() || valve_mode() == VALVE_CALIBRATING;
    bool cal = valve_calibrated();
    int32_t pos = stepper3_position();
    if (moving) {
        still = false;
    } else if (!still) {
        still = true;
        still_since = lv_tick_get();
    }
    if (offer) {
        // Kept, untouched, until the operator answers or something moves.
        if (!moving && !cal)
            return;
        offer = false;
    }
    bool safe = safe_on_flash();
    if (safe && (moving || !cal || pos != last.count))
        (void)withdraw(pos);
    else if (!safe && cal && !moving && still && lv_tick_elaps(still_since) >= STILL_MS)
        put(pos, true);
}

bool valvepos_offer(int32_t *count)
{
    if (offer)
        *count = boot_count;
    return offer;
}

bool valvepos_use(const char **why)
{
    if (!offer) { *why = "no saved position on offer"; return false; }
    if (!valve_restore(boot_count, why))
        return false;
    offer = false;
    return true;
}

void valvepos_decline(void)
{
    offer = false;
}

bool valvepos_command(const char *line)
{
    const char *why;
    if (strcmp(line, "VALVE SAVED?") == 0) {
        if (!ok)
            link_printf("VALVE SAVED serial flash not found\r\n");
        else if (!last.seq)
            link_printf("VALVE SAVED none\r\n");
        else
            link_printf("VALVE SAVED count %ld, %s, entry %lu of sector %lu, %lu writes since boot%s\r\n",
                        (long)last.count, last.state == VP_SAFE ? "safe (at standstill)" : "unsafe (moved since)",
                        (unsigned long)at, (unsigned long)sector, (unsigned long)writes,
                        offer ? ", on offer: VALVE RESTORE to use" : "");
        link_printf("OK\r\n");
    } else if (strcmp(line, "VALVE RESTORE") == 0) {
        if (valvepos_use(&why))
            link_printf("OK position %ld from the saved count\r\n", (long)boot_count);
        else
            link_printf("ERR %s\r\n", why);
    } else {
        return false;
    }
    return true;
}
