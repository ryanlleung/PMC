/* Host test of ctrl.c (stage 8) against the C port of the simulator's flow
 * line (model.c). Runs the porting plan's cases and checks them:
 *   closed to 5 mB, 4.5 to 10 mB, targets 5, 7, 3, 5 with no false trip,
 *   helium off -> F-06, pump off -> W-02 to MANUAL (no wrong-way fault since 0.11.20),
 *   slipping drive -> F-06, AUTO min 0 closing to the seat without a fault, and the
 *   entry refusals. -v prints a line a second. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ctrl.h"
#include "model.h"
#include <stdlib.h>

static bool verbose;
static int fails;
#define CHECK(c, ...) do { bool ok_ = (c); printf("%s ", ok_ ? "PASS" : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)

// The simulator's DEF_P, as the firmware defaults (cfg.c): KP 4, TI 0.25,
// OUTMIN 14, DB 8, KDIR +1, BAND 0.1; POS_MAX as the model (6 turns).
static ctrl_tune_t K = { 4.0f, 0.25f, 8, 1, 0.1f, 14.0f, 99072 };
#define FS 1034.0f

typedef enum { MANUAL, AUTO } mode_t_;
static bool w02;
static mode_t_ mode;
static ctrl_t C;
static ctrl_result_t fault;   // last fault, CTRL_RUN if none
static double fault_t;

static uint32_t ms(void) { return (uint32_t)llround(M.t * 1000); }

// The simulator's requestMove(target, quiet) in AUTO.
static bool request_move(int32_t target)
{
    if (fault != CTRL_RUN) return false;
    if (target < 0) target = 0;
    if (target > K.pos_max) target = K.pos_max;
    if (!M.moving && target < M.pos && M.lim_active) return false;
    return model_start_move(target);
}

static void ctrl_tick(void)
{
    ctrl_track(&C, M.pos, true);
    if (mode != AUTO) { ctrl_idle(&C); return; }
    ctrl_in_t in = { ms(), (float)M.pv, M.pos, M.moving, M.lim_active, FS };
    ctrl_result_t r = ctrl_step(&C, &K, &in);
    if (r == CTRL_RUN) {
        if (C.move && request_move(C.move_to))
            ctrl_sent(&C);
    } else if (r == CTRL_W02) {
        model_stop();
        mode = MANUAL;
        w02 = true;
    } else {
        model_stop();
        fault = r;
        fault_t = M.t;
        mode = MANUAL;
    }
}

typedef struct { double t_band, over, under, end, step; bool in_band; } met_t;

// Runs s seconds; the simulator's met(): first time in band, then the
// largest excursions either side after it.
static met_t run(double s, float sp)
{
    met_t m = { -1, -1e9, 1e9, 0, -1e9, false };
    long n = lround(s / MODEL_DT);
    for (long i = 0; i < n; i++) {
        if (model_tick())
            ctrl_tick();
        if (i % 100 == 99) {
            double e = M.pv - sp;
            if (!m.in_band && fabs(e) <= K.band) { m.in_band = true; m.t_band = M.t; }
            if (m.in_band) {
                if (e > m.over) m.over = e;
                if (e < m.under) m.under = e;
                if (M.t - m.t_band <= 200 && e > m.step) m.step = e;   // overshoot of the step itself
            }
            if (verbose)
                printf("%8.1f pv %8.3f sp %5.1f pos %6ld mode %d fault %d\n", M.t, M.pv, sp, (long)M.pos, mode, fault);
        }
    }
    m.end = M.pv;
    return m;
}

static bool to_auto(void)
{
    const char *why = ctrl_check(&C, &K, true, fault != CTRL_RUN, true, (float)M.pv, FS);
    if (why) { printf("     refused: %s\n", why); return false; }
    ctrl_start(&C, &K, ms(), (float)M.pv, M.pos);
    mode = AUTO;
    return true;
}

static void set_sp(float sp) { ctrl_set_sp(&C, sp, FS, ms()); }

static void fresh(void)
{
    model_init();
    ctrl_init(&C);
    mode = MANUAL;
    fault = CTRL_RUN;
    w02 = false;
    run(120, 0);
}

// The same cases run in the simulator v63 (Playwright, seeded Math.random as
// its tests): first time in band (s), then the largest excursions (mbar).
typedef struct { double t_band, over, under, end; } ref_t;
static const ref_t REF_5 = { 255.9, 0.038, -0.090, 5.008 }, REF_45 = { 1645.9, 0.086, -0.036, 4.508 },
                   REF_10 = { 2615.9, 15.023, -0.144, 9.925 };
static const ref_t REF_SEQ[4] = { { 3759.9, 2.934, -0.765, 5.027 }, { 4636.9, 0.075, -0.366, 7.024 },
                                  { 5561.9, 1.964, -0.683, 3.004 }, { 6435.9, 0.448, -0.996, 5.000 } };

// Matches the simulator: in band within 5 s of it, excursions within 0.02 mB.
static bool same(met_t m, ref_t r)
{
    return fabs(m.t_band - r.t_band) <= 5 && fabs(m.over - r.over) <= 0.02 && fabs(m.under - r.under) <= 0.02 &&
           fabs(m.end - r.end) <= 0.02;
}

static void report(const char *what, met_t m, ref_t r)
{
    CHECK(same(m, r), "%s: in band at %.0f s (sim %.0f), then %+.3f / %+.3f mB (sim %+.3f / %+.3f), end %.3f (sim %.3f)",
          what, m.t_band, r.t_band, m.over, m.under, r.over, r.under, m.end, r.end);
}

int main(int argc, char **argv)
{
    verbose = argc > 1 && !strcmp(argv[1], "-v");

    // Entry refusals (the simulator's toAuto order).
    ctrl_init(&C);
    CHECK(!strcmp(ctrl_check(&C, &K, false, false, true, 1, FS), "Home the valve first"), "refused unhomed");
    CHECK(!strcmp(ctrl_check(&C, &K, true, true, true, 1, FS), "Reset the fault first"), "refused with a fault");
    CHECK(!strcmp(ctrl_check(&C, &K, true, false, false, 1, FS), "No valid pressure"), "refused without a reading");
    CHECK(!strcmp(ctrl_check(&C, &K, true, false, true, 1, FS), "Set a target first"), "refused without a target");
    CHECK(!ctrl_set_sp(&C, FS + 1, FS, 0), "target above the range rejected");
    ctrl_set_sp(&C, 5, FS, 0);
    CHECK(!strcmp(ctrl_check(&C, &K, true, false, true, FS + 1, FS), "Pressure above the sensor range"), "refused above the range");
    ctrl_tune_t k0 = K; k0.kdir = 0;
    CHECK(!strcmp(ctrl_check(&C, &k0, true, false, true, 1, FS), "Pressure response not set"), "refused without KDIR");
    CHECK(ctrl_check(&C, &K, true, false, true, 1, FS) == NULL, "accepted when all set");

    // Bumpless entry: the first output is the position in force.
    fresh();
    model_start_move(33000);
    run(120, 0);
    int32_t at = M.pos;
    set_sp(5);
    to_auto();
    CHECK(C.pos_cmd == at && C.last_sent == at, "bumpless: command %ld = position %ld", (long)C.pos_cmd, (long)at);

    // The reference sequence (also run in the simulator, sim_ref.js).
    fresh();
    set_sp(5);
    to_auto();
    met_t a = run(1500, 5);
    report("closed to 5", a, REF_5);
    CHECK(a.step <= 0.05 && fault == CTRL_RUN && mode == AUTO, "closed to 5 mB: overshoot %.3f mB <= 0.05 (sim 0.038)", a.step);
    set_sp(4.5f);
    report("5 to 4.5", run(900, 4.5f), REF_45);
    set_sp(10);
    met_t b2 = run(1200, 10);
    report("4.5 to 10", b2, REF_10);
    CHECK(fabs(b2.step - 0.362) <= 0.02 && fault == CTRL_RUN && mode == AUTO,
          "4.5 to 10 mB: overshoot %.3f mB (sim 0.362; the +15 mB later is the pot filling, in both)", b2.step);
    const float seq[] = { 5, 7, 3, 5 };
    bool trip = false, settle = true;
    for (int i = 0; i < 4; i++) {
        set_sp(seq[i]);
        met_t m = run(900, seq[i]);
        char what[20];
        snprintf(what, sizeof what, "target %.0f", seq[i]);
        report(what, m, REF_SEQ[i]);
        trip |= fault != CTRL_RUN || mode != AUTO;
        settle &= m.t_band > 0 && fabs(m.end - seq[i]) <= K.band;
    }
    CHECK(!trip && settle, "targets 5, 7, 3, 5: settled, no false trip");
    M.he = false;
    double t0 = M.t;
    run(2000, 5);
    // No wrong-way fault: with the valve pinned open and the pressure flat,
    // the response monitor catches it instead.
    CHECK(fault == CTRL_F06 && mode == MANUAL && !M.moving, "helium off: F-06 after %.0f s, MANUAL, valve idle", fault_t - t0);

    // Pump off at 5 mB.
    fresh();
    set_sp(5);
    to_auto();
    run(1500, 5);
    M.pump = false;
    t0 = M.t;
    run(2000, 5);
    CHECK(fault == CTRL_RUN && w02 && mode == MANUAL, "pump off: no fault, W-02 to MANUAL (t %.0f s)", M.t - t0);

    // Slipping drive at 5 mB, then a step in target.
    fresh();
    set_sp(5);
    to_auto();
    run(1500, 5);
    M.coupled = false;
    set_sp(7);
    t0 = M.t;
    run(900, 7);
    CHECK(fault == CTRL_F06 && mode == MANUAL && fabs(fault_t - t0 - 150) <= 2, "slipping drive: F-06 after %.0f s (sim 150)", fault_t - t0);

    // Room air, target above the reading (the bench test with no process).
    fresh();
    M.he = false; M.pump = false; M.P = 1013;
    run(30, 0);
    set_sp(1030);
    to_auto();
    t0 = M.t;
    run(400, 1030);
    CHECK(fault == CTRL_F06, "room air, target above: F-06 after %.0f s", fault_t - t0);

    // AUTO min 0 (the factory value since 0.11.33; the cases above keep the
    // simulator's 14 to match its references): a target the closed valve
    // cannot reach drives it to closed (within 1 %), with no false fault.
    const float outmin = K.outmin;
    K.outmin = 0;
    fresh();
    set_sp(5);
    to_auto();
    met_t z = run(1500, 5);
    CHECK(z.t_band > 0 && fabs(z.end - 5) <= K.band && fault == CTRL_RUN && mode == AUTO,
          "AUTO min 0: settles at 5 mB (end %.3f)", z.end);
    set_sp(0);
    run(1200, 0);
    CHECK(fault == CTRL_RUN && mode == AUTO && M.pos <= K.pos_max / 100,
          "AUTO min 0, target 0: closes to %ld steps (%.2f mB), no fault", (long)M.pos, M.pv);
    K.outmin = outmin;

    // A live KP or KDIR change in AUTO keeps the output where it is (0.11.38).
    fresh();
    model_start_move(33000);
    run(120, 0);
    set_sp(5);
    to_auto();
    run(5, 5);
    {
        const ctrl_tune_t kk = K;
        int32_t before = C.pos_cmd;
        K.kp *= 2;
        ctrl_tick();
        int32_t d_kp = C.pos_cmd - before;
        before = C.pos_cmd;
        K.kdir = -K.kdir;
        ctrl_tick();
        int32_t d_dir = C.pos_cmd - before;
        CHECK(labs(d_kp) < 60 && labs(d_dir) < 60,
              "KP doubled and KDIR flipped in AUTO: command moves %ld and %ld steps (no jump)", (long)d_kp, (long)d_dir);
        K = kk;
    }

    // A target far above the pressure opens the valve fully (0.11.39). One
    // step of ki x e alone overshoots the top, which used to freeze the
    // integral and leave the valve where AUTO started.
    fresh();
    set_sp(500);
    to_auto();
    run(30, 500);
    CHECK(C.pos_cmd == K.pos_max && fault == CTRL_RUN,
          "Target 500 mB from %.1f mB: command %ld of %ld steps (fully open)", M.pv, (long)C.pos_cmd, (long)K.pos_max);

    printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
