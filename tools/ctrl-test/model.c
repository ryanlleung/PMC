/* C port of the simulator v63 bench side, for the ctrl.c host test only:
 * the cryostat flow line (flTick), the stepper with go-to and retarget
 * (motorTick, startMove), the pretend limit switch with the seat 30 steps
 * past it, and the Druck read through the bench two-point cal (measure).
 * Names follow the simulator. Not firmware. */
#include <math.h>
#include <string.h>

#include "model.h"

/* ---- noise: the simulator's seeded Math.random (mulberry32) --------------- */
static uint32_t rng_a = 12345;
double model_random(void)
{
    rng_a += 0x6D2B79F5u;
    uint32_t t = rng_a;
    t = (t ^ (t >> 15)) * (1u | t);
    t = (t + ((t ^ (t >> 7)) * (61u | t))) ^ t;
    return (double)(t ^ (t >> 14)) / 4294967296.0;
}
static double gauss(void) { return (model_random() + model_random() + model_random() - 1.5) * 2; }
static double noise(double pp) { return (model_random() - 0.5) * pp; }
static double clamp(double v, double a, double b) { return v < a ? a : v > b ? b : v; }
static int sgn(double v) { return v > 0 ? 1 : v < 0 ? -1 : 0; }

/* ---- helium properties ---------------------------------------------------- */
#define RGAS 8.314
#define MHE 4.0026
#define TROOM 295.0
#define KPV (RGAS * TROOM / MHE)
static const double ITS_LO_A[9] = { 1.392408, 0.527153, 0.166756, 0.050988, 0.026514, 0.001975, -0.017976, 0.005409, 0.013259 };
static const double ITS_HI_A[9] = { 3.146631, 1.357655, 0.413923, 0.091159, 0.016349, 0.001826, -0.004325, -0.004973, 0 };
static double itsT(double p)
{
    const double *A = p < 5041.8 ? ITS_LO_A : ITS_HI_A;
    double B = p < 5041.8 ? 5.6 : 10.3, C = p < 5041.8 ? 2.9 : 1.9, x = (log(p) - B) / C, T = 0, xp = 1;
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

/* ---- state ---------------------------------------------------------------- */
model_t M;   // everything the test looks at

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
    if (!M.pump) return 0;
    double f = 1 - FLP.ult * 100 / (Ppa > 1e-9 ? Ppa : 1e-9);
    return FLP.S / 1000 * (f > 0 ? f : 0) / KPV;
}

static void fl_tick(double dt)
{
    double P0 = M.P * 100;
    double m = needle_flow(M.sim_pos, P0);
    fn_ += -fn_ * dt / 2 + FLP.fnoise / 100 * sqrt(dt) * gauss();
    double mv = M.he ? m * (1 + fn_ > 0 ? 1 + fn_ : 0) : 0;
    int nDel = (int)clamp(round(FLP.delay / dt), 1, RING - 1);
    ring[ri] = mv;
    double mi = rn >= nDel ? ring[(ri - nDel + RING) % RING] : 0;
    ri = (ri + 1) % RING;
    if (rn < RING) rn++;
    double Tb = tsat(FLP.bath * 100), T0 = M.T, Te = T0 < 5.0 ? T0 : 5.0;
    double Ps0 = psat(Te), sl = (psat(Te + 0.005) - psat(Te - 0.005)) / 0.01;
    double L = latent(Te), dh = hL(Tb) - hL(Te);
    double a = (M.mL * cL(Te) + FLP.Chw) / dt, b = FLP.V / 1000 / (KPV * dt), sig = pump_sigma(P0);
    double Tsp = tsat(P0);
    double superheat = M.mL <= 0 && T0 > Tsp ? mEv * CP_GAS * (T0 - Tsp) : 0;
    double E = FLP.Q / 1000 + FLP.Gb / 1000 * (Tb - T0) + mi * dh - superheat;
    double G = 0.01, cap = M.mL / dt + mi;
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
    M.T = T0 + dT > 0.8 ? T0 + dT : 0.8;
    M.mL += (mi - mev) * dt;
    if (M.mL < 1e-9) M.mL = 0;
    double mmax = FLP.pot * rhoL(Te);
    if (M.mL > mmax) { P1 += (M.mL - mmax) * KPV / (FLP.V / 1000); M.mL = mmax; }
    M.P = clamp(P1, 0, FLP.relief * 100) / 100;
    mEv = mev;
}

/* ---- limit switch and stepper --------------------------------------------- */
#define HYST 20
static void update_raw(void)
{
    bool raw = M.sim_pos <= 0 ? true : M.sim_pos >= HYST ? false : M.lim_raw;
    M.lim_raw = raw;
    if (raw) { M.lim_active = true; M.clear_on = false; }
    else if (M.lim_active && !M.clear_on) { M.clear_on = true; M.clear_since = M.t; }
}
static void debounce_tick(void)
{
    update_raw();
    if (!M.lim_raw && M.lim_active && M.clear_on && M.t - M.clear_since >= 0.1) { M.lim_active = false; M.clear_on = false; }
}
static void finish(const char *why)
{
    M.moving = false; M.v = 0; M.acc = 0;
    M.last_stop = why;
}
static bool do_step(void)
{
    if (M.dir < 0 && M.lim_active) { finish("LIMIT"); return false; }
    M.pos += M.dir;
    if (M.coupled) {
        int ph = M.dir;
        if (ph > 0 || M.sim_pos > -30) M.sim_pos += ph;
    }
    update_raw();
    return true;
}
static void motor_tick(double dt)
{
    if (!M.moving) return;
    int want = sgn((double)(M.target - M.pos));
    double a = M.accel;
    if (want != M.dir) {
        M.v -= a * dt;
        if (M.v <= M.vstart) { M.dir = want; M.v = M.vstart; M.acc = 0; if (want == 0) { finish("DONE"); return; } }
    } else {
        double rem = fabs((double)(M.target - M.pos)), dstop = (M.v * M.v - M.vstart * M.vstart) / (2 * a);
        M.v = rem <= dstop + 1 ? (M.v - a * dt > M.vstart ? M.v - a * dt : M.vstart) : (M.v + a * dt < M.vrun ? M.v + a * dt : M.vrun);
    }
    M.acc += M.v * dt;
    while (M.acc >= 1 && M.moving) {
        M.acc -= 1;
        if (!do_step()) return;
        if (M.pos == M.target) { finish("DONE"); return; }
    }
}
bool model_start_move(int32_t target)
{
    M.vrun = M.run;
    M.target = target;
    if (!M.moving) {
        if (target == M.pos) return true;
        if (target < M.pos && M.lim_active) { M.last_stop = "LIMIT"; return false; }
        M.moving = true; M.dir = sgn((double)(target - M.pos));
        M.vstart = M.start < M.vrun ? M.start : M.vrun; M.v = M.vstart; M.acc = 0;
    }
    return true;
}
void model_stop(void)
{
    if (M.moving) { M.target = M.pos; finish("STOP"); }
}

/* ---- the Druck through the bench two-point cal ---------------------------- */
static double druck_mv(double p)
{
    const double zero = 0.05, span = 100.02, fs = 1034, load = 1 / 1.0512;
    return (zero + p / fs * (span - zero)) * load;
}
static double cal_p(double mv)
{
    const double r0 = 0.004756, p0 = 0, r1 = 9.323535, p1 = 1013.2, x = mv / 10;
    return p0 + (x - r0) * (p1 - p0) / (r1 - r0);
}
static void measure(void)
{
    double p_true = M.P + noise(0.01);
    M.pv = cal_p(druck_mv(p_true));
}

/* ---- set-up and time ------------------------------------------------------ */
void model_init(void)
{
    memset(&M, 0, sizeof M);
    rng_a = 12345;
    T125 = itsT(P125);
    svp_n = 0;
    for (double l = log(0.02); l <= log(196000) + 1e-9 && svp_n < SVP_MAX; l += 0.01) {
        svp_lp[svp_n] = l;
        svp_tt[svp_n++] = tsat(exp(l));
    }
    memset(ring, 0, sizeof ring);
    ri = rn = 0;
    fn_ = mEv = 0;
    M.P = 2.0; M.T = 1.34; M.mL = 10; M.he = true; M.pump = true; M.coupled = true;
    M.run = 400; M.start = 200; M.accel = 3200;
    M.pos_max = 99072;
    // bootHomed(): homed with the valve closed on the switch
    M.sim_pos = 0; M.lim_raw = true; M.lim_active = true;
    M.pos = 0;
    M.next_meas = 1;
    M.pv = 0;
}

// One simulator tick (DT = 0.01 s); true when a 1 s pressure read happened.
bool model_tick(void)
{
    M.t += MODEL_DT;
    motor_tick(MODEL_DT);
    debounce_tick();
    fl_tick(MODEL_DT);
    if (M.t >= M.next_meas) {
        M.next_meas += 1;
        measure();
        return true;
    }
    return false;
}
