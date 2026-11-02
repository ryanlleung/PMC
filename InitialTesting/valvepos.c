#include "pmc_config.h"
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
#define VP_SAFE      0x45464153UL   // "SAFE": at standstill
#define VP_MOVING    0x45564F4DUL   // "MOVE": during a move, within MOVE_MS of travel
#define VP_UNSAFE    0UL            // not calibrated: nothing to offer
#define STILL_MS     2000
#define MOVE_MS      2000           // a moving entry at most this often (Ryan, 22 Oct)

typedef struct {
    uint32_t seq;        // +1 each entry, never 0xFFFFFFFF
    int32_t count;
    uint32_t state;      // VP_SAFE, VP_MOVING or VP_UNSAFE, XOR the drive's reference (ref_sig)
    uint32_t crc;
} vp_entry_t;
_Static_assert(sizeof(vp_entry_t) == 16, "16-byte entries");

static bool ok;                 // flash found
static uint32_t sector, at;  // where the next entry goes
static vp_entry_t last;         // newest good entry (seq 0: none)
static bool offer;
static int32_t boot_count;
static bool boot_moving;        // the offer was saved during a move
static uint32_t put_at;         // tick of the last entry written
static uint32_t still_since;
static bool still;             // standing since still_since
static uint32_t writes;
static bool warned;            // withdraw failure logged
static bool failed;            // the last put() did not take
static uint32_t failed_at;     // tick of that put()

/* The direction and coil order the count was saved under (0 for the
 * factory ones). A count saved under another direction or order is no
 * position now (a Direction change on Motor override not saved, then a
 * restart): its state does not read as SAFE or MOVING, so it is not
 * offered. Entries from 0.13.27 on carry VP_REF (bit 31, clear in SAFE,
 * MOVING and UNSAFE); older ones do not record a direction and are read as
 * saved under the present one, as before, so an upgrade keeps them. */
#define VP_REF 0x80000000UL
static uint32_t ref_sig(void)
{
    uint8_t o[4];
    stepper3_order(o);
    uint32_t sig = (stepper3_reverse() ? 0x100u : 0u) | (uint32_t)(o[0] << 6 | o[1] << 4 | o[2] << 2 | o[3]);
    return sig ^ (0u << 6 | 2u << 4 | 1u << 2 | 3u);   // factory: forward, order 0213
}
static uint32_t state_of(const vp_entry_t *e)
{
    return e->state & VP_REF ? (e->state & ~VP_REF) ^ ref_sig() : e->state;
}
// SAFE or MOVING under whatever direction and order it was saved: it could
// be offered at a restart under those, so a withdrawal must cover it.
static bool live(const vp_entry_t *e)
{
    uint32_t s = e->state & ~VP_REF;   // the reference bits stay below 0x200
    return e->seq && (((s ^ VP_SAFE) & ~0x1FFu) == 0 || ((s ^ VP_MOVING) & ~0x1FFu) == 0);
}

static bool good(const vp_entry_t *e)
{
    return e->seq != 0xFFFFFFFFUL && e->seq != 0 && e->crc == crc32_ieee(e, offsetof(vp_entry_t, crc));
}

static uint32_t addr(uint32_t s, uint32_t i)
{
    return VP_BASE + s * VP_SECTOR + i * sizeof(vp_entry_t);
}

// Writes one entry and reads it back. False if it did not take: last is
// then unchanged, so the next call tries again in the next slot.
static bool put(int32_t count, uint32_t state)
{
    if (at >= VP_PER) {
        sector = (sector + 1) % VP_SECTORS;
        at = 0;
    }
    failed = true;              // until it is read back right
    failed_at = lv_tick_get();
    if (at == 0 && !extflash_erase_4k(addr(sector, 0)))
        return false;
    vp_entry_t e = { last.seq + 1, count, (state ^ ref_sig()) | VP_REF, 0 }, back;
    if (e.seq == 0xFFFFFFFFUL)
        e.seq = 1;
    e.crc = crc32_ieee(&e, offsetof(vp_entry_t, crc));
    uint32_t a = addr(sector, at);
    at++;                       // the slot is used whatever happens
    writes++;
    put_at = lv_tick_get();
    extflash_write(a, &e, sizeof e);
    extflash_read(a, &back, sizeof back);
    if (memcmp(&e, &back, sizeof e) != 0)
        return false;
    last = e;
    failed = false;
    return true;
}

static bool safe_on_flash(void)
{
    return last.seq && state_of(&last) == VP_SAFE;
}

static bool offerable(const vp_entry_t *e)
{
    return e->seq && (state_of(e) == VP_SAFE || state_of(e) == VP_MOVING);
}

// Writes state at pos, two tries. False if both failed.
static bool put2(int32_t pos, uint32_t state)
{
    return put(pos, state) || put(pos, state);
}

// Withdraws a safe or moving entry (calibration lost), two tries. False if
// both failed: the flash may still hold it.
static bool withdraw(int32_t pos)
{
    if (!live(&last))
        return true;
    if (put2(pos, VP_UNSAFE)) {
        warned = false;
        return true;
    }
    // Once per failure, not every main-loop pass.
    if (!warned)
        valve_event("REF", "Saved position could not be withdrawn (flash write failed), moves refused");
    warned = true;
    return false;
}

// stepper3.c, just before a move starts from rest: a moving entry at the
// start goes before the motor turns, so a reset mid-move never leaves the
// standstill one behind; valvepos_task() then adds one every MOVE_MS while
// it moves. Not calibrated: the entry is withdrawn instead. If neither can
// be written the move is refused; the entry then still matches the valve,
// which has not moved.
static bool before_move(const char **why)
{
    if (!ok)
        return true;
    offer = false;
    still = false;
    int32_t pos = stepper3_position() / stepper3_scale();   // saved in full steps
    if (valve_calibrated() ? put2(pos, VP_MOVING) : withdraw(pos)) {
        warned = false;
        return true;
    }
    *why = "saved position could not be written (flash write failed)";
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
    failed = false;
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
    if (offerable(&last)) {
        offer = true;
        boot_count = last.count;
        boot_moving = state_of(&last) == VP_MOVING;
        char p[16];
        valve_event("REF", "Saved position on offer: count %ld (%s %%)%s, use it or calibrate",
                    (long)boot_count, fixed_fmt(p, sizeof p,
                    fixed_round((int64_t)boot_count * stepper3_scale() * 10000 / valve_pos_max(), 1), 1),
                    boot_moving ? ", saved while moving" : "");
    } else if (live(&last)) {
        // Saved as SAFE or MOVING under another direction or coil order.
        valve_event("REF", "Saved position not offered: saved under another motor direction or coil order");
    }
}

void valvepos_task(void)
{
    if (!ok)
        return;
    bool moving = stepper3_busy() || valve_mode() == VALVE_CALIBRATING;
    bool cal = valve_calibrated();
    int32_t pos = stepper3_position() / stepper3_scale();   // saved in full steps
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
    // After a failed write, MOVE_MS before the next try from here: each try
    // can take an erase (up to 100 ms) and uses up a slot.
    if (failed && lv_tick_elaps(failed_at) < MOVE_MS)
        return;
    if (!cal) {
        (void)withdraw(pos);
    } else if (moving) {
        // The count every MOVE_MS while it moves (a write each time, so
        // no more often: a 4 KB sector takes 256, each erase 100,000 times).
        if (pos != last.count && lv_tick_elaps(put_at) >= MOVE_MS && !put(pos, VP_MOVING)) {
            // The flash no longer keeps up: stop, so the entry on it stays
            // within MOVE_MS of travel, as the start-up question says.
            stepper3_stop();
            valve_event("REF", "Saved position could not be written during the move (flash): valve stopped");
        }
    } else if (offerable(&last) && pos != last.count) {
        (void)put(pos, VP_MOVING);   // set without a move (VALVE CLOSED, restore): a new count
    } else if (!safe_on_flash() && still && lv_tick_elaps(still_since) >= STILL_MS) {
        put(pos, VP_SAFE);
    }
}

// The offer still stands: moot once the position is set another way (Set
// position, homing; valvepos_task withdraws it too, but only on its next
// pass, and the screen asks again before that), or once the direction or
// coil order changed (Motor override) while it waited.
static bool offer_stands(void)
{
    if (offer && (valve_calibrated() || !offerable(&last)))
        offer = false;
    return offer;
}

bool valvepos_offer(int32_t *count)
{
    if (offer_stands())
        *count = boot_count * stepper3_scale();
    return offer;
}

bool valvepos_offer_moving(int32_t *steps)
{
    // The most the valve can have gone since the entry: MOVE_MS between
    // entries, the step interrupt's 1 s stall stop and a loop pass (1 s
    // allowed), at the fastest rate (a move may have run at a rate that was
    // not saved) and in HALF's counts if it is HALF now (it may have run in
    // FULL).
    *steps = (int32_t)((uint64_t)STEPPER3_RATE_MAX * stepper3_scale() * (MOVE_MS + 2000) / 1000);
    return offer_stands() && boot_moving;
}

bool valvepos_use(const char **why)
{
    if (!offer_stands()) { *why = "No saved position to use"; return false; }
    if (!valve_restore(boot_count * stepper3_scale(), why))
        return false;
    offer = false;
    return true;
}

void valvepos_decline(void)
{
    offer = false;
}

#if PMC_ENG_COMMANDS
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
                        (long)last.count, state_of(&last) == VP_SAFE ? "safe (at standstill)" :
                        state_of(&last) == VP_MOVING ? "saved while moving" :
                        state_of(&last) == VP_UNSAFE ? "unsafe (not calibrated)" : "saved under another direction or coil order",
                        (unsigned long)at, (unsigned long)sector, (unsigned long)writes,
                        offer ? ", on offer: VALVE RESTORE to use" : "");
        link_printf("OK\r\n");
    } else if (strcmp(line, "VALVE RESTORE") == 0) {
        if (valvepos_use(&why))
            link_printf("OK position %ld from the saved count\r\n", (long)boot_count);
        else
            link_err(why);
    } else {
        return false;
    }
    return true;
}
#endif

