/* Host test of ctrl.c (stage 8) against the C port of the simulator's flow
 * line (model.c). Runs the porting plan's cases and checks them:
 *   closed to 5 mB, 4.5 to 10 mB, targets 5, 7, 3, 5 with no false trip,
 *   helium off -> F-6, pump off -> W-2 to MANUAL (no wrong-way fault since 0.11.20),
 *   slipping drive -> F-6, AUTO min 0 closing to the seat without a fault, and the
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
// OUTMIN 14, DB 8, BAND 0.1; POS_MAX as the model (6 turns). The PID table is
// always on (0.16.30): boundary 1 is out of reach here, so set 1 runs at
// every pressure, and the cases that test the table set their own.
#define B_NEVER 1e9f
static ctrl_tune_t K = { .kp = 4.0f, .ti = 0.25f, .db = 8, .band = 0.1f, .outmin = 14.0f, .pos_max = 99072,
                         .sets = { { 4.0f, 0.25f, 0 }, { 4.0f, 0.25f, 0 }, { 4.0f, 0.25f, 0 } },
                         .b1 = B_NEVER, .b2 = 2 * B_NEVER };
#define FS 1034.0f

typedef enum { MANUAL, AUTO } mode_t_;
static bool w02;
static mode_t_ mode;
static ctrl_t C;
static ctrl_result_t fault;   // last fault, CTRL_RUN if none
static double fault_t;

static uint32_t ms(void) { return (uint32_t)llround(M.t * 1000); }

// false: no reversal hold, as the simulator v63 (the reference cases).
// fresh() applies it.
static bool smooth = true;
static long n_starts, n_rev;   // moves sent and reversals among them
static int last_way;

// The simulator's requestMove(target, quiet) in AUTO.
static bool request_move(int32_t target)
{
    if (fault != CTRL_RUN) return false;
    if (target < 0) target = 0;
    if (target > K.pos_max) target = K.pos_max;
    if (!M.moving && target < M.pos && M.lim_active) return false;
    // valve.c: stepper3_goto, at MOT RATE.
    int way = target > M.pos ? 1 : target < M.pos ? -1 : 0;
    if (!model_start_move(target)) return false;
    if (way) {
        n_starts++;
        if (last_way && way != last_way) n_rev++;
        last_way = way;
    }
    return true;
}

// PID set changes (gain schedule, 0.15.15): the pressure at each, the
// command's step at that control step, and the step a change of KP with no
// re-base would have given ((KP new - KP old) x error).
#define SW_MAX 32
static int sw_n, sw_from[SW_MAX], sw_to[SW_MAX];
static float sw_pv[SW_MAX];
static long sw_jump[SW_MAX], sw_kick[SW_MAX], sw_int[SW_MAX], sw_kpe[SW_MAX];

static void ctrl_tick(void)
{
    ctrl_track(&C, M.pos, true);
    if (mode != AUTO) { ctrl_idle(&C); return; }
    ctrl_in_t in = { ms(), (float)M.pv, M.pos, M.moving, M.lim_active, FS };
    const ctrl_t before = C;
    ctrl_result_t r = ctrl_step(&C, &K, &in);
    if (C.set != before.set && before.set && sw_n < SW_MAX) {
        const float e = C.sp - in.pv;
        const float dkp = (ctrl_set_gains(&K, C.set).kp - ctrl_set_gains(&K, before.set).kp) / 100.0f * (float)K.pos_max;
        sw_from[sw_n] = before.set;
        sw_to[sw_n] = C.set;
        sw_pv[sw_n] = in.pv;
        sw_kick[sw_n] = lroundf(dkp * e);
        const ctrl_pid_t g = ctrl_set_gains(&K, C.set);
        // This step's integral, and the new KP x the change in the error
        // since the last step, less KP x any setpoint change (P on the
        // pressure only, 0.16.15).
        const float kpn = g.kp / 100.0f * (float)K.pos_max;
        // The integral is skipped while the valve still travels to the last
        // command (travel anti-windup), so either is right.
        const float lo = K.outmin / 100.0f * (float)K.pos_max;
        const float kick = (float)before.pos_cmd + kpn * (e - before.e_last - (C.sp - before.sp_used));
        const float want = kick + (g.ti > 0 ? kpn / (60.0f * g.ti) * e : 0);
        sw_int[sw_n] = lroundf((want < lo ? lo : want > (float)K.pos_max ? (float)K.pos_max : want) - (float)before.pos_cmd);
        sw_kpe[sw_n] = lroundf((kick < lo ? lo : kick > (float)K.pos_max ? (float)K.pos_max : kick) - (float)before.pos_cmd);
        sw_jump[sw_n++] = (long)(C.pos_cmd - before.pos_cmd);
    }
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

typedef struct { double t_band, over, under, end, step, tail; bool in_band; } met_t;

// Runs s seconds; the simulator's met(): first time in band, then the
// largest excursions either side after it.
static met_t run(double s, float sp)
{
    met_t m = { -1, -1e9, 1e9, 0, -1e9, 0, false };
    long n = lround(s / MODEL_DT), nt = 0;
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
            if ((double)(n - i) * MODEL_DT <= 60) { m.tail += M.pv; nt++; }   // the last 60 s
            if (verbose)
                printf("%8.1f pv %8.3f sp %5.1f pos %6ld mode %d fault %d\n", M.t, M.pv, sp, (long)M.pos, mode, fault);
        }
    }
    m.end = M.pv;
    m.tail = nt ? m.tail / nt : M.pv;
    return m;
}

static bool to_auto(void)
{
    const char *why = ctrl_check(&C, &K, true, fault != CTRL_RUN, true, FS);
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
    if (!smooth)
        C.rev_pct = 0;
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
// That holds for the start from closed only. The simulator's PI kicks on a
// setpoint step (P on the setpoint and the pressure); the firmware's has not
// since 0.16.15, and the kick option went in 0.16.30. After a setpoint step
// (step true) the run is in band within 60 s of the simulator's and ends
// where it does; the excursions on the way differ.
static bool same(met_t m, ref_t r, bool step)
{
    if (step)
        return m.t_band > 0 && fabs(m.t_band - r.t_band) <= 60 && fabs(m.end - r.end) <= 0.02;
    return fabs(m.t_band - r.t_band) <= 5 && fabs(m.over - r.over) <= 0.02 && fabs(m.under - r.under) <= 0.02 &&
           fabs(m.end - r.end) <= 0.02;
}

static void report(const char *what, met_t m, ref_t r, bool step)
{
    CHECK(same(m, r, step), "%s: in band at %.0f s (sim %.0f), then %+.3f / %+.3f mB (sim %+.3f / %+.3f), end %.3f (sim %.3f)",
          what, m.t_band, r.t_band, m.over, m.under, r.over, r.under, m.end, r.end);
}

int main(int argc, char **argv)
{
    verbose = argc > 1 && !strcmp(argv[1], "-v");

    // Entry refusals (the simulator's toAuto order).
    ctrl_init(&C);
    CHECK(!strcmp(ctrl_check(&C, &K, false, false, true, FS), "Home the valve first"), "refused unhomed");
    CHECK(!strcmp(ctrl_check(&C, &K, true, true, true, FS), "Reset the fault first"), "refused with a fault");
    CHECK(!strcmp(ctrl_check(&C, &K, true, false, false, FS), "No valid pressure"), "refused without a reading");
    CHECK(!strcmp(ctrl_check(&C, &K, true, false, true, FS), "Set a setpoint first"), "refused without a setpoint");
    CHECK(!ctrl_set_sp(&C, FS + 1, FS, 0), "target above the range rejected");
    ctrl_set_sp(&C, 5, FS, 0);
    CHECK(ctrl_check(&C, &K, true, false, true, FS) == NULL, "accepted when all set");

    // Bumpless entry: the first output is the position in force.
    fresh();
    model_start_move(33000);
    run(120, 0);
    int32_t at = M.pos;
    set_sp(5);
    to_auto();
    CHECK(C.pos_cmd == at && C.last_sent == at, "bumpless: command %ld = position %ld", (long)C.pos_cmd, (long)at);

    // The reference sequence (also run in the simulator, sim_ref.js), with
    // the reversal hold off, as there.
    smooth = false;
    fresh();
    set_sp(5);
    to_auto();
    met_t a = run(1500, 5);
    report("closed to 5", a, REF_5, false);
    CHECK(a.step <= 0.05 && fault == CTRL_RUN && mode == AUTO, "closed to 5 mB: overshoot %.3f mB <= 0.05 (sim 0.038)", a.step);
    set_sp(4.5f);
    report("5 to 4.5", run(900, 4.5f), REF_45, true);
    set_sp(10);
    met_t b2 = run(1200, 10);
    report("4.5 to 10", b2, REF_10, true);
    CHECK(fabs(b2.step - 0.362) <= 0.02 && fault == CTRL_RUN && mode == AUTO,
          "4.5 to 10 mB: overshoot %.3f mB (sim 0.362; the +15 mB later is the pot filling, in both)", b2.step);
    const float seq[] = { 5, 7, 3, 5 };
    bool trip = false, settle = true;
    for (int i = 0; i < 4; i++) {
        set_sp(seq[i]);
        met_t m = run(900, seq[i]);
        char what[20];
        snprintf(what, sizeof what, "target %.0f", seq[i]);
        report(what, m, REF_SEQ[i], true);
        trip |= fault != CTRL_RUN || mode != AUTO;
        settle &= m.t_band > 0 && fabs(m.end - seq[i]) <= K.band;
    }
    CHECK(!trip && settle, "targets 5, 7, 3, 5: settled, no false trip");
    smooth = true;
    M.he = false;
    double t0 = M.t;
    run(2000, 5);
    // No wrong-way fault: with the valve pinned open and the pressure flat,
    // the response monitor catches it instead.
    CHECK(fault == CTRL_F06 && mode == MANUAL && !M.moving, "helium off: F-6 after %.0f s, MANUAL, valve idle", fault_t - t0);

    // The same targets as the firmware runs, with the reversal hold: in
    // band no later than 60 s after the reference, and the last 60 s within
    // the band on average (one sample wanders 0.1 mB with the hold).
    {
        fresh();
        set_sp(5);
        to_auto();
        const float sp[] = { 5, 4.5f, 10, 5, 7, 3, 5 };
        const double len[] = { 1500, 900, 1200, 900, 900, 900, 900 };
        const ref_t *ref[] = { &REF_5, &REF_45, &REF_10, &REF_SEQ[0], &REF_SEQ[1], &REF_SEQ[2], &REF_SEQ[3] };
        bool ok = true;
        for (int i = 0; i < 7; i++) {
            set_sp(sp[i]);
            met_t m = run(len[i], sp[i]);
            printf("     with the hold, target %.1f: in band at %.0f s (reference %.0f), then %+.3f / %+.3f mB (reference %+.3f / %+.3f), end %.3f, last 60 s %.3f\n",
                   sp[i], m.t_band, ref[i]->t_band, m.over, m.under, ref[i]->over, ref[i]->under, m.end, m.tail);
            ok &= m.t_band > 0 && m.t_band <= ref[i]->t_band + 60 && fabs(m.tail - sp[i]) <= K.band &&
                  fault == CTRL_RUN && mode == AUTO && (i || m.step <= 0.1);
        }
        CHECK(ok, "with the hold, no kick: targets 5, 4.5, 10, 5, 7, 3, 5 settle within 60 s of the reference, no trip");
    }
    // Settled at 5 mbar, 10 minutes: how often the motor turns round.
    {
        long st[2], rv[2];
        double lo[2], hi[2];
        for (int f = 0; f < 2; f++) {
            smooth = f == 1;
            fresh();
            set_sp(5);
            to_auto();
            run(1200, 5);
            n_starts = n_rev = 0;
            lo[f] = 1e9; hi[f] = -1e9;
            for (int i = 0; i < 600; i++) {
                run(1, 5);
                lo[f] = fmin(lo[f], M.pv); hi[f] = fmax(hi[f], M.pv);
            }
            st[f] = n_starts; rv[f] = n_rev;
        }
        smooth = true;
        printf("     settled 10 min: %ld moves, %ld reversals, %.3f to %.3f mB without; "
               "%ld moves, %ld reversals, %.3f to %.3f mB with the hold\n", st[0], rv[0], lo[0], hi[0], st[1], rv[1], lo[1], hi[1]);
        CHECK(rv[1] * 4 <= rv[0] && hi[1] - 5 <= K.band && 5 - lo[1] <= K.band,
              "settled: reversals %ld, from %ld without the hold, still within the band", rv[1], rv[0]);
    }

    // Pump off at 5 mB.
    fresh();
    set_sp(5);
    to_auto();
    run(1500, 5);
    M.pump = false;
    t0 = M.t;
    run(2000, 5);
    CHECK(fault == CTRL_RUN && w02 && mode == MANUAL, "pump off: no fault, W-2 to MANUAL (t %.0f s)", M.t - t0);

    // A setpoint band of 0 (0.16.16, Ryan 12 Nov): In band and Stable never
    // show, but a steady hold is not ended by W-2, which still judges
    // against CTRL_TCONV_BAND.
    {
        const ctrl_tune_t kk = K;
        K.band = 0;
        fresh();
        set_sp(5);
        to_auto();
        run(CTRL_TCONV_S + 1200, 5);
        const ctrl_reg_t reg = ctrl_regulation(&C, &K, ms(), true, 5.04f);
        CHECK(fault == CTRL_RUN && !w02 && mode == AUTO && reg == CTRL_REG_REGULATING,
              "band 0: %.0f min in AUTO at 5 mB, no W-2, reads Regulating (%d)", (CTRL_TCONV_S + 1200) / 60.0, (int)reg);
        K = kk;
    }

    // Slipping drive at 5 mB, then a step in target.
    fresh();
    set_sp(5);
    to_auto();
    run(1500, 5);
    M.coupled = false;
    set_sp(7);
    t0 = M.t;
    run(900, 7);
    CHECK(fault == CTRL_F06 && mode == MANUAL && fabs(fault_t - t0 - 150) <= 2, "slipping drive: F-6 after %.0f s (sim 150)", fault_t - t0);

    // Room air, target above the reading (the bench test with no process).
    fresh();
    M.he = false; M.pump = false; M.P = 1013;
    run(30, 0);
    set_sp(1030);
    to_auto();
    t0 = M.t;
    run(400, 1030);
    CHECK(fault == CTRL_F06, "room air, target above: F-6 after %.0f s", fault_t - t0);

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

    // A live KP change in AUTO keeps the output where it is (0.11.38).
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
        CHECK(labs(d_kp) < 60, "KP doubled in AUTO: command moves %ld steps (no jump)", (long)d_kp);
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

    // KD (0.14.21, a setting): the D term works on the measured pressure's
    // rate and against it; KD 0 is the PI as before.
    {
        ctrl_tune_t k0 = K, k1 = K;
        k0.kd = 0;
        k1.kd = 1.0f;   // 1 % of travel per mbar/s
        ctrl_t a, b;
        ctrl_init(&a);
        ctrl_init(&b);
        ctrl_set_sp(&a, 5, FS, 0);
        ctrl_set_sp(&b, 5, FS, 0);
        ctrl_start(&a, &k0, 0, 5, 30000);
        ctrl_start(&b, &k1, 0, 5, 30000);
        ctrl_in_t in = { .t_ms = 1000, .pv = 5.5f, .pos = 30000, .fs = FS };
        ctrl_step(&a, &k0, &in);
        ctrl_step(&b, &k1, &in);
        long dd = (long)(b.pos_cmd - a.pos_cmd), want = -lroundf(0.01f * (float)K.pos_max * 0.5f);
        CHECK(labs(dd - want) <= 1, "KD 1 %%/(mB/s), pressure rising 0.5 mB/s: command %ld steps from the PI's (expected %ld)",
              dd, want);
    }

    // Gain schedule (0.15.15): which set, from the boundaries and the hysteresis.
    {
        ctrl_tune_t k = K;
        k.b1 = 8; k.b2 = 30; k.hyst = 0.15f;
        // Up: 1 to 2 past 9.2, 2 to 3 past 34.5. Down: 3 to 2 under 25.5, 2 to 1 under 6.8.
        bool pv = ctrl_pick_set(&k, 0, 8.5f) == 2 && ctrl_pick_set(&k, 1, 9.1f) == 1 && ctrl_pick_set(&k, 1, 9.3f) == 2 &&
                  ctrl_pick_set(&k, 2, 34.4f) == 2 && ctrl_pick_set(&k, 2, 34.6f) == 3 &&
                  ctrl_pick_set(&k, 3, 25.6f) == 3 && ctrl_pick_set(&k, 3, 25.4f) == 2 &&
                  ctrl_pick_set(&k, 2, 6.9f) == 2 && ctrl_pick_set(&k, 2, 6.7f) == 1 &&
                  ctrl_pick_set(&k, 1, 50) == 3 && ctrl_pick_set(&k, 3, 2) == 1;
        CHECK(pv, "PID table: 1>2 past 9.2, 2>3 past 34.5, 3>2 under 25.5, 2>1 under 6.8 mB, both at once on a large move");
        k.sets[1].kp = 0;
        CHECK(!strcmp(ctrl_check(&C, &k, true, false, true, FS), "Tuning not set"), "PID table: a set with KP 0 refused");
    }
    // Four sets (0.16.14): boundary 3 above boundary 2 adds set 4; not above
    // it (0, initialisers without it), three sets as before.
    {
        ctrl_tune_t k = K;
        k.b1 = 6; k.b2 = 10; k.b3 = 14; k.hyst = 0.1f;
        for (int s = 0; s < 3; s++)
            k.sets[s] = (ctrl_pid_t){ 0.5f, 1.0f, 0 };
        // Up: 1>2 past 6.6, 2>3 past 11, 3>4 past 15.4. Down: 4>3 under 12.6, 3>2 under 9, 2>1 under 5.4.
        const bool four = ctrl_sets(&k) == 4 &&
            ctrl_pick_set(&k, 0, 5) == 1 && ctrl_pick_set(&k, 0, 8) == 2 && ctrl_pick_set(&k, 0, 12) == 3 &&
            ctrl_pick_set(&k, 0, 20) == 4 &&
            ctrl_pick_set(&k, 3, 15.3f) == 3 && ctrl_pick_set(&k, 3, 15.5f) == 4 &&
            ctrl_pick_set(&k, 4, 12.7f) == 4 && ctrl_pick_set(&k, 4, 12.5f) == 3 &&
            ctrl_pick_set(&k, 3, 9.1f) == 3 && ctrl_pick_set(&k, 3, 8.9f) == 2 &&
            ctrl_pick_set(&k, 1, 50) == 4 && ctrl_pick_set(&k, 4, 2) == 1;
        k.sets[2].kp = 0;
        const bool check4 = ctrl_check(&C, &k, true, false, true, FS) != NULL;
        k.b3 = 0;
        const bool three = ctrl_sets(&k) == 3 && ctrl_pick_set(&k, 0, 20) == 3 && ctrl_pick_set(&k, 3, 500) == 3 &&
                           ctrl_check(&C, &k, true, false, true, FS) == NULL;
        CHECK(four && check4 && three, "four sets: 6, 10 and 14 mB with 10 %%: 1>2>3>4 past 6.6, 11 and 15.4, back under "
              "12.6, 9 and 5.4, all at once on a large move; set 4 with KP 0 refused; boundary 3 0 keeps three sets");
    }
    // The PID table, 5 to 10 to 5 mB on the model: going up each
    // set change past its boundary by the hysteresis; going down (0.16.9:
    // the lower of pressure and setpoint) straight to the setpoint's set as
    // the setpoint changes. Every change bumpless.
    {
        const ctrl_tune_t kk = K;
        K.b1 = 6; K.b2 = 8.5f; K.hyst = 0.1f;   // up past 6.6 and 9.35, down under 7.65 and 5.4
        K.sets[0] = (ctrl_pid_t){ 3.0f, 0.25f, 0 };
        K.sets[1] = (ctrl_pid_t){ 2.0f, 0.30f, 0 };
        fresh();
        set_sp(5);
        to_auto();
        const int set0 = C.set;
        run(900, 5);
        sw_n = 0;
        set_sp(10);
        met_t up = run(1200, 10);
        const int n_up = sw_n;
        set_sp(5);
        met_t down = run(1200, 5);
        bool order = false, where = true, smooth_ = true;
        long worst = 0;
        for (int i = 0; i < sw_n; i++) {
            const float b = sw_from[i] + sw_to[i] == 3 ? K.b1 : K.b2;
            if (i < n_up)
                where &= sw_to[i] > sw_from[i] && sw_pv[i] > b * 1.1f;
            const long e1 = labs(sw_jump[i] - sw_int[i]), e2 = labs(sw_jump[i] - sw_kpe[i]);
            const long extra = e1 < e2 ? e1 : e2;   // off the kick, with or without this step's integral
            worst = extra > worst ? extra : worst;
            printf("     set %d to %d at %.3f mB: command %+ld steps, the kick %+ld, with this step's integral %+ld (%+ld more without "
                   "the re-base)\n", sw_from[i], sw_to[i], sw_pv[i], sw_jump[i], sw_kpe[i], sw_int[i], sw_kick[i]);
        }
        smooth_ = worst <= 2;
        for (int i = 0; i + 1 < n_up; i++)
            order |= sw_from[i] == 1 && sw_to[i] == 2 && sw_from[i + 1] == 2 && sw_to[i + 1] == 3;
        // Down: one change, 3 to 1, on the first step after the setpoint
        // went to 5, with the pressure still near 10.
        const bool back = sw_n == n_up + 1 && sw_from[n_up] == 3 && sw_to[n_up] == 1 && sw_pv[n_up] > K.b2 * 1.1f;
        CHECK(set0 == 1 && order && back && where, "PID table: set 1 at 5 mB, 1>2>3 going to 10 each past its boundary by "
              "the hysteresis; back to 5, 3>1 at once at %.2f mB (the lower of pressure and setpoint) (%d changes)",
              sw_n > n_up ? sw_pv[n_up] : 0.0f, sw_n);
        CHECK(smooth_, "PID table: each change bumpless, the command moves by the step's integral and the new KP x the "
              "error's change since the last step (%ld steps off)", worst);
        CHECK(fault == CTRL_RUN && mode == AUTO && up.t_band > 0 && down.t_band > 0 && fabs(down.end - 5) <= K.band,
              "PID table: settles at 10 and back at 5 mB, no trip (end %.3f)", down.end);
        K = kk;
    }
    // The set from the lower of pressure and setpoint (0.16.9): going up the
    // pressure's set, as before; going down the setpoint's at once, from
    // AUTO's start too.
    {
        ctrl_tune_t k = K;
        k.b1 = 6; k.b2 = 14; k.hyst = 0.1f;
        k.sets[0] = (ctrl_pid_t){ 0.14f, 1.5f, 0 };
        k.sets[1] = (ctrl_pid_t){ 0.20f, 0.25f, 0 };
        ctrl_t c;
        ctrl_init(&c);
        ctrl_set_sp(&c, 4, FS, 0);
        ctrl_start(&c, &k, 0, 20, 30000);
        const int start_down = c.set;                       // PV 20, SP 4: set 1
        ctrl_set_sp(&c, 20, FS, 0);
        ctrl_start(&c, &k, 0, 20, 30000);
        const int start_at = c.set;                         // PV 20, SP 20: set 3
        ctrl_set_sp(&c, 8, FS, 1000);
        ctrl_in_t in = { .t_ms = 1000, .pv = 20, .pos = 30000, .fs = FS };
        ctrl_step(&c, &k, &in);
        const int step_down = c.set;                        // SP 8 under PV 20: set 2 at once
        ctrl_set_sp(&c, 4, FS, 2000);
        ctrl_start(&c, &k, 2000, 4, 30000);
        ctrl_set_sp(&c, 20, FS, 3000);
        in.t_ms = 3000; in.pv = 4;
        ctrl_step(&c, &k, &in);
        const int step_up = c.set;                          // SP 20 over PV 4: still set 1
        in.t_ms = 4000; in.pv = 7;
        ctrl_step(&c, &k, &in);
        const int up_7 = c.set;                             // PV 7 past 6.6: set 2
        CHECK(start_down == 1 && start_at == 3 && step_down == 2 && step_up == 1 && up_7 == 2,
              "set from the lower of pressure and setpoint: start PV 20 SP 4 set %d, PV 20 SP 20 set %d; SP 20 to 8 set %d at "
              "once; SP 4 to 20 set %d until the pressure passes 6.6 (set %d at 7)", start_down, start_at, step_down, step_up, up_7);
    }
    // No kick on a setpoint step (0.16.15): P acts on the pressure only, so
    // SP 16 to 12 with the pressure at 16 (set 3 to set 2) and SP 12 to 8
    // within set 2 each move the command by one step's integral at set 2's
    // gains, not by KP x 4 mB. (0.16.13 to 0.16.14 kicked by KP x the step,
    // and the bounce as the kick was given back is what Ryan did not want.)
    {
        ctrl_tune_t k = K;
        k.b1 = 6; k.b2 = 14; k.hyst = 0.1f;
        k.sets[0] = (ctrl_pid_t){ 0.14f, 1.5f, 0 };
        k.sets[1] = (ctrl_pid_t){ 0.30f, 0.25f, 0 };
        k.db = k.db_close = 1;
        const float per = 0.14f / 100.0f * (float)k.pos_max;   // set 2's KP, steps per mB
        long kick[2];
        int sets[2][2];
        const float sps[2][2] = { { 16, 12 }, { 12, 8 } };
        for (int f = 0; f < 2; f++) {
            ctrl_t c;
            ctrl_init(&c);
            ctrl_set_sp(&c, sps[f][0], FS, 0);
            ctrl_start(&c, &k, 0, sps[f][0], 30000);
            ctrl_in_t in = { .t_ms = 1000, .pv = sps[f][0], .pos = 30000, .fs = FS };
            ctrl_step(&c, &k, &in);
            sets[f][0] = c.set;
            const int32_t before = c.pos_cmd;
            ctrl_set_sp(&c, sps[f][1], FS, 2000);
            in.t_ms = 2000;
            ctrl_step(&c, &k, &in);
            sets[f][1] = c.set;
            kick[f] = (long)(c.pos_cmd - before);
        }
        const long want = lroundf(-4 * per / (60 * 1.5f));   // this step's integral alone
        CHECK(sets[0][0] == 3 && sets[0][1] == 2 && sets[1][0] == 2 && sets[1][1] == 2 && labs(kick[0] - want) <= 1 &&
              labs(kick[1] - want) <= 1,
              "setpoint step, no kick (P on the pressure only): SP 16 to 12 (set %d to %d) %+ld steps, SP 12 to 8 (set %d to "
              "%d) %+ld, one step's integral at set 2's gains %+ld (the kick was %+ld)", sets[0][0], sets[0][1], kick[0],
              sets[1][0], sets[1][1], kick[1], want, lroundf(-4 * per));
    }
    // The reversal hold scales with KP (0.16.9): 0.2 mB of error at the set's
    // KP, at most 0.10 % of travel. The same 59-step turn round is sent at
    // KP 0.06 (hold 0.012 %, 12 steps) and held at KP 4 (0.10 %, 99 steps).
    {
        long sent[2];
        const float kps[2] = { 0.06f, 4.0f };
        for (int f = 0; f < 2; f++) {
            ctrl_tune_t k = K;
            k.kp = kps[f];
            k.ti = 0;                                       // P alone: the command is KP x e from the start
            k.db = k.db_close = 1;
            ctrl_t c;
            ctrl_init(&c);
            ctrl_set_sp(&c, 5, FS, 0);
            ctrl_start(&c, &k, 0, 5, 30000);
            c.last_dir = 1;                                 // the last move sent was opening
            const float mbar_per_step = 100.0f / (k.kp * (float)k.pos_max);
            ctrl_in_t in = { .t_ms = 1000, .pv = 5 + 59.0f * mbar_per_step, .pos = 30000, .fs = FS };
            ctrl_step(&c, &k, &in);
            sent[f] = c.move ? (long)(c.move_to - 30000) : 0;
        }
        CHECK(sent[0] == -59 && sent[1] == 0, "reversal hold with KP: a 59-step closing turn round sent at KP 0.06 (%ld), "
              "held at KP 4 (%ld)", sent[0], sent[1]);
    }
    // Deadbands (0.15.15): opening moves wait for DB open, closing ones for DB close.
    {
        ctrl_tune_t k = K;
        k.db = 200;
        k.db_close = 8;
        k.kp = 1.0f;          // 990.72 steps/mB
        k.ti = 1.6512f;       // 10 steps/s at 1 mB of error
        ctrl_t c;
        ctrl_init(&c);
        ctrl_set_sp(&c, 6, FS, 0);
        ctrl_start(&c, &k, 0, 5, 30000);
        int first = 0;
        for (int i = 1; i <= 25 && !first; i++) {
            ctrl_in_t in = { .t_ms = (uint32_t)i * 1000u, .pv = 5, .pos = 30000, .fs = FS };
            ctrl_step(&c, &k, &in);
            if (c.move) first = i;
        }
        CHECK(first == 20 && c.move_to - 30000 >= 200 && c.move_to - 30000 < 210,
              "DB open 200: the creep sends nothing for 19 s, then one move of %ld steps at %d s", (long)(c.move_to - 30000), first);
        ctrl_t d;
        ctrl_init(&d);
        ctrl_set_sp(&d, 6, FS, 0);
        ctrl_start(&d, &k, 0, 6, 30000);
        ctrl_in_t in = { .t_ms = 1000, .pv = 6.01f, .pos = 30000, .fs = FS };
        ctrl_step(&d, &k, &in);
        const bool closed_now = d.move && d.move_to < 30000;
        ctrl_init(&d);
        ctrl_set_sp(&d, 6, FS, 0);
        ctrl_start(&d, &k, 0, 6, 30000);
        in.pv = 5.99f;
        ctrl_step(&d, &k, &in);
        CHECK(closed_now && !d.move, "DB close 8: a 10-step closing move goes at once, the same opening is held");
    }

    printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
