/* The bench side of the simulator: what the firmware's pins and sensors
 * see. The cryostat flow line is the simulator v63 model (as ported to
 * tools/ctrl-test/model.c); the needle is turned by whatever the firmware
 * writes to the four Stepper 3 outputs, so step mode, direction and coil
 * order settings act as they do on the bench. Not firmware. */
#include <math.h>
#include <string.h>

#include "bench.h"

bench_t B;

/* ---- noise ---------------------------------------------------------------- */
static uint32_t rng_a = 12345;
static double rnd(void)
{
    rng_a += 0x6D2B79F5u;
    uint32_t t = rng_a;
    t = (t ^ (t >> 15)) * (1u | t);
    t = (t + ((t ^ (t >> 7)) * (61u | t))) ^ t;
    return (double)(t ^ (t >> 14)) / 4294967296.0;
}
static double gauss(void) { return (rnd() + rnd() + rnd() - 1.5) * 2; }
static double clamp(double v, double a, double b) { return v < a ? a : v > b ? b : v; }

/* ---- helium properties (model.c) ------------------------------------------ */
#define RGAS 8.314
#define MHE 4.0026
#define TROOM 295.0
#define KPV (RGAS * TROOM / MHE)
static const double ITS_LO_A[9] = { 1.392408, 0.527153, 0.166756, 0.050988, 0.026514, 0.001975, -0.017976, 0.005409, 0.013259 };
static const double ITS_HI_A[9] = { 3.146631, 1.357655, 0.413923, 0.091159, 0.016349, 0.001826, -0.004325, -0.004973, 0 };
static double itsT(double p)
{
    const double *A = p < 5041.8 ? ITS_LO_A : ITS_HI_A;
    double Bc = p < 5041.8 ? 5.6 : 10.3, C = p < 5041.8 ? 2.9 : 1.9, x = (log(p) - Bc) / C, T = 0, xp = 1;
    for (int i = 0; i < 9; i++) { T += A[i] * xp; xp *= x; }
    return T;
}
#define P125 114.7
#define CC_B 10.8
static double T125;
static double tsat(double p)
{
    if (p <= 0.01) return 0.5;
    if (p < P125) return 1 / (1 / T125 - log(p / P125) / CC_B);
    return itsT(p < 196000 ? p : 196000);
}
#define SVP_MAX 2600
static double svp_lp[SVP_MAX], svp_tt[SVP_MAX];
static int svp_n;
static double psat(double T)
{
    if (T <= svp_tt[0]) return exp(svp_lp[0]);
    if (T >= svp_tt[svp_n - 1]) return exp(svp_lp[svp_n - 1]);
    int lo = 0, hi = svp_n - 1;
    while (hi - lo > 1) { int m = (lo + hi) >> 1; if (svp_tt[m] <= T) lo = m; else hi = m; }
    return exp(svp_lp[lo] + (svp_lp[hi] - svp_lp[lo]) * (T - svp_tt[lo]) / (svp_tt[hi] - svp_tt[lo]));
}
typedef struct { double x, y; } pt_t;
static double interp(const pt_t *tab, int n, double T)
{
    if (T <= tab[0].x) return tab[0].y;
    for (int i = 1; i < n; i++)
        if (T <= tab[i].x) return tab[i - 1].y + (tab[i].y - tab[i - 1].y) * (T - tab[i - 1].x) / (tab[i].x - tab[i - 1].x);
    return tab[n - 1].y;
}
static const pt_t HE_HL[] = { {1.0, 0}, {1.4, 0.04}, {1.6, 0.12}, {1.8, 0.30}, {2.0, 0.70}, {2.17, 1.40}, {2.5, 2.40}, {3.0, 3.80}, {3.5, 5.40}, {4.0, 7.40}, {4.22, 8.55}, {5.0, 12.5} };
static const pt_t HE_L[] = { {1.0, 21.6}, {1.5, 22.6}, {2.17, 23.3}, {3.0, 22.6}, {4.22, 20.7}, {5.0, 17.5} };
static const pt_t HE_RHO[] = { {1.0, 0.145}, {2.17, 0.146}, {3.0, 0.141}, {4.22, 0.125}, {5.0, 0.110} };
#define N(a) (int)(sizeof a / sizeof a[0])
static double hL(double T) { return interp(HE_HL, N(HE_HL), T); }
static double latent(double T) { return interp(HE_L, N(HE_L), T); }
static double rhoL(double T) { return interp(HE_RHO, N(HE_RHO), T); }
static double cL(double T) { double v = (hL(T + 0.02) - hL(T - 0.02)) / 0.04; return v > 0.05 ? v : 0.05; }
#define CP_GAS 5.19

/* ---- flow line (model.c fl_tick) ------------------------------------------ */
static const struct { double bath, Q, Gb, Chw, S, ult, V, seat, bore, gap, Cd, cap, delay, pot, pitch, turns, spt, fnoise, relief; }
    FLP = { 1030, 2, 1, 1, 5, 0.02, 5, 15, 0.5, 3.94, 0.6, 140, 3, 200, 0.5, 6, 16512, 1, 1200 };
#define RING 2000
static double ring[RING];
static int ri, rn;
static double fn_, mEv;

static double needle_flow(double steps, double Ppa)
{
    double lift = clamp(steps, 0, FLP.turns * FLP.spt) / FLP.spt * FLP.pitch;
    double h0 = FLP.seat / 100 * FLP.turns * FLP.pitch;
    double gap = (lift - h0 > 0 ? lift - h0 : 0) * FLP.gap / 1000;
    double An = FLP.Cd * M_PI * FLP.bore * gap;
    double Ac = M_PI * pow(FLP.cap / 1000, 2) / 4;
    double A = An > 0 ? 1 / sqrt(1 / (An * An) + 1 / (Ac * Ac)) : 0;
    double dp = FLP.bath * 100 - Ppa;
    return A * 1e-6 * sqrt(2 * 125 * (dp > 0 ? dp : 0)) * 1000;
}
static double pump_sigma(double Ppa)
{
    if (!B.pump) return 0;
    double f = 1 - FLP.ult * 100 / (Ppa > 1e-9 ? Ppa : 1e-9);
    return FLP.S / 1000 * (f > 0 ? f : 0) / KPV;
}

static void fl_tick(double dt)
{
    double P0 = B.P * 100;
    double m = needle_flow(B.needle_half / 2.0, P0);
    fn_ += -fn_ * dt / 2 + FLP.fnoise / 100 * sqrt(dt) * gauss();
    double mv = B.he ? m * (1 + fn_ > 0 ? 1 + fn_ : 0) : 0;
    int nDel = (int)clamp(round(FLP.delay / dt), 1, RING - 1);
    ring[ri] = mv;
    double mi = rn >= nDel ? ring[(ri - nDel + RING) % RING] : 0;
    ri = (ri + 1) % RING;
    if (rn < RING) rn++;
    double Tb = tsat(FLP.bath * 100), T0 = B.T, Te = T0 < 5.0 ? T0 : 5.0;
    double Ps0 = psat(Te), sl = (psat(Te + 0.005) - psat(Te - 0.005)) / 0.01;
    double L = latent(Te), dh = hL(Tb) - hL(Te);
    double a = (B.mL * cL(Te) + FLP.Chw) / dt, b = FLP.V / 1000 / (KPV * dt), sig = pump_sigma(P0);
    double Tsp = tsat(P0);
    double superheat = B.mL <= 0 && T0 > Tsp ? mEv * CP_GAS * (T0 - Tsp) : 0;
    double E = FLP.Q / 1000 + FLP.Gb / 1000 * (Tb - T0) + mi * dh - superheat;
    double G = 0.01, cap = B.mL / dt + mi;
    double dT = 0, P1 = P0, mev = 0;
    if (cap > 0) {
        double A11 = a + L * G * sl, A12 = -L * G, B1 = E - L * G * Ps0;
        double A21 = -G * sl, A22 = b + G + sig, B2 = b * P0 + G * Ps0;
        double det = A11 * A22 - A12 * A21;
        dT = (B1 * A22 - A12 * B2) / det;
        P1 = (A11 * B2 - A21 * B1) / det;
        mev = G * (Ps0 + sl * dT - P1);
    }
    if (cap <= 0 || mev < 0 || mev > cap) {
        mev = clamp(mev, 0, cap > 0 ? cap : 0);
        dT = (E - L * mev) / a;
        P1 = (b * P0 + mev) / (b + sig);
    }
    B.T = T0 + dT > 0.8 ? T0 + dT : 0.8;
    B.mL += (mi - mev) * dt;
    if (B.mL < 1e-9) B.mL = 0;
    double mmax = FLP.pot * rhoL(Te);
    if (B.mL > mmax) { P1 += (B.mL - mmax) * KPV / (FLP.V / 1000); B.mL = mmax; }
    B.P = clamp(P1, 0, FLP.relief * 100) / 100;
    mEv = mev;
}

/* ---- bench cryostat, fitted to the bench logs ----------------------------- */
// The flow line as the bench logs of 9 to 11 Nov 2026 show it (valve,
// pressure and the Lake Shore's heat exchanger and cold head temperatures,
// 1 s samples; simulator/wasm/fit_bench.py checks the same numbers against
// logs). The numbers are those of the 11 Nov 17:26 step test session.
//
// The pressure heads for
//   floor + F(needle) x G(Thx)
// after a dead time, through a lag. The needle follows a closing valve at
// once and an opening valve 0.30 % of travel behind (play): a small opening
// move after a closing one does nothing. G is 1.33 with the heat exchanger
// at 1.8 K (liquid in it) and 0.63 from 8 K (gas only), so the same opening
// gives twice the pressure wet as dry. The lag is 25 to 70 s with the heat
// exchanger under 3.5 K and 5 s from 5.7 K; it is never more than 2 s at
// 2 mbar and 8 s at 6 mbar.
//
// What sets Thx is the cold head. It warms with the flow (3.5 K at 8 mbar,
// 4.1 K at 14.5, 5.9 K at 29) over minutes. Under 3.72 K all the helium
// reaches the heat exchanger as liquid, over 4.03 K none does, and Thx
// heads for a mix of its two steady values: with liquid, colder the more
// flow (3.5 K at 8 mbar, 1.8 K from 12.5 mbar); with gas, 6 to 14 K.
// So a step up from 12 mbar first sees a wet heat exchanger (the pressure
// climbs past 15 mbar), then the cold head passes 3.72 K, the heat
// exchanger dries and the pressure falls back at the same opening. Coming
// down from 16 mbar the heat exchanger stays dry until the cold head is
// back under 4.03 K; then it refills and the pressure holds up while the
// valve closes. The heat exchanger itself moves in seconds when cold and
// in minutes when warm (its heat capacity grows with temperature).
//
// Two things move from one bench session to the next and are fixed here at
// their 11 Nov afternoon values: the valve's zero (9 Nov was 0.6 % lower,
// 10 Nov 0.2 % lower) and how cold the cold head runs (9 and 10 Nov were
// 0.2 K colder, so the heat exchanger dried later on a step up).
#define BX_N 18
static const double BX_Y[BX_N] = { 0, 1.68, 1.88, 2.28, 2.48, 2.73, 2.98, 3.88, 6.28, 7.28, 8.68, 9.98, 12.78, 19.28, 24.3, 31.28, 49.28, 100 };   // valve %
static const double BX_F[BX_N] = { 0, 0, 0.4, 0.88, 4.76, 7.4, 9, 12.1, 18.8, 21.8, 26.8, 31.1, 40.1, 52.7, 69.2, 75, 79.9, 87.2 };          // mbar with Thx 3.5 K
#define BG_N 7
static const double BG_T[BG_N] = { 1.8, 2.5, 3.5, 5, 6.5, 8, 12 };                 // heat exchanger K
static const double BG_F[BG_N] = { 1.33, 1.09, 1, 0.92, 0.76, 0.63, 0.63 };     // x the pressure
#define BT_N 9
static const double BT_T[BT_N] = { 1.8, 2, 2.75, 3.5, 4.5, 5.7, 7, 12, 30 };     // heat exchanger K
static const double BT_LAG[BT_N] = { 70, 64, 26, 24, 12, 5, 5, 5, 5 };          // s, pressure lag
#define BL_N 6
static const double BL_P[BL_N] = { 1, 2, 4, 6, 8, 12 };                            // mbar
static const double BL_LAG[BL_N] = { 1.5, 2, 4, 8, 30, 1000 };                 // s, the most the lag is
#define BC_N 11
static const double BC_P[BC_N] = { 1, 2, 4, 8, 10, 12, 14.5, 18, 24, 29, 48 };     // mbar
static const double BC_T[BC_N] = { 2.91, 3.02, 3.09, 3.46, 3.56, 3.88, 4.09, 4.21, 4.93, 5.92, 10.88 };   // K, cold head, steady
#define BK_N 5
static const double BK_T[BK_N] = { 3.5, 4, 5, 6, 30 };                             // cold head K
static const double BK_S[BK_N] = { 255, 100, 100, 150, 150 };                      // s, its time constant when cooling
#define BW_N 6
static const double BW_C[BW_N] = { 3.72, 3.78, 3.88, 3.98, 4.03, 30 };             // cold head K
static const double BW_X[BW_N] = { 1, 0.85, 0.66, 0.13, 0, 0 };                    // share of the helium arriving as liquid
#define BH_N 11
static const double BH_P[BH_N] = { 1, 2, 4, 6, 8, 9, 10, 11, 12, 12.5, 60 };       // mbar
static const double BH_T[BH_N] = { 19, 8, 7.13, 4.77, 3.47, 2.92, 2.61, 2.06, 2.05, 1.81, 1.81 };   // K, Thx steady with liquid arriving
#define BD_N 9
static const double BD_P[BD_N] = { 4, 8, 12, 16, 20, 24, 29, 46, 55 };             // mbar
static const double BD_T[BD_N] = { 9, 8, 6.8, 6.2, 5.7, 5.9, 7, 9.8, 13.9 }; // K, Thx steady with gas only
#define BX_FLOOR  0.67     // mbar, valve shut (the pump's floor with the line)
#define BX_DEAD   2.0      // s, dead time
#define BX_PLAY   0.3     // % of travel the needle lags an opening valve by
#define BX_CUP    1.5      // the cold head warms this many times more slowly than it cools
#define BX_HXTAU  12.5     // s, heat exchanger time constant at 2.8 K
#define BX_HXPOW  2.64     // it grows with this power of Thx
#define BX_HXMIN  4      // s, and is never under this
#define BX_HXSTEP 3      // K: the most Thx is pulled by, however far it is from its steady value
#define BX_HX0    3.0      // K at power-on: the cryostat has been running
#define BX_NOISE  0.004    // flow noise, fraction (2 s correlation)
#define BX_WANDER 0.004    // slow wander, fraction (200 s correlation)
#define BX_PUMPOFF 1000.0  // mbar the line fills towards with the pump off (60 s)
#define BX_RING   1000     // 10 s of 10 ms ticks
static double bx_ring[BX_RING];
static int bx_ri;
static double bx_y, bx_fn, bx_wn, bx_c;   // needle %, noise, cold head K

static double table(const double *xs, const double *ys, int n, double x)
{
    if (x <= xs[0]) return ys[0];
    for (int i = 1; i < n; i++)
        if (x <= xs[i]) return ys[i - 1] + (ys[i] - ys[i - 1]) * (x - xs[i - 1]) / (xs[i] - xs[i - 1]);
    return ys[n - 1] + (ys[n - 1] - ys[n - 2]) * (x - xs[n - 1]) / (xs[n - 1] - xs[n - 2]);   // on as the last piece
}
// The needle's opening as the firmware counts valve % (POSMAX 66048 steps,
// 0 % at RETURN 400 steps above the switch): the logs are in these units.
static double bx_valve_pct(void) { return (B.needle_half / 2.0 - 400.0) / 660.48; }
static double bx_target(void)
{
    if (!B.pump) return BX_PUMPOFF;
    double f = B.he && bx_y > 0 ? table(BX_Y, BX_F, BX_N, bx_y) : 0;
    return BX_FLOOR + f * table(BG_T, BG_F, BG_N, B.hx) * (1 + bx_fn + bx_wn);
}
static void bx_fill(double mbar)
{
    for (int i = 0; i < BX_RING; i++) bx_ring[i] = mbar;
    B.P = mbar;
    bx_c = table(BC_P, BC_T, BC_N, mbar);   // the cold head as after a long time at this pressure
}
static void bx_set_hx(double hx)
{
    B.hx = hx;
}
static void bx_reset(double hx)
{
    bx_y = bx_valve_pct();
    bx_fn = bx_wn = 0;
    bx_set_hx(hx);
    bx_ri = 0;
    bx_fill(bx_target());
}
static void bx_tick(double dt)
{
    double x = bx_valve_pct();
    if (x < bx_y) bx_y = x;                              // closing: follows at once
    else if (x - BX_PLAY > bx_y) bx_y = x - BX_PLAY;     // opening: follows behind the play
    bx_fn += -bx_fn * dt / 2 + BX_NOISE * sqrt(dt) * gauss();
    bx_wn += -bx_wn * dt / 200 + BX_WANDER * sqrt(2 * dt / 200) * gauss();
    bx_ring[bx_ri] = bx_target();
    double in = bx_ring[(bx_ri - (int)lround(BX_DEAD / dt) + BX_RING) % BX_RING];
    bx_ri = (bx_ri + 1) % BX_RING;
    double lag = table(BT_T, BT_LAG, BT_N, B.hx), most = table(BL_P, BL_LAG, BL_N, B.P);
    if (most < lag) lag = most;
    if (!B.pump) lag = 60.0;   // the pump off: the line fills
    B.P += (in - B.P) * (1 - exp(-dt / lag));
    if (B.P < 0) B.P = 0;
    // The cold head follows the flow.
    double ce = table(BC_P, BC_T, BC_N, B.P), tc = table(BK_T, BK_S, BK_N, bx_c);
    if (ce > bx_c) tc *= BX_CUP;
    bx_c += (ce - bx_c) * (1 - exp(-dt / tc));
    // The heat exchanger heads for its steady value with the share of
    // liquid the cold head delivers, quickly when cold, slowly when warm.
    double wet = table(BW_C, BW_X, BW_N, bx_c);
    double T = wet * table(BH_P, BH_T, BH_N, B.P) + (1 - wet) * table(BD_P, BD_T, BD_N, B.P);
    double th = BX_HXTAU * pow(B.hx / 2.8, BX_HXPOW);
    if (th < BX_HXMIN) th = BX_HXMIN;
    B.hx += clamp(T - B.hx, -BX_HXSTEP, BX_HXSTEP) * (1 - exp(-dt / th));
}

/* ---- motor and needle ----------------------------------------------------- */
// Half-step sequence as stepper3.c; index = rotor phase.
static const uint8_t seq[8] = { 0x1, 0x3, 0x2, 0x6, 0x4, 0xC, 0x8, 0x9 };
#define SEAT_HALF   (-60)          // the needle seats 30 full steps past the switch
#define TOP_HALF    (2 * 6 * 16512)
#define HYST_HALF   40             // switch releases 20 full steps off the seat side

void bench_coils(uint8_t coils)
{
    // coils: bit n = motor coil n energised, after the firmware's coil order
    // has been undone by the caller (the wiring of the real motor).
    B.coils = coils;
    if (!coils)
        return;   // detent: the rotor stays where it is
    int k = -1;
    for (int i = 0; i < 8; i++)
        if (seq[i] == coils) k = i;
    if (k < 0)
        return;   // not a pattern the rotor follows (three or four coils)
    int d = (k - B.rotor + 8) & 7;
    if (d > 4) d -= 8;
    if (d == 4 || d == -4 || B.supply_v < 8.0) {
        // Opposite coils (wrong order) or no torque: the rotor does not follow.
        if (B.supply_v >= 8.0) B.rotor = k;
        return;
    }
    B.rotor = k;
    // With the factory direction (MOT DIR 0) the firmware opens by stepping
    // the sequence backwards, so a backward rotor step lifts the needle.
    int nd = -d;
    if (!B.coupled) { B.slipped += nd; return; }
    int to = B.needle_half + nd;
    if (to < SEAT_HALF || to > TOP_HALF) { B.stalled++; return; }
    B.needle_half = to;
}

bool bench_limit_pin_high(void)
{
    // NC contact to ground with the pull-up: actuated (open) reads high.
    switch (B.limit_fault) {
    case LIMFAULT_BROKEN: return true;    // broken wire reads as tripped
    case LIMFAULT_STUCK:  return false;   // never trips
    default: break;
    }
    int trip = B.limit_fault == LIMFAULT_EARLY ? B.early_half : 0;
    if (B.needle_half <= trip) B.lim_raw = true;
    else if (B.needle_half >= trip + HYST_HALF) B.lim_raw = false;
    return B.lim_raw;
}

/* ---- Druck and the Power Monitor ------------------------------------------ */
// Signal in mV at 10 V excitation for p mbar: the bench Druck through the
// INA228 input loading (about 5 % low), as model.c.
static double druck_mv(double p)
{
    // The bench model's pressures are what the calibrated bench board read
    // (its logs): its Druck reads true on the nominal table, with no CAL.
    if (B.plant == PLANT_BENCH) return p / 1034.214 * 100.0;
    const double zero = 0.05, span = 100.02, fs = 1034, load = 1 / 1.0512;
    return (zero + p / fs * (span - zero)) * load;
}

void bench_read(int32_t *raw_shunt, int32_t *raw_bus, int16_t *raw_temp)
{
    double exc = B.exc_v > 0 ? B.exc_v + gauss() * 0.0005 : 0.0;
    double p = B.vent ? B.room_mbar : B.P;
    p += (rnd() - 0.5) * 0.01;
    double mv = B.druck ? druck_mv(p) * exc / 10.0 : (rnd() - 0.5) * 0.002;
    mv += gauss() * 0.0003;   // about 0.3 uV of reading noise
    *raw_shunt = (int32_t)lround(mv * 1e6 / 312.5);
    *raw_bus = (int32_t)lround(exc * 1e6 / 195.3125);
    *raw_temp = (int16_t)lround((B.die_c + gauss() * 0.05) / 0.0078125);
}

/* ---- set-up and time ------------------------------------------------------ */
void bench_init(void)
{
    memset(&B, 0, sizeof B);
    T125 = itsT(P125);
    svp_n = 0;
    for (double l = log(0.02); l <= log(196000) + 1e-9 && svp_n < SVP_MAX; l += 0.01) {
        svp_lp[svp_n] = l;
        svp_tt[svp_n++] = tsat(exp(l));
    }
    memset(ring, 0, sizeof ring);
    ri = rn = 0;
    fn_ = mEv = 0;
    B.P = 2.0; B.T = 1.34; B.mL = 10;
    B.plant = PLANT_BENCH;
    B.he = B.pump = B.coupled = B.druck = B.pm = true;
    B.exc_v = 10.051;
    B.room_mbar = 1013.2;
    B.baro = true;
    B.baro_err = 0;
    B.supply_v = 12.1;
    B.die_c = 31.0;
    B.early_half = 2 * 600;
    B.needle_half = 0;
    B.lim_raw = true;
    B.rotor = 1;
    bx_reset(BX_HX0);         // the bench model starts shut, with the heat exchanger cold
}

void bench_set_pressure(double mbar)
{
    if (B.plant == PLANT_BENCH) bx_fill(mbar);
    else B.P = mbar;
}

void bench_set_hx(double k)
{
    if (B.plant == PLANT_BENCH) bx_set_hx(k);
    else B.hx = k;
}

void bench_plant(int plant)
{
    B.plant = plant == PLANT_PHYSICS ? PLANT_PHYSICS : PLANT_BENCH;
    if (B.plant == PLANT_BENCH) bx_reset(BX_HX0);
}

void bench_tick_10ms(void)
{
    if (B.plant == PLANT_BENCH) bx_tick(0.01);
    else fl_tick(0.01);
}
