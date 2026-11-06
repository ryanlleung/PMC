#!/usr/bin/env python3
"""PID autotune for the board's PID table, run from a PC over the text port.

The control test GUI (tools/pmc-gui) runs it from its Gains tab; this file
holds the sequence so it can be tested on a model without a board.

For each test point, low to high (1 2 4 8 12 16 24 48 mbar by default):
  1. Reach: AUTO at the point with safe gains until the pressure holds; the
     valve position it holds at is x0.
  2. Relay test (Astrom and Hagglund, as industrial autotuners do): MANUAL,
     the valve at x0 + d while the pressure is below the point and x0 - d
     while it is above, so the loop oscillates by itself at the frequency
     where the plant lags half a cycle. The oscillation gives the ultimate
     gain Ku (the KP at which AUTO would oscillate) and its period Pu, with
     no curve fit and no settled baseline. d grows when nothing crosses and
     shrinks when the swing is too big.
  3. Gains: KP = Ku / 4 (a margin of about 2 in plant gain before anything
     rings), TI = 2 Pu, KD 0.
  4. Check: AUTO with those gains, setpoint 20 % up and back. Overshoot over
     10 % or more than two reversals: KP x 0.8; no overshoot and slower than
     6 Pu to settle: KP x 1.25. At most two trims.
Then the three PID table sets are the geometric means of the rows in each
band (below B1, B1 to B2, above B2; 6 and 20 mbar by default).

While it runs, the PID table is Off and set 1 carries the gains under test
(PID SET); at the end set 1, the PID table, the setpoint and the mode are put
back as found. Writing the sets to the board (PIDS SET, SCHED SET PV) is a
separate step, after the results have been looked at.

Python 3.8 or later, standard library only. Needs firmware 0.15.8 or later
(PID SET); the sets need 0.15.15 or later (PIDS SET, SCHED SET).
"""
import cmath
import csv
import math
import os

import pmc_gains
from pmc_gains import KP_MIN, KP_MAX, TI_MIN, TI_MAX, Refused, value

POINTS = [1, 2, 4, 8, 12, 16, 24, 48]
B1, B2, HYST = 6.0, 20.0, 15

# Relay test limits.
D_MIN, D_MAX = 0.1, 5.0        # valve half swing, %
AMP_HI, AMP_LO = 0.15, 0.03    # pressure half swing, fraction of the point
AGREE = 0.15                   # periods and swings within this = steady
NO_CROSS_S = 90                # no switch this long: d doubles
RELAY_MAX_S = 720
MAX_HALVES = 16                # 8 full cycles after the restart
D_CHANGES = 4                  # most changes of d in one test
SHIFTS = 3                     # most moves of x0 when nothing crosses
NOISE_S = 40                   # MANUAL at x0 first, for the noise

# Check step.
CHECK_UP = 0.20                # setpoint up by this fraction, then back
SETTLED_S = 60
OVERSHOOT_MAX = 10.0           # % of the step
REVERSALS_MAX = 2
SLOW_PU = 6.0                  # settled later than this many Pu = slow
TRIM_DOWN, TRIM_UP = 0.8, 1.25

# Reaching a point in AUTO.
REACH_MAX_S = 600
CHECK_HOLD_MAX_S = 300


def clip_kp(kp):
    return round(min(max(kp, KP_MIN), KP_MAX), 2)


def clip_ti(ti):
    return round(min(max(ti, TI_MIN), TI_MAX), 2)


def relay_gains(ku, pu_s, div=4.0, ti_pu=2.0):
    """KP (% per mbar) and TI (min) from the ultimate gain and period."""
    return clip_kp(ku / div), clip_ti(ti_pu * pu_s / 60.0)


def harmonic(samples, w):
    """First harmonic (complex amplitude) of [(t, y)] at w rad/s, mean removed."""
    ys = [y for _, y in samples]
    if len(ys) < 4:
        return 0j
    m = sum(ys) / len(ys)
    return 2.0 / len(ys) * sum((y - m) * cmath.exp(-1j * w * t) for t, y in samples)


def noise(ps):
    """The reading's noise, mbar, from one sample to the next, so a slow
    drift (the pressure still settling) does not count."""
    d = [b - a for a, b in zip(ps, ps[1:])]
    if len(d) < 3:
        return 0.0
    m = sum(d) / len(d)
    return math.sqrt(sum((x - m) ** 2 for x in d) / len(d) / 2)


class Relay:
    """The relay test's logic, fed one sample a second; no I/O, so the same
    code runs against the board and against a model in the tests.

    feed(t, p, x) returns the valve % to send now, or None for no change.
    When done is set, result() has Ku and Pu, or why says what went wrong.
    """

    def __init__(self, sp, x0, d, eps):
        self.sp, self.x0, self.d, self.eps = sp, x0, d, eps
        self.hi = None           # True: valve at x0 + d (pressure below the point)
        self.sw = []             # switches since the last restart: (t, hi after)
        self.ext = []            # each completed half since the restart: (hi, extreme)
        self.cur = None          # running extreme of the half in progress
        self.t0 = self.t_last = None
        self.rows = []           # (t, p, x, valve sent)
        self.changes = self.shifts = 0
        self.done, self.steady, self.why, self.note = False, False, '', ''

    def valve(self):
        x = self.x0 + (self.d if self.hi else -self.d)
        return round(min(max(x, 0.0), 100.0), 1)

    def restart(self, t, d, why):
        self.d = round(d, 1)
        self.changes += 1
        self.sw, self.ext, self.cur, self.t_last = [], [], None, t
        self.note = '%s: relay step now %.1f %%' % (why, self.d)
        return self.valve()

    def usable(self):
        """Completed halves after the first cycle: [(length s, hi, extreme)]."""
        hs = [(self.sw[k + 1][0] - self.sw[k][0], self.sw[k][1], self.ext[k][1]) for k in range(len(self.ext))]
        return hs[2:]

    def feed(self, t, p, x):
        if self.done:
            return None
        if self.t0 is None:
            self.t0 = self.t_last = t
        self.rows.append((t, p, x, self.valve() if self.hi is not None else None))
        if p is None:
            return None
        if p > 2 * self.sp + 0.5 or p < 0.4 * self.sp:
            return self.fail('pressure %.2f mbar, too far from %g mbar' % (p, self.sp))
        if self.hi is None:
            self.hi = p < self.sp
            self.rows[-1] = (t, p, x, self.valve())
            return self.valve()
        if self.cur is not None:
            self.cur = min(self.cur, p) if self.hi else max(self.cur, p)
        cmd = None
        if (self.hi and p > self.sp + self.eps) or (not self.hi and p < self.sp - self.eps):
            if self.sw:
                self.ext.append((self.hi, self.cur))
            self.hi = not self.hi
            self.sw.append((t, self.hi))
            self.cur, self.t_last = p, t
            cmd = self.valve()
            cmd = self.assess(t) or cmd
        elif t - self.t_last > NO_CROSS_S:
            # Nothing crossed: x0 is off (AUTO had not quite held), so move
            # it by d the way the pressure needs; after that, a larger d
            # (the needle may not be moving at all).
            if self.shifts < SHIFTS and 0 < self.x0 + (self.d if self.hi else -self.d) < 100:
                self.shifts += 1
                self.x0 = round(self.x0 + (self.d if self.hi else -self.d), 1)
                self.changes -= 1
                cmd = self.restart(t, self.d, 'no crossing in %d s, valve centre moved to %.1f %%' %
                                   (NO_CROSS_S, self.x0))
            elif self.d >= D_MAX or self.changes >= D_CHANGES:
                return self.fail('no crossing in %d s with the relay step at %.1f %%' % (NO_CROSS_S, self.d))
            else:
                cmd = self.restart(t, min(2 * self.d, D_MAX), 'no crossing in %d s' % NO_CROSS_S)
        if not self.done and t - self.t0 >= RELAY_MAX_S:
            if len(self.usable()) >= 2:
                self.done = True
                self.note = 'no steady oscillation in %d min: result from the last cycles' % (RELAY_MAX_S // 60)
            else:
                return self.fail('no oscillation in %d min' % (RELAY_MAX_S // 60))
        return cmd

    def fail(self, why):
        self.done, self.why = True, why
        return None

    def swing(self):
        """Half the last peak-to-trough, mbar (None before one of each)."""
        if len(self.ext) < 2:
            return None
        return abs(self.ext[-1][1] - self.ext[-2][1]) / 2

    def assess(self, t):
        """After each switch: adapt d, or end once the oscillation is steady."""
        a = self.swing()
        if a is not None and a > AMP_HI * self.sp and self.d > D_MIN and self.changes < D_CHANGES:
            return self.restart(t, max(self.d / 2, D_MIN), 'swing %.2f mbar, over %d %% of the point' %
                                (a, AMP_HI * 100))
        if (a is not None and len(self.ext) >= 4 and a < AMP_LO * self.sp and self.d < D_MAX
                and self.changes < D_CHANGES):
            return self.restart(t, min(2 * self.d, D_MAX), 'swing %.3f mbar, under %d %% of the point' %
                                (a, AMP_LO * 100))
        hs = self.usable()
        if len(hs) >= 4:
            last = hs[-4:]
            per = [last[k][0] + last[k + 1][0] for k in range(3)]
            spans = [abs(last[0][2] - last[1][2]), abs(last[2][2] - last[3][2])]
            if (max(per) - min(per) <= AGREE * sum(per) / 3
                    and max(spans) - min(spans) <= AGREE * sum(spans) / 2):
                self.done = self.steady = True
                return None
        if len(hs) >= MAX_HALVES:
            self.done = True
            self.note = 'not steady after %d cycles: result from the last cycles' % (MAX_HALVES // 2)
        return None

    def result(self):
        """Ku (% per mbar), Pu (s) and the rest, from the last whole cycles."""
        hs = self.usable()
        m = min(len(hs) // 2 * 2, 8)
        if m < 2:
            return None
        j = len(self.sw) - 1 - m
        ta, tb = self.sw[j][0], self.sw[-1][0]
        pu = (tb - ta) / (m / 2)
        win = hs[-m:]
        peaks = [e for _, hi, e in win if not hi]
        troughs = [e for _, hi, e in win if hi]
        a = (sum(peaks) / len(peaks) - sum(troughs) / len(troughs)) / 2
        ku_relay = 4 * self.d / (math.pi * math.sqrt(max(a * a - self.eps * self.eps, (a / 2) ** 2)))
        # The same ratio from the valve's actual motion (its travel time, the
        # 0 and 100 % ends): first harmonic of valve over that of pressure.
        w = 2 * math.pi / pu
        seg = [r for r in self.rows if ta <= r[0] < tb and r[1] is not None and r[2] is not None]
        hp = harmonic([(r[0], r[1]) for r in seg], w)
        hx = harmonic([(r[0], r[2]) for r in seg], w)
        ku = abs(hx) / abs(hp) if abs(hp) > 1e-9 and abs(hx) > 1e-9 else ku_relay
        hi_t = [h for h, hi, _ in win if hi]
        lo_t = [h for h, hi, _ in win if not hi]
        return {'Ku': ku, 'Ku_relay': ku_relay, 'Pu_s': pu, 'a_mbar': a, 'd_pct': self.d, 'eps_mbar': self.eps,
                'cycles': m // 2, 'steady': self.steady,
                'asym': (sum(hi_t) / len(hi_t)) / (sum(lo_t) / len(lo_t)) if hi_t and lo_t else None}


def step_figures(rows, p0, p1, band):
    """rows [(s from the change, p, x)]: overshoot %, settled at (s, None if
    not), error reversals after the first crossing, and whether the valve
    sat at an end (then a slow step is not the gains' fault)."""
    step = p1 - p0
    ps = [(t, p) for t, p, _ in rows if p is not None]
    if not ps or abs(step) < 1e-6:
        return None
    peak = max((p - p0) / step for _, p in ps)
    settle = None
    for t, p in ps:
        if abs(p - p1) > band:
            settle = None
        elif settle is None:
            settle = t
    if settle is not None and ps[-1][0] - settle < SETTLED_S:
        settle = None
    rev, side, crossed = 0, None, False
    for _, p in ps:
        e = (p - p1) / step
        if not crossed:
            crossed = e >= 0
            side = 1 if step > 0 else -1     # beyond the new setpoint
            continue
        s = 1 if p - p1 > band else -1 if p - p1 < -band else None
        if s is not None and s != side:
            rev += 1
            side = s
    xs = [x for _, _, x in rows if x is not None]
    return {'overshoot_pct': round(max(0.0, (peak - 1) * 100), 1), 'settle_s': settle, 'reversals': rev,
            'saturated': bool(xs) and (max(xs) >= 99.5 or min(xs) <= 0.05)}


def trim_rule(fig, pu_s):
    """KP factor from a check: (factor, why); 1.0 keeps KP."""
    if fig is None:
        return 1.0, ''
    if fig['overshoot_pct'] > OVERSHOOT_MAX or fig['reversals'] > REVERSALS_MAX:
        return TRIM_DOWN, 'overshoot %.0f %%, %d reversals' % (fig['overshoot_pct'], fig['reversals'])
    slow = fig['settle_s'] is None or fig['settle_s'] > SLOW_PU * pu_s
    if fig['overshoot_pct'] < 1.0 and slow and not fig['saturated']:
        return TRIM_UP, 'no overshoot and %s' % ('not settled' if fig['settle_s'] is None else
                                                 'settled in %.0f s (over %d Pu)' % (fig['settle_s'], SLOW_PU))
    return 1.0, ''


def worse(a, b):
    """The two legs of a check as one: the worse of each figure."""
    if a is None or b is None:
        return a or b
    s = None if a['settle_s'] is None or b['settle_s'] is None else max(a['settle_s'], b['settle_s'])
    return {'overshoot_pct': max(a['overshoot_pct'], b['overshoot_pct']), 'settle_s': s,
            'reversals': max(a['reversals'], b['reversals']), 'saturated': a['saturated'] or b['saturated']}


def geomean(xs):
    return math.exp(sum(math.log(x) for x in xs) / len(xs))


def table_sets(rows, b1=B1, b2=B2):
    """The three PID table sets from the rows: [(set, kp, ti, [points])],
    kp and ti None where no point fell in that band."""
    bands = [lambda s: s < b1, lambda s: b1 <= s < b2, lambda s: s >= b2]
    out = []
    for n, inside in enumerate(bands, 1):
        rs = [r for r in rows if inside(r['sp_mbar'])]
        if rs:
            out.append((n, clip_kp(geomean([r['kp'] for r in rs])), clip_ti(geomean([r['ti_min'] for r in rs])),
                        [r['sp_mbar'] for r in rs]))
        else:
            out.append((n, None, None, []))
    return out


def write_lines(sets, b1=B1, b2=B2, h=HYST):
    """The text-port lines that put the sets and the PID table on the board."""
    lines = ['PIDS SET %d,%.2f,%.2f,0' % (n, kp, ti) for n, kp, ti, _ in sets if kp is not None]
    lines.append('SCHED SET PV,%.1f,%.1f,%d' % (b1, b2, h))
    return lines


class AutoTuner(pmc_gains.Tuner):
    """The sequence on the board, one point at a time. a: div (KP = Ku /
    div), ti_pu (TI = ti_pu Pu), trims, d (first relay step %)."""

    def stage(self, text, phase=None, most=None):
        self.say(text)

    def left(self, want, mode):
        return 'the board left %s (now %s)' % (want, mode or 'no mode')

    def still(self):
        """Until the valve has stopped: in MANUAL the board refuses a
        settings save while a move runs (ERR 212)."""
        t0 = self.clock.now()
        while self.link.ask('VALV MOVING?')[0].endswith(' 1') and self.clock.now() - t0 < 60:
            self.clock.wait_until(self.clock.now() + 0.5)

    def auto(self):
        self.still()
        self.link.do('MODE AUTO')

    def pid(self, kp, ti):
        if self.sample()[2] != 'AUTO':
            self.still()
        self.link.do('PID SET %.2f,%.2f,0' % (kp, ti))

    def hold(self, sp, most):
        """AUTO at sp until the minute's mean is on the point and level.
        Returns (valve % it holds at, held, the last 30 s mean pressure)."""
        tol = max(0.05 * sp, 0.1)

        def held(rows):
            if rows[-1][3] != 'AUTO':
                raise RuntimeError(self.left('AUTO', rows[-1][3]))
            t = rows[-1][0]
            if t < 60:
                return False
            a = [r[1] for r in rows if r[0] > t - 30]
            b = [r[1] for r in rows if t - 60 < r[0] <= t - 30]
            if not a or not b:
                return False
            ma, mb = sum(a) / len(a), sum(b) / len(b)
            return abs((ma + mb) / 2 - sp) < tol and abs(ma - mb) < tol / 2
        rows = self.samples(most, held)
        if not rows:
            raise RuntimeError('no pressure reading')
        ok = held(rows)
        span = 60 if ok else 30      # not held: where it was last, nearest to where it would hold
        xs = [r[2] for r in rows if r[0] >= rows[-1][0] - span and r[2] is not None]
        ps = [r[1] for r in rows if r[0] >= rows[-1][0] - 30]
        return (round(sum(xs) / len(xs), 1) if xs else None), ok, sum(ps) / len(ps)

    def reach(self, sp, kp, ti):
        self.pid(kp, ti)
        self.link.do('PRES SP %.1f' % sp)
        if self.sample()[2] != 'AUTO':
            self.auto()
        self.stage('AUTO at %g mbar with KP %.2f TI %.2f, waiting for it to hold' % (sp, kp, ti),
                   '%g mbar reach' % sp, REACH_MAX_S)
        x0, ok, p = self.hold(sp, REACH_MAX_S)
        if x0 is None:
            raise RuntimeError('no valve reading')
        if not ok and abs(p - sp) > 0.3 * sp:
            raise RuntimeError('AUTO did not reach %g mbar in %d min (pressure %.2f mbar, valve %.1f %%): '
                               'out of reach, or the gains too low' % (sp, REACH_MAX_S // 60, p, x0))
        self.say('%g mbar %s, valve %.1f %%' % (sp, 'held' if ok else 'not held in %d min (mean valve used)' %
                                                 (REACH_MAX_S // 60), x0))
        return x0

    def relay(self, sp, x0, d):
        self.link.do('MODE MANUAL')
        self.link.do('VALV SET %.1f' % x0)
        self.stage('MANUAL at %.1f %%, %d s for the noise' % (x0, NOISE_S), '%g mbar noise' % sp, NOISE_S)
        rows = self.samples(NOISE_S)
        sd = noise([r[1] for r in rows])
        eps = max(3 * sd, 0.01 * sp, 0.02)
        R = Relay(sp, x0, d, eps)
        self.stage('relay test: valve %.1f %% +/- %.1f %%, switching at %g +/- %.2f mbar' % (x0, d, sp, eps),
                   '%g mbar relay' % sp, RELAY_MAX_S)
        t0, n, sent = self.clock.now(), 0, None
        try:
            while not R.done:
                n += 1
                self.clock.wait_until(t0 + n)
                p, x, m = self.sample()
                if m != 'MANUAL':
                    raise RuntimeError(self.left('MANUAL', m))
                cmd = R.feed(self.clock.now() - t0, p, x)
                if cmd is not None and cmd != sent:
                    self.link.do('VALV SET %.1f' % cmd)
                    sent = cmd
                if R.note:
                    self.say('%g mbar relay: %s' % (sp, R.note))
                    R.note = ''
        finally:
            if self.log_dir:
                with open(os.path.join(self.log_dir, 'relay_%gmbar.csv' % sp), 'w', newline='') as f:
                    w = csv.writer(f)
                    w.writerow(['t_s', 'p_mbar', 'valve_pct', 'valve_sent_pct'])
                    for r in R.rows:
                        w.writerow(['%.1f' % r[0], '' if r[1] is None else '%.3f' % r[1],
                                    '' if r[2] is None else '%.2f' % r[2], '' if r[3] is None else '%.1f' % r[3]])
        if R.why:
            self.link.do('VALV SET %.1f' % x0)
            raise RuntimeError('relay test: ' + R.why)
        r = R.result()
        if r is None:
            raise RuntimeError('relay test: too few cycles')
        r['x0'], r['noise'] = R.x0, sd
        return r

    def leg(self, sp, p0, p1, band, most, name):
        self.link.do('PRES SP %.1f' % p1)
        t0, n, rows, since = self.clock.now(), 0, [], None
        while True:
            n += 1
            self.clock.wait_until(t0 + n)
            p, x, m = self.sample()
            if m != 'AUTO':
                raise RuntimeError(self.left('AUTO', m))
            t = self.clock.now() - t0
            rows.append((t, p, x))
            if p is not None:
                since = (since if since is not None else t) if abs(p - p1) <= band else None
            if (since is not None and t - since >= SETTLED_S) or t >= most:
                break
        if self.log_dir:
            with open(os.path.join(self.log_dir, '%s.csv' % name), 'w', newline='') as f:
                w = csv.writer(f)
                w.writerow(['t_s', 'p_mbar', 'valve_pct', 'from_mbar', 'to_mbar'])
                for r in rows:
                    w.writerow(['%.1f' % r[0], '' if r[1] is None else '%.3f' % r[1],
                                '' if r[2] is None else '%.2f' % r[2], p0, p1])
        return step_figures(rows, p0, p1, band)

    def check(self, sp, kp, ti, r, k):
        """AUTO with kp, ti; setpoint up 20 % and back. The worse leg's figures."""
        self.pid(kp, ti)
        self.auto()
        self.stage('check %d: AUTO at %g mbar with KP %.2f TI %.2f, waiting for it to hold' % (k, sp, kp, ti),
                   '%g mbar check %d hold' % (sp, k), CHECK_HOLD_MAX_S)
        self.hold(sp, CHECK_HOLD_MAX_S)
        hi = round(max(sp * (1 + CHECK_UP), sp + 0.2), 1)
        band = max(0.05 * (hi - sp), 2 * r['noise'], 0.02)
        most = min(max(12 * r['Pu_s'] + 90, 150), 360)
        legs = []
        for k2, (a, b) in enumerate([(sp, hi), (hi, sp)]):
            self.stage('check %d: setpoint %g -> %g mbar, waiting for it to hold within %.2f mbar for %d s' %
                       (k, a, b, band, SETTLED_S), '%g mbar check %d %s' % (sp, k, 'up' if k2 == 0 else 'back'), most)
            legs.append(self.leg(sp, a, b, band, most, 'check_%gmbar_%d_%s' % (sp, k, 'up' if k2 == 0 else 'back')))
        return worse(*legs)

    def point(self, sp, reach_gains, d):
        """One test point: a result row for the table."""
        x0 = self.reach(sp, *reach_gains)
        r = self.relay(sp, x0, d)
        kp, ti = relay_gains(r['Ku'], r['Pu_s'], self.a.div, self.a.ti_pu)
        self.say('%g mbar: Ku %.2f %%/mbar, Pu %.1f s (relay %.1f %%, swing %.3f mbar, %d cycles%s) -> KP %.2f TI %.2f'
                 % (sp, r['Ku'], r['Pu_s'], r['d_pct'], r['a_mbar'], r['cycles'], '' if r['steady'] else ', not steady',
                    kp, ti))
        kp0, trims, fig, why = kp, [], None, ''
        for k in range(self.a.trims + 1):
            fig = self.check(sp, kp, ti, r, k + 1)
            f, why = trim_rule(fig, r['Pu_s'])
            if fig:
                self.say('%g mbar check %d (KP %.2f): overshoot %.1f %%, %s, %d reversals%s' %
                         (sp, k + 1, kp, fig['overshoot_pct'],
                          'not settled' if fig['settle_s'] is None else 'settled in %.0f s' % fig['settle_s'],
                          fig['reversals'], ', valve at an end' if fig['saturated'] else ''))
            if f == 1.0 or k == self.a.trims:
                break
            kp = clip_kp(kp * f)
            trims.append('x%.2f' % f)
            self.say('%g mbar: %s, KP x %.2f -> %.2f' % (sp, why, f, kp))
        note = ''
        if f != 1.0:
            note = 'still %s after %d trims' % (why, self.a.trims)
        if not r['steady']:
            note = (note + '; ' if note else '') + 'relay not steady'
        fig = fig or {}
        return {'sp_mbar': sp, 'kp': kp, 'ti_min': ti, 'Ku': round(r['Ku'], 3), 'Pu_s': round(r['Pu_s'], 1),
                'kp_rule': kp0, 'd_pct': r['d_pct'], 'a_mbar': round(r['a_mbar'], 3), 'eps_mbar': round(r['eps_mbar'], 3),
                'cycles': r['cycles'], 'asym': None if r['asym'] is None else round(r['asym'], 2), 'valve_pct': x0,
                'trims': ' '.join(trims), 'overshoot_pct': fig.get('overshoot_pct'),
                'settle_s': None if fig.get('settle_s') is None else round(fig['settle_s']),
                'reversals': fig.get('reversals'), 'note': note}


def next_d(sp, ku, first):
    """Relay step for a point from the last point's Ku: a swing of about 6 %."""
    if not ku:
        return first
    return round(min(max(0.06 * sp * ku * math.pi / 4, 0.2), D_MAX), 1)


def found(link):
    """What to put back: setpoint, mode, valve, the PID table and set 1."""
    sp = value(link.ask('PRES SP?')[0], 'PRES SP')
    p, x, mode = link.ask('PRES?;VALV?;MODE?')
    out = {'sp': sp, 'mode': mode.partition(' ')[2], 'valve': value(x, 'VALV'), 'sched': None, 'set1': None}
    pid = link.ask('PID?')[0]
    out['pid'] = [float(v) for v in pid[4:].split(',')]
    try:
        s = link.ask('SCHED?')[0]
        out['sched'] = s[6:]
        g = link.ask('PIDS?')[0][5:].split(',')
        out['set1'] = [float(v) for v in g[1:4]]
        out['sets'] = [[float(v) for v in g[1 + 3 * k:4 + 3 * k]] for k in range(3)]
    except Refused:
        pass                     # firmware before 0.15.15: one set
    return out


def put_back_lines(f, x_last=None):
    """The lines that put the board back as found (each tried on its own)."""
    out = []
    if f.get('sched') is not None:
        out.append('PIDS SET 1,%.2f,%.2f,%.2f' % tuple(f['set1']))
        out.append('SCHED SET ' + f['sched'])
    elif f.get('pid'):
        out.append('PID SET %.2f,%.2f,%.2f' % tuple(f['pid']))
    if f.get('sp') is not None:
        out.append('PRES SP %.1f' % f['sp'])
    if f.get('mode') == 'AUTO':
        out.append('MODE AUTO')
    else:
        out.append('MODE MANUAL')
        if f.get('valve') is not None:
            out.append('VALV SET %.1f' % f['valve'])
    return out


def pid_table_off(link, f):
    """The PID table Off (set 1 in use, so PID SET writes set 1)."""
    if f.get('sched') and not f['sched'].startswith('OFF'):
        link.do('SCHED SET OFF,' + f['sched'].split(',', 1)[1])
