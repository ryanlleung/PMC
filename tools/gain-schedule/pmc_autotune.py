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
     6 Pu to settle: KP x 1.25. At most two trims up, and none after a trim
     down; trims down go on until the check passes (at most MAX_CUTS).
Whenever AUTO runs (reaching a point, holding it, the check steps), a
watchdog looks for a sustained oscillation: two full cycles in a row (four
swings), each swing more than 30 % of the setpoint. It then cuts KP at once (x 0.5, or
x 0.25 when the swing is larger than the setpoint itself) with PID SET and
carries on with the lower gain (Ryan, 9 Nov: "make sure it tunes down the
gains when it oscillates like crazy"; at 2 mbar the 1 mbar gains swung the
pressure 1 to 10 mbar every 14 s).
Then the three PID table sets are the geometric means of the rows in each
band (below B1, B1 to B2, above B2; 6 and 20 mbar by default).

Learned gains (Ryan, 9 Nov: "it should refine the numbers rather than
starting fresh every time"): each point's result is kept in
autotune-learned.csv in the log folder. On the next run a point with a
learned row reaches its setpoint with its own learned gains, sizes the relay
step from its learned Ku, and starts the check from its learned KP and TI
rather than Ku / 4 and 2 Pu, so the trims carry on from run to run. If the
new relay test's Ku is more than LEARNED_AGREE times off the learned one the
plant has changed, and that point starts fresh. Fresh ignores the file (its
results still go in it). With no learned file yet, it is built from the
results.csv of the earlier autotune runs in the same log folder (newest per
setpoint), so runs made before learning existed count too. The gains worked
out by hand from the bench logs (autotune-analysed.csv beside this file) go
in once, over any older learned row (merge_analysed).

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
import time

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
WANDER_MAX = 0.25              # the wander allowed for in a check: at most this fraction of the step
USELESS = 0.9                  # a cut whose check is not 10 % better did not help; two such end the cuts
RUNOFF_S = 120                 # after a run-off, how long the pressure has to come back

# Reaching a point in AUTO.
REACH_MAX_S = 600
CHECK_HOLD_MAX_S = 300

# The oscillation watchdog in AUTO.
WILD_PP = 0.30                 # a full cycle swinging more than this fraction of the setpoint...
WILD_PP_MIN = 0.2              # ...and at least this, mbar
WILD_CYCLES = 2                # ...so many cycles in a row: oscillating
WILD_SPAN_S = 400              # ...all within this long
WILD_CUT, WILD_CUT_BIG = 0.5, 0.25   # KP x this; x BIG when the swing is larger than the setpoint
LEARNED_FILE = 'autotune-learned.csv'
LEARNED_COLS = ['sp_mbar', 'kp', 'ti_min', 'Ku', 'Pu_s', 'd_pct', 'a_mbar', 'valve_pct', 'trims', 'note', 'when']
LEARNED_AGREE = 2.0            # learned Ku within this factor of the new relay's: refine from it
# Gains worked out from the bench logs, merged into the learned file (merge_analysed).
ANALYSED_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'autotune-analysed.csv')
MAX_CUTS = 6                   # most KP cuts at one point (watchdog and check trims down together)


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


def wander(rows):
    """The pressure's own swing with the valve held, mbar: half its
    peak-to-peak about a straight line through [(t, p)], so a slow drift
    (the pressure still settling) does not count. At 4 mbar on 9 Nov the
    pressure wandered 0.3 mbar peak to peak with the valve standing still."""
    tp = [(t, p) for t, p in rows if p is not None]
    if len(tp) < 3:
        return 0.0
    n = len(tp)
    mt, mp = sum(t for t, _ in tp) / n, sum(p for _, p in tp) / n
    vt = sum((t - mt) ** 2 for t, _ in tp)
    b = sum((t - mt) * (p - mp) for t, p in tp) / vt if vt else 0.0
    res = [p - mp - b * (t - mt) for t, p in tp]
    return (max(res) - min(res)) / 2


def badness(fig):
    """How far a check is from passing on overshoot and reversals: 1 = at the limit."""
    if not fig or fig.get('wild_pp'):
        return float('inf')
    return max(fig['overshoot_pct'] / OVERSHOOT_MAX, fig['reversals'] / REVERSALS_MAX)


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
        self.off = None          # when the pressure ran off and the step was cut; None once back
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
            # Too far: a smaller step (a steep valve, Ryan 9 Nov: 0.4 % moved
            # 2 mbar by 7), then time for the pressure to come back before
            # it is judged again (9 Nov: at 2 mbar it was still 5 mbar the
            # next second, and the test gave up 13 s in). Only when the step
            # is already the least, or it does not come back, give up.
            if self.off is not None:
                if t - self.off > RUNOFF_S:
                    return self.fail('pressure %.2f mbar, not back near %g mbar %d s after the relay step was cut'
                                     % (p, self.sp, RUNOFF_S))
                return None
            if self.d <= D_MIN or self.changes >= D_CHANGES + 2:
                return self.fail('pressure %.2f mbar, too far from %g mbar' % (p, self.sp))
            self.hi, self.off = p < self.sp, t
            return self.restart(t, max(self.d / 4, D_MIN), 'pressure %.2f mbar, too far from %g mbar' % (p, self.sp))
        if self.off is not None:     # back: the test starts from here
            self.off, self.t_last = None, t
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

    def live(self):
        """What the relay has so far, in words, for the page."""
        hs = self.usable()
        a = self.swing()
        txt = 'valve %.1f %% +/- %.1f %%, switching at %g +/- %.2f mbar; %d switches' % (
            self.x0, self.d, self.sp, self.eps, len(self.sw))
        if a is not None:
            txt += ', last swing +/- %.3f mbar (keep between %.3f and %.3f)' % (a, AMP_LO * self.sp, AMP_HI * self.sp)
        if len(hs) >= 2:
            txt += ', last period %.1f s' % (hs[-1][0] + hs[-2][0])
        return txt

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
                'hx_pct': abs(hx), 'hp_mbar': abs(hp), 'harmonic': ku is not ku_relay,
                'cycles': m // 2, 'steady': self.steady,
                'asym': (sum(hi_t) / len(hi_t)) / (sum(lo_t) / len(lo_t)) if hi_t and lo_t else None}


class Wild:
    """The watchdog: a sustained oscillation in AUTO, fed one reading a
    second. The pressure's turns are taken as a zigzag: a turn counts once
    the pressure has come back from it by more than WILD_PP of the setpoint
    (at least WILD_PP_MIN), so the reading's noise and the needle's small
    limit cycle make none. feed(p) turns True at the 2 x WILD_CYCLES-th
    such swing in a row, all within WILD_SPAN_S. It does not need the
    setpoint to be crossed: a swing held off to one side (the pump's floor
    under a low setpoint, or windup) counts the same. A step's overshoot
    and ring-down make one or two swings at most."""

    def __init__(self, sp):
        self.sp = sp
        self.big = max(WILD_PP * sp, WILD_PP_MIN)
        self.t, self.dir = 0, 0
        self.lo = self.hi = None     # before the first turn: the range so far, (p, t)
        self.ext = None              # the running extreme of the swing under way, (p, t)
        self.turns = []              # (p, t) of each turn
        self.pp = 0.0                # the smallest swing of those that tripped it, mbar
        self.wild = False

    def feed(self, p):
        self.t += 1
        if p is None or self.wild:
            return self.wild
        t, big = self.t, self.big
        if self.dir == 0:
            self.lo = min(self.lo or (p, t), (p, t))
            self.hi = max(self.hi or (p, t), (p, t))
            if p >= self.lo[0] + big:
                self.turns, self.dir, self.ext = [self.lo], 1, (p, t)
            elif p <= self.hi[0] - big:
                self.turns, self.dir, self.ext = [self.hi], -1, (p, t)
            return False
        if (p - self.ext[0]) * self.dir > 0:
            self.ext = (p, t)
        elif (self.ext[0] - p) * self.dir >= big:
            self.turns = (self.turns + [self.ext])[-2 * WILD_CYCLES - 1:]
            self.dir, self.ext = -self.dir, (p, t)
        pts = self.turns + ([self.ext] if abs(self.ext[0] - self.turns[-1][0]) >= big else [])
        pts = pts[-2 * WILD_CYCLES - 1:]
        if len(pts) == 2 * WILD_CYCLES + 1 and t - pts[0][1] <= WILD_SPAN_S:
            self.wild = True
            # the size from the finished swings (the one under way may have only just passed `big`)
            done = [abs(a[0] - b[0]) for a, b in zip(self.turns, self.turns[1:])][-(2 * WILD_CYCLES - 1):]
            self.pp = min(done) if done else big
        return self.wild

    def status(self):
        """Swings in a row so far that count (each over `big`), for the page."""
        if not self.turns:
            return 0
        n = len(self.turns) - 1 + (abs(self.ext[0] - self.turns[-1][0]) >= self.big)
        return n

    def cut(self):
        """The KP factor for this oscillation."""
        return WILD_CUT_BIG if self.pp > self.sp else WILD_CUT


def step_figures(rows, p0, p1, band, slack=0.0):
    """rows [(s from the change, p, x)]: overshoot %, settled at (s, None if
    not), error reversals after the first crossing, and whether the valve
    sat at an end (then a slow step is not the gains' fault). Overshoot
    counts only beyond slack mbar (the pressure's own wander)."""
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
    over = max(0.0, (peak - 1) * abs(step) - slack)
    return {'overshoot_pct': round(over / abs(step) * 100, 1), 'settle_s': settle, 'reversals': rev,
            'saturated': bool(xs) and (max(xs) >= 99.5 or min(xs) <= 0.05)}


def trim_rule(fig, pu_s):
    """KP factor from a check: (factor, why); 1.0 keeps KP."""
    if fig is None:
        return 1.0, ''
    if fig.get('wild_pp'):
        return (WILD_CUT_BIG if fig.get('wild_big') else WILD_CUT,
                'oscillating (swing %.2f mbar)' % fig['wild_pp'])
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
    out = {'overshoot_pct': max(a['overshoot_pct'], b['overshoot_pct']), 'settle_s': s,
           'reversals': max(a['reversals'], b['reversals']), 'saturated': a['saturated'] or b['saturated']}
    w = max((x for x in (a, b) if x.get('wild_pp')), key=lambda x: x['wild_pp'], default=None)
    if w:
        out['wild_pp'], out['wild_big'] = w['wild_pp'], w.get('wild_big', False)
    return out


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

    # The working, for the page: the lines of this setpoint's calculations
    # (calc) and one line that changes while a stage runs (now).
    calcs, calc_now = [], ''

    def calc(self, text):
        self.calcs = (self.calcs + [text])[-400:]
        self.show()

    def now(self, text):
        self.calc_now = text
        self.show()

    def show(self):
        """The GUI shows calcs and calc_now; nothing here."""

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

    def calm(self, sp, kp, ti, w):
        """The watchdog tripped: KP cut at once (in AUTO), the new KP."""
        new = clip_kp(kp * w.cut())
        if new >= kp:
            raise RuntimeError('AUTO oscillating at %g mbar (swing %.2f mbar) with KP already at %.2f'
                               % (sp, w.pp, kp))
        self.say('%g mbar: AUTO oscillating (swing %.2f mbar), KP x %.2f -> %.2f' % (sp, w.pp, w.cut(), new))
        self.calc('Oscillating: %d swings in a row, the smallest %.2f mbar, each over %.2f mbar (%d %% of %g mbar). '
                  'KP x %.2f%s: %.2f x %.2f = %.2f (PID SET, AUTO carries on)' % (
                      2 * WILD_CYCLES, w.pp, w.big, WILD_PP * 100, sp, w.cut(),
                      ', the swing larger than the setpoint' if w.cut() == WILD_CUT_BIG else '', kp, w.cut(), new))
        self.pid(new, ti)
        self.cuts.append('x%.2f osc' % w.cut())
        return new

    def hold(self, sp, most, kp, ti, pre='', phase=None):
        """AUTO at sp until the minute's mean is on the point and level,
        cutting KP while it oscillates (each cut starts the wait again,
        and the stage line with it: the new KP and its own `most`).
        Returns (valve % it holds at, held, the last 30 s mean pressure,
        the KP in use)."""
        tol = max(0.05 * sp, 0.1)
        kp0 = kp

        def level(rows):
            t = rows[-1][0]
            if t < 60:
                return False
            a = [r[1] for r in rows if r[0] > t - 30]
            b = [r[1] for r in rows if t - 60 < r[0] <= t - 30]
            if not a or not b:
                return False
            ma, mb = sum(a) / len(a), sum(b) / len(b)
            return abs((ma + mb) / 2 - sp) < tol and abs(ma - mb) < tol / 2
        for n in range(MAX_CUTS + 1):
            self.stage('%sAUTO at %g mbar with KP %.2f TI %.2f%s, waiting for it to hold' % (
                pre, sp, kp, ti, ' (cut from %.2f)' % kp0 if kp < kp0 else ''), phase if n == 0 else None, most)
            w = Wild(sp)

            def held(rows):
                if rows[-1][3] != 'AUTO':
                    raise RuntimeError(self.left('AUTO', rows[-1][3]))
                if w.feed(rows[-1][1]):
                    return True
                self.now('AUTO at %g mbar, KP %.2f TI %.2f, %.0f s: pressure %.3f mbar; watchdog %d of %d swings over '
                         '%.2f mbar' % (sp, kp, ti, rows[-1][0], rows[-1][1], w.status(), 2 * WILD_CYCLES, w.big))
                return level(rows)
            rows = self.samples(most, held)
            if not w.wild:
                break
            kp = self.calm(sp, kp, ti, w)
        else:
            raise RuntimeError('AUTO still oscillating at %g mbar after %d cuts' % (sp, MAX_CUTS))
        if not rows:
            raise RuntimeError('no pressure reading')
        ok = level(rows)
        span = 60 if ok else 30      # not held: where it was last, nearest to where it would hold
        xs = [r[2] for r in rows if r[0] >= rows[-1][0] - span and r[2] is not None]
        ps = [r[1] for r in rows if r[0] >= rows[-1][0] - 30]
        x0 = round(sum(xs) / len(xs), 1) if xs else None
        self.calc('%s at %g mbar: x0 = %s %% (mean valve, last %d s), pressure %.3f mbar (mean, last 30 s)' % (
            'Held' if ok else 'Not held', sp, '--' if x0 is None else '%.1f' % x0, span, sum(ps) / len(ps)))
        return x0, ok, sum(ps) / len(ps), kp

    def reach(self, sp, kp, ti, why=None):
        self.calc('Reach %g mbar: AUTO with KP %.2f TI %.2f (%s)' % (
            sp, kp, ti, why or 'the setpoint before\'s gains, or the first gains'))
        self.pid(kp, ti)
        self.link.do('PRES SP %.1f' % sp)
        if self.sample()[2] != 'AUTO':
            self.auto()
        x0, ok, p, _ = self.hold(sp, REACH_MAX_S, kp, ti, '', '%g mbar reach' % sp)
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
        wd = wander([(r[0], r[1]) for r in rows])
        eps = max(3 * sd, 0.01 * sp, 0.02)
        self.calc('Noise: sigma = %.4f mbar (sample to sample, %d s in MANUAL at x0). Switch band eps = '
                  'max(3 sigma, 1 %% of %g, 0.02) = %.3f mbar' % (sd, NOISE_S, sp, eps))
        self.calc('Wander: the pressure swings +/- %.3f mbar on its own with the valve held (drift removed)' % wd)
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
                    self.calc('Relay: ' + R.note)
                    R.note = ''
                self.now('Relay, %d s: %s' % (n, R.live()))
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
        r['x0'], r['noise'], r['wander'] = R.x0, sd, wd
        self.calc('Relay result over %d cycles: period Pu = %.1f s, pressure swing a = +/- %.3f mbar, relay step d = %.1f %%' %
                  (r['cycles'], r['Pu_s'], r['a_mbar'], r['d_pct']))
        if r.get('harmonic'):
            self.calc('Ku = valve swing / pressure swing at Pu (first harmonics) = %.3f %% / %.3f mbar = %.3f %%/mbar '
                      '(relay formula 4d / (pi sqrt(a^2 - eps^2)) gives %.3f)' %
                      (r['hx_pct'], r['hp_mbar'], r['Ku'], r['Ku_relay']))
        else:
            self.calc('Ku = 4d / (pi sqrt(a^2 - eps^2)) = 4 x %.1f / (pi x sqrt(%.3f^2 - %.3f^2)) = %.3f %%/mbar' %
                      (r['d_pct'], r['a_mbar'], r['eps_mbar'], r['Ku']))
        return r

    def leg(self, sp, p0, p1, band, most, name, slack=0.0):
        """One step of a check. Ends early when the watchdog trips; the
        figures then carry the swing (wild_pp)."""
        self.link.do('PRES SP %.1f' % p1)
        t0, n, rows, since = self.clock.now(), 0, [], None
        wd = Wild(p1)
        while True:
            n += 1
            self.clock.wait_until(t0 + n)
            p, x, m = self.sample()
            if m != 'AUTO':
                raise RuntimeError(self.left('AUTO', m))
            t = self.clock.now() - t0
            rows.append((t, p, x))
            if p is not None:
                self.now('Check step %g -> %g mbar, %.0f s: pressure %.3f mbar, %s; watchdog %d of %d swings over %.2f mbar'
                         % (p0, p1, t, p, 'in the band %.0f s of %d' % (t - since, SETTLED_S) if since is not None
                            else 'outside the band +/- %.2f' % band, wd.status(), 2 * WILD_CYCLES, wd.big))
            if p is not None:
                since = (since if since is not None else t) if abs(p - p1) <= band else None
            if (since is not None and t - since >= SETTLED_S) or t >= most or wd.feed(p):
                break
        if self.log_dir:
            with open(os.path.join(self.log_dir, '%s.csv' % name), 'w', newline='') as f:
                w = csv.writer(f)
                w.writerow(['t_s', 'p_mbar', 'valve_pct', 'from_mbar', 'to_mbar'])
                for r in rows:
                    w.writerow(['%.1f' % r[0], '' if r[1] is None else '%.3f' % r[1],
                                '' if r[2] is None else '%.2f' % r[2], p0, p1])
        fig = step_figures(rows, p0, p1, band, slack)
        if fig is not None and wd.wild:
            fig['wild_pp'], fig['wild_big'] = round(wd.pp, 3), wd.pp > p1
        return fig

    def check(self, sp, kp, ti, r, k):
        """AUTO with kp, ti; setpoint up 20 % and back. The worse leg's
        figures, and the KP it ended with (lower if the watchdog cut it
        while it held; then the steps run with that). A leg that oscillates
        ends the check there."""
        self.pid(kp, ti)
        self.auto()
        kp = self.hold(sp, CHECK_HOLD_MAX_S, kp, ti, 'check %d: ' % k, '%g mbar check %d hold' % (sp, k))[3]
        hi = round(max(sp * (1 + CHECK_UP), sp + 0.2), 1)
        # The pressure's own wander is not the gains': overshoot counts only
        # beyond it, and it widens the band (9 Nov, 4 mbar: +/- 0.17 mbar
        # with the valve still, against 10 % of a 0.8 mbar step).
        slack = min(r.get('wander', 0.0), WANDER_MAX * (hi - sp))
        band = max(0.05 * (hi - sp), 2 * r['noise'], 0.02, slack)
        if k == 1:
            self.calc('Check limits: band +/- %.3f mbar = max(5 %% of the %.1f mbar step, 2 sigma, 0.02, the wander); '
                      'overshoot counted beyond the wander, %.3f mbar (at most %d %% of the step)' % (
                          band, hi - sp, slack, WANDER_MAX * 100))
        most = min(max(12 * r['Pu_s'] + 90, 150), 360)
        legs = []
        for k2, (a, b) in enumerate([(sp, hi), (hi, sp)]):
            self.stage('check %d: setpoint %g -> %g mbar, waiting for it to hold within %.2f mbar for %d s' %
                       (k, a, b, band, SETTLED_S), '%g mbar check %d %s' % (sp, k, 'up' if k2 == 0 else 'back'), most)
            legs.append(self.leg(sp, a, b, band, most, 'check_%gmbar_%d_%s' % (sp, k, 'up' if k2 == 0 else 'back'),
                                 slack))
            fg = legs[-1]
            if fg:
                self.calc('Check %d %s %g -> %g mbar (KP %.2f): %s' % (
                    k, 'up' if k2 == 0 else 'back', a, b, kp,
                    'oscillating, ended early (swing %.2f mbar)' % fg['wild_pp'] if fg.get('wild_pp') else
                    'overshoot %.1f %% (limit %.0f), %s, %d reversals (limit %d)' % (
                        fg['overshoot_pct'], OVERSHOOT_MAX,
                        'not settled' if fg['settle_s'] is None else 'settled in %.0f s' % fg['settle_s'],
                        fg['reversals'], REVERSALS_MAX)))
            if legs[-1] and legs[-1].get('wild_pp'):
                self.link.do('PRES SP %.1f' % sp)
                break
        return worse(*legs) if len(legs) == 2 else legs[0], kp

    def drop_check(self, sp, k):
        """Check k missed spec and is run again: its files go at once, so
        the run folder keeps only the check each result came from (Ryan,
        9 Nov). The session log still has every sample."""
        if not self.log_dir:
            return
        gone = []
        for leg in ('up', 'back'):
            name = 'check_%gmbar_%d_%s.csv' % (sp, k, leg)
            try:
                os.remove(os.path.join(self.log_dir, name))
                gone.append(name)
            except OSError:
                pass
        if gone:
            self.calc('Check %d missed spec: %s deleted' % (k, ', '.join(gone)))

    def point(self, sp, reach_gains, d, prior=None):
        """One test point: a result row for the table. prior: the point's
        learned row (load_learned), to refine from."""
        self.calc('%g mbar' % sp)        # a heading line: the setpoint alone
        if prior:
            self.calc('Learned %s: KP %.2f TI %.2f, Ku %s, Pu %s s%s' % (
                prior.get('when') or 'earlier', prior['kp'], prior['ti_min'],
                '--' if prior.get('Ku') is None else '%.3f' % prior['Ku'],
                '--' if prior.get('Pu_s') is None else '%.1f' % prior['Pu_s'],
                ' (trims ' + prior['trims'] + ')' if prior.get('trims') else ''))
        self.cuts = []
        x0 = self.reach(sp, *reach_gains, why='its learned gains' if prior else None)
        reach_cuts, self.cuts = self.cuts, []   # the reach gains are the point before's, not this point's
        r = self.relay(sp, x0, d)
        kp, ti = relay_gains(r['Ku'], r['Pu_s'], self.a.div, self.a.ti_pu)
        self.calc('KP = Ku / %g = %.3f / %g = %.2f %%/mbar; TI = %g x Pu = %g x %.1f s = %.0f s = %.2f min' % (
            self.a.div, r['Ku'], self.a.div, kp, self.a.ti_pu, self.a.ti_pu, r['Pu_s'], self.a.ti_pu * r['Pu_s'], ti))
        self.say('%g mbar: Ku %.2f %%/mbar, Pu %.1f s (relay %.1f %%, swing %.3f mbar, %d cycles%s) -> KP %.2f TI %.2f'
                 % (sp, r['Ku'], r['Pu_s'], r['d_pct'], r['a_mbar'], r['cycles'], '' if r['steady'] else ', not steady',
                    kp, ti))
        kp0, fig, why, f, ups, downs, k = kp, None, '', 1.0, 0, 0, 0
        refined = False
        if prior:
            old = prior.get('Ku')
            ratio = r['Ku'] / old if old else None
            if ratio is not None and 1 / LEARNED_AGREE <= ratio <= LEARNED_AGREE:
                kp, ti, refined = clip_kp(prior['kp']), clip_ti(prior['ti_min']), True
                self.calc('Refine: the new Ku is %.2f times the learned %.3f (within %g x), so the check starts from '
                          'the learned KP %.2f TI %.2f, not the rule\'s KP %.2f TI %.2f' % (
                              ratio, old, LEARNED_AGREE, kp, ti, kp0, relay_gains(r['Ku'], r['Pu_s'], self.a.div,
                                                                                   self.a.ti_pu)[1]))
            else:
                self.calc('Fresh: %s, so the check starts from the rule\'s KP %.2f TI %.2f' % (
                    'no learned Ku' if ratio is None else 'the new Ku is %.2f times the learned %.3f (more than %g x '
                    'off: the plant has changed)' % (ratio, old, LEARNED_AGREE), kp, ti))
        scores, useless = [], False
        # every change of KP at this point is in self.cuts, in order (calm() adds its own)
        while True:
            k += 1
            fig, kp = self.check(sp, kp, ti, r, k)
            f, why = trim_rule(fig, r['Pu_s'])
            if f != 1.0:
                self.calc('Rule: %s, so KP x %.2f' % (why, f))
            elif fig:
                self.calc('Rule: keep KP %.2f. Overshoot %.1f %% (a trim down over %.0f %%), %d reversals (down over %d); '
                          'a trim up needs under 1 %% overshoot and settling slower than %g Pu = %.0f s%s' % (
                              kp, fig['overshoot_pct'], OVERSHOOT_MAX, fig['reversals'], REVERSALS_MAX, SLOW_PU,
                              SLOW_PU * r['Pu_s'], ', or the valve at an end' if fig['saturated'] else ''))
            if fig:
                self.say('%g mbar check %d (KP %.2f): %s' % (
                    sp, k, kp, 'oscillating, swing %.2f mbar' % fig['wild_pp'] if fig.get('wild_pp') else
                    'overshoot %.1f %%, %s, %d reversals%s' % (
                        fig['overshoot_pct'], 'not settled' if fig['settle_s'] is None else
                        'settled in %.0f s' % fig['settle_s'], fig['reversals'],
                        ', valve at an end' if fig['saturated'] else '')))
            # Up at most `trims` times and never after a cut; down until it
            # passes, MAX_CUTS in all with the watchdog's.
            downs = sum(1 for c in self.cuts if c.startswith('x0'))
            scores.append(badness(fig))
            if f == 1.0:
                break
            cut = 'x%.2f' % TRIM_DOWN
            if (f == TRIM_DOWN and len(scores) >= 3 and self.cuts[-2:] == [cut, cut]
                    and min(scores[-2:]) >= USELESS * scores[-3]):
                # Two cuts and the check no better: the swing is not the
                # gains' (the pressure's wander, the needle), and more cuts
                # only make AUTO slower (9 Nov: six at 4 mbar, KP 0.17 to 0.04).
                useless = True
                self.calc('Not applied: the last two cuts (KP %.2f to %.2f) did not make the check 10 %% better '
                          '(%.1f, %.1f, %.1f times the limit), so the swing is not from the gains; KP stays %.2f'
                          % (kp / TRIM_DOWN ** 2, kp, scores[-3], scores[-2], scores[-1], kp))
                break
            if (f > 1.0 and (ups >= self.a.trims or downs)) or (f < 1.0 and downs >= MAX_CUTS):
                self.calc('Not applied: %s' % ('at most %d cuts at one setpoint' % MAX_CUTS if f < 1.0 else
                                               'no trim up after a cut' if downs else
                                               'at most %d trims up' % self.a.trims))
                break
            new = clip_kp(kp * f)
            if new == kp:
                break
            if f > 1.0:
                ups += 1
            kp = new
            self.cuts.append('x%.2f' % f)
            self.say('%g mbar: %s, KP x %.2f -> %.2f' % (sp, why, f, kp))
            self.drop_check(sp, k)
        trims = self.cuts
        self.calc('Result at %g mbar: KP %.2f, TI %.2f min%s' % (sp, kp, ti, ' (KP changed ' + ' '.join(trims) + ')'
                                                                 if trims else ''))
        self.now('')
        note = ''
        if useless:
            note = 'still %s; cuts stopped, two did not help' % why
        elif f < 1.0:
            note = 'still %s after %d cuts' % (why, MAX_CUTS)
        elif f > 1.0 and not downs:
            note = 'still %s after %d trims' % (why, self.a.trims)
        if not r['steady']:
            note = (note + '; ' if note else '') + 'relay not steady'
        if reach_cuts:
            note = (note + '; ' if note else '') + 'reach oscillated, its KP cut ' + ' '.join(reach_cuts)
        if refined:
            note = (note + '; ' if note else '') + 'refined from learned KP %.2f TI %.2f' % (prior['kp'], prior['ti_min'])
        fig = fig or {}
        return {'sp_mbar': sp, 'kp': kp, 'ti_min': ti, 'Ku': round(r['Ku'], 3), 'Pu_s': round(r['Pu_s'], 1),
                'kp_rule': kp0, 'd_pct': r['d_pct'], 'a_mbar': round(r['a_mbar'], 3), 'eps_mbar': round(r['eps_mbar'], 3),
                'cycles': r['cycles'], 'asym': None if r['asym'] is None else round(r['asym'], 2), 'valve_pct': x0,
                'trims': ' '.join(trims), 'overshoot_pct': fig.get('overshoot_pct'),
                'settle_s': None if fig.get('settle_s') is None else round(fig['settle_s']),
                'reversals': fig.get('reversals'), 'note': note}


def learned_key(sp):
    return '%g' % float(sp)


def _learned_rows(path):
    """The usable rows of a learned or results CSV (numbers as floats); a row
    with no KP or TI (a point that failed) is left out."""
    rows = []
    try:
        with open(path, newline='', encoding='utf-8') as f:
            for row in csv.DictReader(f):
                try:
                    r = dict(row)
                    for k in ('sp_mbar', 'kp', 'ti_min', 'Ku', 'Pu_s', 'd_pct', 'a_mbar', 'valve_pct'):
                        r[k] = float(r[k]) if r.get(k) not in (None, '') else None
                    if r['sp_mbar'] is None or not r['kp'] or r['ti_min'] is None:
                        continue
                    rows.append(r)
                except (TypeError, ValueError):
                    continue
    except OSError:
        pass
    return rows


def load_learned(path):
    """{'16': row} from autotune-learned.csv; {} if there is none or it
    cannot be read."""
    return {learned_key(r['sp_mbar']): r for r in _learned_rows(path)}


def seed_learned(folder):
    """The learned table built from the results.csv of every earlier autotune
    run in the log folder (autotune_<date>_<time>/), newest per setpoint: so
    runs made before the learned file existed still count. {} if none."""
    learned = {}
    try:
        runs = sorted(d for d in os.listdir(folder) if d.startswith('autotune_'))
    except OSError:
        return learned
    for d in runs:
        for r in _learned_rows(os.path.join(folder, d, 'results.csv')):
            t = r.get('time') or ''
            try:
                when = time.strftime('%d/%m/%y %H:%M', time.strptime(t, '%Y-%m-%d %H:%M:%S'))
            except ValueError:
                when = t
            learn(learned, r, when)
    return learned


def save_learned(path, learned):
    """Writes every learned row, lowest setpoint first (replaced whole, so a
    half-written file is never left: written beside it and renamed)."""
    tmp = path + '.tmp'
    with open(tmp, 'w', newline='', encoding='utf-8') as f:
        w = csv.DictWriter(f, fieldnames=LEARNED_COLS, extrasaction='ignore')
        w.writeheader()
        for k in sorted(learned, key=float):
            row = {c: '' if learned[k].get(c) is None else learned[k][c] for c in LEARNED_COLS}
            row['sp_mbar'] = k          # '%g': 4, not 4.0
            w.writerow(row)
    os.replace(tmp, path)


def learn(learned, r, when):
    """A point's result row into the learned table."""
    row = {c: r.get(c) for c in LEARNED_COLS}
    row['valve_pct'] = r.get('valve_pct')
    row['when'] = when
    learned[learned_key(r['sp_mbar'])] = row


def _when(s):
    """A learned row's 'when' (DD/MM/YY HH:MM) as a tuple that sorts by
    time; None if it is not one."""
    try:
        return tuple(time.strptime(s or '', '%d/%m/%y %H:%M'))[:5]
    except ValueError:
        return None


def merge_analysed(learned, path=ANALYSED_FILE):
    """The gains worked out by hand from the bench logs (autotune-analysed.csv
    beside this file; Ryan, 10 Nov: "put your analysed new kp ti kd values in
    the learned values") into the learned table. Each replaces the learned
    row for its setpoint when that row is older or has no date, so it goes in
    once and a later autotune result still wins. The setpoints it replaced."""
    put = []
    for r in _learned_rows(path):
        new = _when(r.get('when'))
        if new is None:
            continue
        k = learned_key(r['sp_mbar'])
        old = _when((learned.get(k) or {}).get('when'))
        if old is None or old < new:
            learn(learned, r, r['when'])
            put.append(k)
    return put


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
