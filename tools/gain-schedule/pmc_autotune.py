#!/usr/bin/env python3
"""PID autotune for the board's PID table, run from a PC over the text port.

The control test GUI (tools/pmc-gui) runs it from its Gains tab; this file
holds the sequence so it can be tested on a model without a board. It also
holds the text-port link and clock the GUI uses.

For each test point, in the order given (the GUI: as typed, 2 4 8 12 16 24 48 mbar by default):
  1. Reach: AUTO at the point with safe gains until the pressure holds; the
     valve position it holds at is x0.
  2. Relay test (Astrom and Hagglund, as industrial autotuners do): MANUAL,
     the valve at x0 + d while the pressure is below the point and x0 - d
     while it is above, so the loop oscillates by itself at the frequency
     where the plant lags half a cycle. The oscillation gives the ultimate
     gain Ku (the KP at which AUTO would oscillate) and its period Pu, with
     no curve fit and no settled baseline. d grows when nothing crosses and
     shrinks when the swing is too big.
  3. First gains. With a learned fit of the cryostat's response (K, L, tau)
     for this point: L_eff = max(L, Pu / 4), TI = min(max(tau, 2.5 L_eff),
     20 L_eff), KP = min(TI / (5 K L_eff), Ku / 3), KD 0. Without one:
     KP = Ku / 4, TI = 2 Pu.
  4. Check, then refine, up to four times. Check: AUTO with those gains,
     setpoint 20 % up and back. After every check the pressure is fitted to
     the valve over that check's samples,
         p(t) = c + drift x t + K x lag_tau( valve(t - L) ),
     on a grid of L and tau, and the fit is kept when it is good. The check
     is judged on overshoot and reversals, and on the shape of the answer:
     the seconds to 63 % and 90 % of the step, how far the valve backs off
     before 90 %, and how long the pressure sags under the setpoint with the
     valve still. The first rule that applies is applied (refine_rule):
     too many reversals or a watchdog cut, KP x 0.7; overshoot, KP x 0.8; a
     long tail with the valve backing off, TI x 0.7; slow, TI x 0.8. KP is
     never raised. A refinement that makes the check worse is undone and
     ends the refinements.
Whenever AUTO runs (reaching a point, holding it, the check steps), a
watchdog looks for a sustained oscillation: two full cycles in a row (four
swings), each swing more than 30 % of the setpoint. It then cuts KP at once (x 0.5, or
x 0.25 when the swing is larger than the setpoint itself) with PID SET and
carries on with the lower gain (Ryan, 9 Nov: "make sure it tunes down the
gains when it oscillates like crazy"; at 2 mbar the 1 mbar gains swung the
pressure 1 to 10 mbar every 14 s).
Then the three PID table sets come from the rows in each band (below B1, B1
to B2, above B2; 6 and 14 mbar by default): the lowest KP of the rows in the
band and the geometric mean of their TIs.

Learned gains (Ryan, 9 Nov: "it should refine the numbers rather than
starting fresh every time"): each point's result, with the fit it ended on,
is kept in autotune-learned.csv in the log folder. On the next run a point
with a learned row reaches its setpoint with its own learned gains, sizes
the relay step from its learned Ku and, when the row has a fit, starts the
check from the first-gains rule on that fit. If the new relay test's Ku is
more than LEARNED_AGREE times off the learned one the plant has changed, and
that point starts fresh. Fresh ignores the file (its results still go in
it). With no learned file yet, it is built from the results.csv of the
earlier autotune runs in the same log folder (newest per setpoint).

While it runs, the PID table is Off and set 1 carries the gains under test
(PID SET); at the end set 1, the PID table, the setpoint and the mode are put
back as found. Writing the sets to the board (PIDS SET, SCHED SET PV) is a
separate step, after the results have been looked at.

The log (Ryan, 11 Nov: "a logger for the autotune, with the decisions and
calculations made and the changes to the kp ki kd"): autotune_log.csv in the
run's folder, one row per calculation, decision, gain change and board
command, as it happens, each with the KP, TI and KD in force on the board
after it (log_row, LOG_COLS).

Python 3.8 or later, standard library only. Needs firmware 0.15.8 or later
(PID SET); the sets need 0.15.15 or later (PIDS SET, SCHED SET).
"""
import cmath
import csv
import math
import os
import socket
import time

TCP_PORT = 8888

# Firmware limits for KP and TI (cfg.c tune_lo/tune_hi, two decimals).
KP_MIN, KP_MAX = 0.01, 100.0
TI_MIN, TI_MAX = 0.01, 100.0

POINTS = [1, 2, 4, 8, 12, 16, 24, 48]
B1, B2, HYST = 6.0, 14.0, 10

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

# First gains without a fit: the old relay rule.
FIRST_DIV, FIRST_TI_PU = 4.0, 2.0   # KP = Ku / 4, TI = 2 Pu

# Check step.
CHECK_UP = 0.20                # setpoint up by this fraction, then back
SETTLED_S = 60
OVERSHOOT_MAX = 5.0            # % of the step, beyond the wander
REVERSALS_MAX = 2
WANDER_MAX = 0.25              # the wander allowed for in a check: at most this fraction of the step
RUNOFF_S = 120                 # after a run-off, how long the pressure has to come back

# Fit of the response to the valve (fit_response).
L_GRID = [0, 2, 4, 6, 8, 12, 16, 24, 32, 48, 64]                       # dead time, s
TAU_GRID = [2, 4, 6, 9, 13, 18, 25, 35, 50, 70, 100, 140, 200]         # lag, s
R2_MIN = 0.8                   # a fit is kept when it explains this much
MOVE_MIN = 0.05                # ...and the valve moved more than this, %, in the window
KU_CAP = 3.0                   # KP never over Ku / this

# Refine rules (refine_rule).
MAX_REFINES = 4                # most refinements at one point
R1_KP = 0.7                    # too many reversals, or the watchdog cut KP
R2_KP = 0.8                    # overshoot
R3_TI = 0.7                    # a tail: the valve backs off, then opens slowly again
R4_TI = 0.8                    # slow with no overshoot or back-off
TAIL_RATIO = 4.0               # t90 over this times t63 = a tail
BACKOFF_MAX = 10.0             # % of the valve's move
SLOW_RATIO = 8.0               # t90 over this times (L_eff + tau) = slow
NO_OVERSHOOT = 1.0             # under this % of the step counts as none
TI_MIN_LEFF = 2.5              # TI never under this times L_eff
KI_LEFF = 4.0                  # integral gain KP / TI never over 1 / (this x K x L_eff)
VALVE_STILL = 0.01             # the valve moved less than this, %, in 30 s = not moving
WORSE_T90 = 1.5                # a check this many times slower to 90 % is worse
WORSE_BAD = 1.15               # ...or this many times as far over the overshoot and reversal limits

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
LEARNED_COLS = ['sp_mbar', 'kp', 'ti_min', 'Ku', 'Pu_s', 'd_pct', 'a_mbar', 'valve_pct', 'trims', 'note', 'when',
                'K', 'L_s', 'tau_s', 't63_s', 't90_s', 'backoff_pct']
LEARNED_NUM = ('sp_mbar', 'kp', 'ti_min', 'Ku', 'Pu_s', 'd_pct', 'a_mbar', 'valve_pct',
               'K', 'L_s', 'tau_s', 't63_s', 't90_s', 'backoff_pct')
LEARNED_AGREE = 2.0            # learned Ku within this factor of the new relay's: use its fit
MAX_CUTS = 6                   # most KP cuts by the watchdog while reaching or holding one point
LOG_FILE = 'autotune_log.csv'  # in the run's folder
# time: PC clock; t_s: seconds since the autotune started (board time);
# kind: start, calc, decision, gains, command, result, end; kp, ti_min, kd:
# the gains on the board after the row (as far as the autotune knows);
# change: what a gains row changed, old -> new.
LOG_COLS = ['time', 't_s', 'setpoint_mbar', 'stage', 'kind', 'kp', 'ti_min', 'kd', 'change', 'text']


# ---------------------------------------------------------------------------
# The text port, the clock and the sampling the sequence stands on.
# ---------------------------------------------------------------------------

class Refused(Exception):
    """The board answered ERR."""


class Link:
    def __init__(self, host, timeout=10.0):
        h, _, p = host.partition(':')
        self.addr = (h, int(p) if p else TCP_PORT)
        self.timeout = timeout
        self.sock = None
        self.buf = b''

    def connect(self):
        self.close()
        self.sock = socket.create_connection(self.addr, timeout=self.timeout)
        self.buf = b''

    def close(self):
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
        self.sock = None

    def ask(self, line):
        """Send one line, return the reply's parts; raise Refused on ERR."""
        if not self.sock:
            self.connect()
        self.sock.sendall(line.encode('ascii') + b'\r\n')
        while b'\n' not in self.buf:
            d = self.sock.recv(4096)
            if not d:
                raise ConnectionError('board closed the connection')
            self.buf += d
        reply, _, self.buf = self.buf.partition(b'\n')
        parts = reply.decode('latin1').strip().split(';')
        if parts and parts[0].startswith('ERR'):
            raise Refused(parts[0])
        return parts

    def do(self, cmd):
        """A set command (silent when accepted), followed by MODE? so there is a reply."""
        return self.ask(cmd + ';MODE?')


def value(part, head):
    """'PRES 2.913 mbar' -> 2.913; 'PRES NONE' -> None."""
    if not part.startswith(head):
        raise ValueError(f'expected {head}, got {part!r}')
    v = part[len(head):].split()
    if not v or v[0] == 'NONE':
        return None
    return float(v[0])


class Clock:
    """Board seconds; --speed > 1 only for a simulator run faster than real time."""
    def __init__(self, speed=1.0):
        self.speed = speed
        self.t0 = time.monotonic()

    def now(self):
        return (time.monotonic() - self.t0) * self.speed

    def wait_until(self, t):
        dt = (t - self.now()) / self.speed
        if dt > 0:
            time.sleep(dt)


class Tuner:
    def __init__(self, link, clock, a, log_dir):
        self.link, self.clock, self.a, self.log_dir = link, clock, a, log_dir

    def say(self, msg):
        print(f'[{self.clock.now():7.0f} s] {msg}', flush=True)

    def sample(self):
        p, x, m = self.link.ask('PRES?;VALV?;MODE?')
        return value(p, 'PRES'), value(x, 'VALV'), m.partition(' ')[2]

    def samples(self, seconds, stop=None):
        """Read once a board second for up to `seconds`; stop(rows) ends early."""
        rows, t0 = [], self.clock.now()
        nxt = t0
        while True:
            self.clock.wait_until(nxt)
            nxt += 1.0
            p, x, m = self.sample()
            if p is not None:
                rows.append((self.clock.now() - t0, p, x, m))
            if rows and rows[-1][0] >= seconds:
                return rows
            if stop and stop(rows):
                return rows


def clip_kp(kp):
    return round(min(max(kp, KP_MIN), KP_MAX), 2)


def clip_ti(ti):
    return round(min(max(ti, TI_MIN), TI_MAX), 2)


def relay_gains(ku, pu_s, div=FIRST_DIV, ti_pu=FIRST_TI_PU):
    """KP (% per mbar) and TI (min) from the ultimate gain and period: the
    first gains of a point that has no fit yet."""
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



def lag_filter(u, tau):
    """First-order lag of the samples u (one a second), starting at u[0]."""
    a = 1.0 - math.exp(-1.0 / tau)
    y, out = u[0], []
    for v in u:
        y += a * (v - y)
        out.append(y)
    return out


def fit_response(ps, xs):
    """Fit the pressure to the valve, both one sample a second:

        p(t) = c + drift x t + K x lag_tau( valve(t - L) )

    over the grids L_GRID and TAU_GRID, each pair by least squares in c,
    drift and K (the normal equations), keeping the pair with the least
    squared error. Returns {'K' mbar per %, 'L' s, 'tau' s, 'R2', 'drift'
    mbar per s, 'n', 'ok'} or None with too few samples or a valve that never
    moved; ok says it is good enough to keep: R2 >= R2_MIN, K > 0 and the
    valve moved more than MOVE_MIN % in the window. (The delay and the lag
    commute, so each lag is filtered once and shifted for every L.)"""
    n = len(ps)
    if n < 30 or len(xs) != n:
        return None
    moved = max(xs) - min(xs)
    mp = sum(ps) / n
    tt = [i - (n - 1) / 2.0 for i in range(n)]
    pc = [p - mp for p in ps]
    spp = sum(v * v for v in pc)
    stt = sum(t * t for t in tt)
    stp = sum(t * v for t, v in zip(tt, pc))
    if spp <= 0 or moved <= 0:
        return None
    best = None
    for tau in TAU_GRID:
        y = lag_filter(xs, tau)
        for L in L_GRID:
            if L >= n // 2:
                continue
            x = [y[0]] * L + y[:n - L] if L else y
            mx = sum(x) / n
            xc = [v - mx for v in x]
            sxx = sum(v * v for v in xc)
            if sxx <= 1e-12:
                continue
            stx = sum(t * v for t, v in zip(tt, xc))
            sxp = sum(a * b for a, b in zip(xc, pc))
            det = stt * sxx - stx * stx
            if det <= 1e-9 * stt * sxx:
                continue
            drift = (sxx * stp - stx * sxp) / det
            k = (stt * sxp - stx * stp) / det
            sse = spp - drift * stp - k * sxp
            if best is None or sse < best[0]:
                best = (sse, k, drift, L, tau)
    if best is None:
        return None
    sse, k, drift, L, tau = best
    r2 = 1.0 - max(sse, 0.0) / spp
    return {'K': k, 'L': L, 'tau': tau, 'R2': r2, 'drift': drift, 'n': n, 'moved': moved,
            'ok': r2 >= R2_MIN and k > 0 and moved > MOVE_MIN}


def l_eff(fit, pu_s):
    """The dead time the loop has to allow for, s: the fit's, but never under
    a quarter of the relay's period."""
    return max(fit['L'] if fit else 0.0, pu_s / 4.0)


def rule_gains(fit, ku, pu_s):
    """KP (% per mbar) and TI (min) from a fit {K, L, tau} and the relay
    test's Ku and Pu:
        L_eff = max(L, Pu / 4)
        TI    = min(max(tau, 2.5 L_eff), 20 L_eff)    seconds
        KP    = min(TI / (5 K L_eff), Ku / 3)
    KD is 0."""
    le = l_eff(fit, pu_s)
    ti_s = min(max(fit['tau'], TI_MIN_LEFF * le), 20.0 * le)
    kp = min(ti_s / (5.0 * fit['K'] * le), ku / KU_CAP)
    return clip_kp(kp), clip_ti(ti_s / 60.0)


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
    """rows [(s from the change, p, x)]: the figures of one leg of a check.

    overshoot_pct   beyond slack mbar (the pressure's own wander), % of the step
    settle_s        when it settled in the band for SETTLED_S (None if not)
    reversals       error reversals after the first crossing
    t63_s, t90_s    seconds to cover 63 % and 90 % of the step (None if never)
    backoff_pct     the largest retreat of the valve from its furthest point
                    before t90 (all of the leg if it never got there), as %
                    of the valve's whole move in the leg (the furthest it got
                    from where it started, so never over 100 %)
    sag_s           the longest stretch, once the pressure has reached the
                    band, under the band (short of the setpoint, on the way
                    back down as well as up) with the valve not moving
                    (under VALVE_STILL % in 30 s)
    saturated       the valve sat at an end (a slow step is then not the gains' fault)"""
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
    t63 = next((t for t, p in ps if (p - p0) / step >= 0.63), None)
    t90 = next((t for t, p in ps if (p - p0) / step >= 0.90), None)
    # The valve's back-off.
    back = 0.0
    xt = [(t, x) for t, _, x in rows if x is not None]
    if len(xt) >= 2:
        sg = 1 if xt[-1][1] >= xt[0][1] else -1          # the way the valve went
        move = max((x - xt[0][1]) * sg for _, x in xt)    # its whole move: the furthest it got
        if move >= MOVE_MIN:
            far = retreat = None
            for t, x in xt:
                if t90 is not None and t > t90:
                    break
                if far is None or (x - far) * sg > 0:
                    far = x
                retreat = max(retreat or 0.0, (far - x) * sg)
            back = (retreat or 0.0) / move * 100
    # The sag: the pressure falling under the setpoint, the valve still.
    sag, start, reached = 0.0, None, False
    for i, (t, p, x) in enumerate(rows):
        if p is None:
            continue
        if abs(p - p1) <= band:
            reached, start = True, None
            continue
        short = p < p1 - band                # under the setpoint, on either leg
        j = max(0, i - 30)
        still = x is not None and rows[j][2] is not None and abs(x - rows[j][2]) < VALVE_STILL
        if reached and short and still:
            start = t if start is None else start
            sag = max(sag, t - start)
        else:
            start = None
    return {'overshoot_pct': round(over / abs(step) * 100, 1), 'settle_s': settle, 'reversals': rev,
            't63_s': None if t63 is None else round(t63), 't90_s': None if t90 is None else round(t90),
            'backoff_pct': round(back, 1), 'sag_s': round(sag),
            'saturated': bool(xs) and (max(xs) >= 99.5 or min(xs) <= 0.05)}


def refine_rule(fig, kp, ti, ku, pu_s, fit):
    """What to change after a check: the first rule that applies.

      1. more than REVERSALS_MAX reversals, or the watchdog cut KP: KP x 0.7
         (a watchdog-ended leg: its own cut, x 0.5 or x 0.25, if that is more)
      2. overshoot over OVERSHOOT_MAX % of the step, on either leg: KP x 0.8
      3. tail: no overshoot, t90 over 4 x t63 and the valve backing off over
         10 %: TI x 0.7, KP kept
      4. slow: no overshoot, no back-off, t90 over 8 x (L_eff + tau): TI x 0.8
      5. otherwise the check passes.

    Rules 3 and 4 never take TI under 2.5 L_eff (in min) or the integral gain
    KP / TI over 1 / (4 K L_eff); at the limit the check passes ('limit').
    KP is never raised, and never over Ku / 3.

    Returns {'kind': 'kp' | 'ti' | 'pass' | 'limit', 'kp', 'ti', 'why', 'f' (the factor), 'note'}."""
    keep = {'kind': 'pass', 'kp': kp, 'ti': ti, 'why': '', 'note': ''}
    if fig is None:
        return keep
    le = l_eff(fit, pu_s)

    def cut(f, why):
        new = clip_kp(min(kp * f, ku / KU_CAP))
        if new >= kp:
            return dict(keep, kind='limit', why=why + ', KP already at its lowest', note='KP already at its lowest, still ' + why)
        return {'kind': 'kp', 'kp': new, 'ti': ti, 'why': why, 'f': f}

    def shorten(f, why):
        floor = TI_MIN_LEFF * le / 60.0
        if fit:
            floor = max(floor, KI_LEFF * fit['K'] * le * kp / 60.0)
        new = clip_ti(max(ti * f, floor))
        if new >= ti:
            return dict(keep, kind='limit', why=why + ', as fast as the cryostat allows', note='as fast as the cryostat allows')
        return {'kind': 'ti', 'kp': kp, 'ti': new, 'why': why, 'f': f}

    if fig.get('wild_pp') or fig['reversals'] > REVERSALS_MAX or fig.get('cut'):
        f = R1_KP
        if fig.get('wild_pp'):
            f = min(R1_KP, fig.get('wild_cut', 1.0))
        return cut(f, 'oscillating (swing %.2f mbar)' % fig['wild_pp'] if fig.get('wild_pp') else
                   '%d reversals' % fig['reversals'] if fig['reversals'] > REVERSALS_MAX else 'the watchdog cut KP')
    if fig['overshoot_pct'] > OVERSHOOT_MAX:
        return cut(R2_KP, 'overshoot %.0f %%' % fig['overshoot_pct'])
    if fig['overshoot_pct'] < NO_OVERSHOOT and not fig['saturated']:
        t63, t90 = fig.get('t63_s'), fig.get('t90_s')
        far = float('inf')
        if t63 is not None and (t90 is None or t90 > TAIL_RATIO * max(t63, 1)) and \
                fig.get('backoff_pct', 0.0) > BACKOFF_MAX:
            return shorten(R3_TI, 'a tail: 90 %% in %s s against 63 %% in %d s, the valve backing off %.0f %%' % (
                '--' if t90 is None else '%d' % t90, t63, fig['backoff_pct']))
        if fit and fig.get('backoff_pct', 0.0) <= BACKOFF_MAX and \
                (t90 if t90 is not None else far) > SLOW_RATIO * (le + fit['tau']):
            return shorten(R4_TI, 'slow: 90 %% in %s s, over %g x (%.0f + %d s)' % (
                '--' if t90 is None else '%d' % t90, SLOW_RATIO, le, fit['tau']))
    return keep


def worse(a, b):
    """The two legs of a check as one: the worse of each figure."""
    if a is None or b is None:
        return a or b

    def mx(k):
        x, y = a.get(k), b.get(k)
        return None if x is None or y is None else max(x, y)
    out = {'overshoot_pct': max(a['overshoot_pct'], b['overshoot_pct']), 'settle_s': mx('settle_s'),
           'reversals': max(a['reversals'], b['reversals']), 'saturated': a['saturated'] or b['saturated'],
           't63_s': mx('t63_s'), 't90_s': mx('t90_s'),
           'backoff_pct': max(a.get('backoff_pct', 0.0), b.get('backoff_pct', 0.0)),
           'sag_s': max(a.get('sag_s', 0), b.get('sag_s', 0))}
    w = max((x for x in (a, b) if x.get('wild_pp')), key=lambda x: x['wild_pp'], default=None)
    if w:
        out['wild_pp'], out['wild_big'], out['wild_cut'] = w['wild_pp'], w.get('wild_big', False), w.get('wild_cut', 1.0)
    return out


def worse_check(new, old):
    """True when a refinement made the check worse: it oscillates where it did
    not, it is over the overshoot or reversal limits by WORSE_BAD times as much
    as the check before, or it takes WORSE_T90 times as long to reach 90 %."""
    if new is None or old is None:
        return False
    if new.get('wild_pp') and not old.get('wild_pp'):
        return True
    bn, bo = badness(new), badness(old)
    if bn > 1.0 and bn > WORSE_BAD * bo:
        return True
    tn, to = new.get('t90_s'), old.get('t90_s')
    if to is not None and (tn is None or tn > WORSE_T90 * max(to, 1)):
        return True
    return False


def geomean(xs):
    return math.exp(sum(math.log(x) for x in xs) / len(xs))


def table_sets(rows, b1=B1, b2=B2):
    """The three PID table sets from the rows: [(set, kp, ti, [points])],
    kp and ti None where no point fell in that band. Per band: the lowest KP
    of the rows in it (so no setpoint in the band gets more KP than it was
    tuned for) and the geometric mean of their TIs; KD is 0."""
    bands = [lambda s: s < b1, lambda s: b1 <= s < b2, lambda s: s >= b2]
    out = []
    for n, inside in enumerate(bands, 1):
        rs = [r for r in rows if inside(r['sp_mbar'])]
        if rs:
            out.append((n, clip_kp(min(r['kp'] for r in rs)), clip_ti(geomean([r['ti_min'] for r in rs])),
                        [r['sp_mbar'] for r in rs]))
        else:
            out.append((n, None, None, []))
    return out


def write_lines(sets, b1=B1, b2=B2, h=HYST):
    """The text-port lines that put the sets and the PID table on the board."""
    lines = ['PIDS SET %d,%.2f,%.2f,0' % (n, kp, ti) for n, kp, ti, _ in sets if kp is not None]
    lines.append('SCHED SET PV,%.1f,%.1f,%d' % (b1, b2, h))
    return lines


def log_row(log_dir, row):
    """One row (LOG_COLS) appended to the run's autotune_log.csv, the header
    first when the file is new. Each row is written as it comes, so a run
    that stops or stalls keeps its log up to there."""
    if not log_dir:
        return
    path = os.path.join(log_dir, LOG_FILE)
    try:
        new = not os.path.exists(path)
        with open(path, 'a', newline='', encoding='utf-8') as f:
            w = csv.writer(f)
            if new:
                w.writerow(LOG_COLS)
            w.writerow(['' if v is None else v for v in row])
    except OSError:
        pass


def gains_change(old, new):
    """'KP 0.64 -> 0.45, TI 0.88 -> 0.62': what changed from old to new
    ([KP, TI, KD]; old None = not known)."""
    out = []
    for name, i in (('KP', 0), ('TI', 1), ('KD', 2)):
        a = None if not old else old[i]
        if a is None or abs(a - new[i]) > 1e-9:
            out.append('%s %s -> %.2f' % (name, '--' if a is None else '%.2f' % a, new[i]))
    return ', '.join(out)


class AutoTuner(Tuner):
    """The sequence on the board, one point at a time."""

    series = []     # (pressure, valve) of the check just run, one a second, for the fit
    held_rows = []  # the samples of the last hold(), for the same

    def stage(self, text, phase=None, most=None):
        if phase:
            self.phase_now = phase
        self.say(text)

    # The working, for the page: the lines of this setpoint's calculations
    # (calc) and one line that changes while a stage runs (now).
    calcs, calc_now = [], ''

    def calc(self, text, kind='calc'):
        self.calcs = (self.calcs + [text])[-400:]
        self.log(kind, text)
        self.show()

    def decide(self, text):
        """A calculation that chooses what happens next: in the Working card
        as any other line, and a decision in the log."""
        self.calc(text, 'decision')

    # The log: see LOG_COLS. gains: [KP, TI, KD] on the board as far as the
    # autotune knows (None until found or first set); sp_now, phase_now:
    # the point and the stage the rows belong to; log_rows: the rows other
    # than calculations, for the page.
    gains, sp_now, phase_now, t_log0, check_why = None, None, '', None, ''
    log_rows = []

    def log(self, kind, text, change=''):
        t = self.clock.now() if self.clock is not None else None
        if t is not None and self.t_log0 is None:
            self.t_log0 = t
        g = self.gains or [None] * 3
        row = [time.strftime('%Y-%m-%d %H:%M:%S'), '' if t is None else '%.1f' % (t - self.t_log0),
               '' if self.sp_now is None else '%g' % self.sp_now, self.phase_now, kind] + \
              ['' if v is None else '%.2f' % v for v in g] + [change, text]
        log_row(self.log_dir, row)
        if kind != 'calc':
            self.log_rows = (self.log_rows + [row])[-300:]

    def send(self, cmd, why=''):
        """A line that changes the board's setpoint, mode or valve, logged."""
        self.link.do(cmd)
        self.log('command', cmd + (': ' + why if why else ''))

    def set_gains(self, new, cmd, why=''):
        """The autotune changed the gains on the board: logged with old -> new."""
        new = [round(float(v), 2) for v in new]
        change = gains_change(self.gains, new)
        self.gains = new
        if change:
            self.log('gains', cmd + (': ' + why if why else ''), change)

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

    def auto(self, why=''):
        self.still()
        was = self.sample()[2]
        self.link.do('MODE AUTO')
        if was != 'AUTO':
            self.log('command', 'MODE AUTO' + (': ' + why if why else ''))

    def pid(self, kp, ti, why=''):
        if self.sample()[2] != 'AUTO':
            self.still()
        cmd = 'PID SET %.2f,%.2f,0' % (kp, ti)
        self.link.do(cmd)
        self.set_gains([kp, ti, 0.0], cmd, why)

    def calm(self, sp, kp, ti, w):
        """The watchdog tripped: KP cut at once (in AUTO), the new KP."""
        new = clip_kp(kp * w.cut())
        if new >= kp:
            raise RuntimeError('AUTO oscillating at %g mbar (swing %.2f mbar) with KP already at %.2f'
                               % (sp, w.pp, kp))
        self.say('%g mbar: AUTO oscillating (swing %.2f mbar), KP x %.2f -> %.2f' % (sp, w.pp, w.cut(), new))
        self.decide('Oscillating: %d swings in a row, the smallest %.2f mbar, each over %.2f mbar (%d %% of %g mbar). '
                  'KP x %.2f%s: %.2f x %.2f = %.2f (PID SET, AUTO carries on)' % (
                      2 * WILD_CYCLES, w.pp, w.big, WILD_PP * 100, sp, w.cut(),
                      ', the swing larger than the setpoint' if w.cut() == WILD_CUT_BIG else '', kp, w.cut(), new))
        self.pid(new, ti, 'watchdog: AUTO oscillating at %g mbar, KP x %.2f' % (sp, w.cut()))
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
        self.held_rows = rows
        ok = level(rows)
        span = 60 if ok else 30      # not held: where it was last, nearest to where it would hold
        xs = [r[2] for r in rows if r[0] >= rows[-1][0] - span and r[2] is not None]
        ps = [r[1] for r in rows if r[0] >= rows[-1][0] - 30]
        x0 = round(sum(xs) / len(xs), 1) if xs else None
        self.calc('%s at %g mbar: x0 = %s %% (mean valve, last %d s), pressure %.3f mbar (mean, last 30 s)' % (
            'Held' if ok else 'Not held', sp, '--' if x0 is None else '%.1f' % x0, span, sum(ps) / len(ps)))
        return x0, ok, sum(ps) / len(ps), kp

    def reach(self, sp, kp, ti, why=None):
        self.decide('Reach %g mbar: AUTO with KP %.2f TI %.2f (%s)' % (
            sp, kp, ti, why or 'the setpoint before\'s gains, or the first gains'))
        self.pid(kp, ti, 'reach %g mbar with %s' % (sp, why or 'the setpoint before\'s gains, or the first gains'))
        self.send('PRES SP %.1f' % sp, 'reach %g mbar' % sp)
        if self.sample()[2] != 'AUTO':
            self.auto('reach %g mbar' % sp)
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
        self.phase_now = '%g mbar noise' % sp
        self.send('MODE MANUAL', 'relay test at %g mbar' % sp)
        self.send('VALV SET %.1f' % x0, 'x0, the valve AUTO held %g mbar at' % sp)
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
                    self.decide('Relay: ' + R.note)
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
            self.send('VALV SET %.1f' % x0, 'relay test ended: ' + R.why)
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
        self.send('PRES SP %.1f' % p1, 'check step %g -> %g mbar' % (p0, p1))
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
        self.leg_rows = rows
        fig = step_figures(rows, p0, p1, band, slack)
        if fig is not None and wd.wild:
            fig['wild_pp'], fig['wild_big'], fig['wild_cut'] = round(wd.pp, 3), wd.pp > p1, wd.cut()
        return fig

    def check(self, sp, kp, ti, r, k):
        """AUTO with kp, ti; setpoint up 20 % and back. The worse leg's
        figures, and the KP it ended with (lower if the watchdog cut it
        while it held; then the steps run with that). A leg that oscillates
        ends the check there."""
        self.phase_now = '%g mbar check %d hold' % (sp, k)
        self.pid(kp, ti, 'check %d at %g mbar%s' % (k, sp, ' with ' + self.check_why if self.check_why else ''))
        self.auto('check %d at %g mbar' % (k, sp))
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
        rows = list(self.held_rows)     # the hold, then the two legs: one series for the fit
        legs = []
        for k2, (a, b) in enumerate([(sp, hi), (hi, sp)]):
            self.stage('check %d: setpoint %g -> %g mbar, waiting for it to hold within %.2f mbar for %d s' %
                       (k, a, b, band, SETTLED_S), '%g mbar check %d %s' % (sp, k, 'up' if k2 == 0 else 'back'), most)
            legs.append(self.leg(sp, a, b, band, most, 'check_%gmbar_%d_%s' % (sp, k, 'up' if k2 == 0 else 'back'),
                                 slack))
            rows += self.leg_rows
            fg = legs[-1]
            if fg:
                self.calc('Check %d %s %g -> %g mbar (KP %.2f): %s' % (
                    k, 'up' if k2 == 0 else 'back', a, b, kp,
                    'oscillating, ended early (swing %.2f mbar)' % fg['wild_pp'] if fg.get('wild_pp') else
                    'overshoot %.1f %% (limit %.0f), %s, %d reversals (limit %d), 63 %% in %s, 90 %% in %s, '
                    'valve back-off %.0f %%, sag %d s' % (
                        fg['overshoot_pct'], OVERSHOOT_MAX,
                        'not settled' if fg['settle_s'] is None else 'settled in %.0f s' % fg['settle_s'],
                        fg['reversals'], REVERSALS_MAX,
                        'not reached' if fg['t63_s'] is None else '%d s' % fg['t63_s'],
                        'not reached' if fg['t90_s'] is None else '%d s' % fg['t90_s'],
                        fg['backoff_pct'], fg['sag_s'])))
            if legs[-1] and legs[-1].get('wild_pp'):
                self.send('PRES SP %.1f' % sp, 'check %d ended: oscillating' % k)
                break
        # One sample a second, the valve held over a missing reading.
        xs, ps, last = [], [], None
        for row in rows:
            if row[1] is None:
                continue
            last = row[2] if row[2] is not None else last
            if last is not None:
                ps.append(row[1])
                xs.append(last)
        self.series = (ps, xs)
        return worse(*legs) if len(legs) == 2 else legs[0], kp

    def refit(self, k, fit):
        """The check's pressure fitted to its valve (fit_response). Returns the
        fit to carry on with: the new one when it is good, else `fit`."""
        ps, xs = self.series if self.series else ([], [])
        new = fit_response(ps, xs)
        if new is None:
            self.calc('Fit after check %d: nothing to fit (%d samples, valve moved %.2f %%), keeping %s' % (
                k, len(ps), max(xs) - min(xs) if xs else 0.0, 'the earlier fit' if fit else 'none'))
            return fit
        self.calc('Fit after check %d (%d samples): K %.2f mbar/%%, L %d s, tau %d s, R2 %.2f, valve moved %.2f %%: %s' % (
            k, new['n'], new['K'], new['L'], new['tau'], new['R2'], new['moved'],
            'kept' if new['ok'] else 'not kept (needs R2 %.1f, K > 0, valve moved over %g %%), %s' % (
                R2_MIN, MOVE_MIN, 'keeping the earlier fit' if fit else 'no fit yet')))
        return new if new['ok'] else fit

    def drop_check(self, sp, k):
        """Check k was passed over (a refinement replaced it, or was undone): its
        files go, so the run folder keeps only the check each result came
        from (Ryan, 9 Nov). The session log still has every sample."""
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
            self.calc('Check %d passed over: %s deleted' % (k, ', '.join(gone)))

    def point(self, sp, reach_gains, d, prior=None):
        """One test point: a result row for the table. prior: the point's
        learned row (load_learned), whose fit gives the first gains."""
        self.sp_now, self.check_why, self.phase_now = sp, '', '%g mbar reach' % sp
        self.calc('%g mbar' % sp)        # a heading line: the setpoint alone
        if prior:
            self.decide('Learned %s: KP %.2f TI %.2f, Ku %s, Pu %s s%s%s' % (
                prior.get('when') or 'earlier', prior['kp'], prior['ti_min'],
                '--' if prior.get('Ku') is None else '%.3f' % prior['Ku'],
                '--' if prior.get('Pu_s') is None else '%.1f' % prior['Pu_s'],
                ', fit K %.2f mbar/%%, L %g s, tau %g s' % (prior['K'], prior['L_s'], prior['tau_s'])
                if prior.get('K') and prior.get('L_s') is not None and prior.get('tau_s') else ', no fit',
                ' (changed ' + prior['trims'] + ')' if prior.get('trims') else ''))
        self.cuts = []
        x0 = self.reach(sp, *reach_gains, why='its learned gains' if prior else None)
        reach_cuts, self.cuts = self.cuts, []   # the reach gains are the point before's, not this point's
        r = self.relay(sp, x0, d)
        ku, pu = r['Ku'], r['Pu_s']
        self.say('%g mbar: Ku %.2f %%/mbar, Pu %.1f s (relay %.1f %%, swing %.3f mbar, %d cycles%s)'
                 % (sp, ku, pu, r['d_pct'], r['a_mbar'], r['cycles'], '' if r['steady'] else ', not steady'))

        # First gains: the rule on the learned fit, else Ku / 4 and 2 Pu.
        fit, first = None, ''
        if prior and prior.get('K') and prior.get('L_s') is not None and prior.get('tau_s'):
            old = prior.get('Ku')
            ratio = ku / old if old else None
            if ratio is None or 1 / LEARNED_AGREE <= ratio <= LEARNED_AGREE:
                fit = {'K': prior['K'], 'L': prior['L_s'], 'tau': prior['tau_s']}
            else:
                self.decide('Fresh: the new Ku is %.2f times the learned %.3f (more than %g x off: the plant has '
                          'changed), so the learned fit is not used' % (ratio, old, LEARNED_AGREE))
        if fit:
            kp, ti = rule_gains(fit, ku, pu)
            le = l_eff(fit, pu)
            ti_s = min(max(fit['tau'], TI_MIN_LEFF * le), 20.0 * le)
            self.decide('First gains from the learned fit (K %.2f, L %g s, tau %g s): L_eff = max(L, Pu / 4) = max(%g, %.1f) '
                      '= %.1f s; TI = min(max(tau, 2.5 L_eff), 20 L_eff) = min(max(%g, %.1f), %.0f) = %.0f s = %.2f min; '
                      'KP = min(TI / (5 K L_eff), Ku / 3) = min(%.0f / (5 x %.2f x %.1f), %.3f / 3) = %.2f %%/mbar' % (
                          fit['K'], fit['L'], fit['tau'], fit['L'], pu / 4, le, fit['tau'], TI_MIN_LEFF * le,
                          20 * le, ti_s, ti, ti_s, fit['K'], le, ku, kp))
            first = 'first gains from the learned fit'
            self.check_why = 'the first gains from the learned fit'
        else:
            kp, ti = relay_gains(ku, pu)
            self.check_why = 'the first gains, KP = Ku / %g, TI = %g Pu' % (FIRST_DIV, FIRST_TI_PU)
            self.decide('First gains, no fit yet: KP = Ku / %g = %.3f / %g = %.2f %%/mbar; TI = %g x Pu = %g x %.1f s = '
                      '%.0f s = %.2f min' % (FIRST_DIV, ku, FIRST_DIV, kp, FIRST_TI_PU, FIRST_TI_PU, pu,
                                             FIRST_TI_PU * pu, ti))
        self.say('%g mbar: KP %.2f TI %.2f for the first check' % (sp, kp, ti))
        kp0 = kp

        # Check and refine: after every check the fit is redone, then the first
        # rule that applies changes KP or TI, until the check passes, a
        # refinement makes it worse (undone), or MAX_REFINES.
        steps, k, refines, prev, end_note = [], 0, 0, None, ''
        fig = None
        while True:
            k += 1
            self.series = None
            fig, kp_after = self.check(sp, kp, ti, r, k)
            if fig is not None and kp_after < kp - 1e-9:
                fig['cut'] = True            # the watchdog cut KP while the check held
            kp = kp_after
            fit = self.refit(k, fit)
            if fig:
                self.say('%g mbar check %d (KP %.2f TI %.2f): %s' % (
                    sp, k, kp, ti, 'oscillating, swing %.2f mbar' % fig['wild_pp'] if fig.get('wild_pp') else
                    'overshoot %.1f %%, %d reversals, 63 %% in %s, 90 %% in %s, valve back-off %.0f %%, sag %d s%s' % (
                        fig['overshoot_pct'], fig['reversals'],
                        '--' if fig['t63_s'] is None else '%d s' % fig['t63_s'],
                        '--' if fig['t90_s'] is None else '%d s' % fig['t90_s'],
                        fig['backoff_pct'], fig['sag_s'], ', valve at an end' if fig['saturated'] else '')))
            if prev is not None:
                if worse_check(fig, prev['fig']):
                    self.decide('Undone: check %d is worse than check %d (overshoot %s against %s %%, %s against %s '
                              'reversals, 90 %% in %s against %s s), so KP %.2f TI %.2f stays and the refinements end' % (
                                  k, prev['k'], '--' if not fig else fig['overshoot_pct'], prev['fig']['overshoot_pct'],
                                  '--' if not fig else fig['reversals'], prev['fig']['reversals'],
                                  '--' if not fig or fig['t90_s'] is None else fig['t90_s'],
                                  '--' if prev['fig']['t90_s'] is None else prev['fig']['t90_s'], prev['kp'], prev['ti']))
                    self.drop_check(sp, k)
                    kp, ti, fig = prev['kp'], prev['ti'], prev['fig']
                    end_note = '%s made the check worse and was undone' % steps.pop()
                    break
                self.drop_check(sp, prev['k'])
            act = refine_rule(fig, kp, ti, ku, pu, fit)
            if act['kind'] == 'pass':
                if fig:
                    self.decide('Rule: the check passes with KP %.2f TI %.2f (overshoot %.1f %%, limit %g; %d reversals, '
                              'limit %d; no tail, not slow)' % (kp, ti, fig['overshoot_pct'], OVERSHOOT_MAX,
                                                                  fig['reversals'], REVERSALS_MAX))
                break
            if act['kind'] == 'limit':
                self.decide('Rule: %s: at its limit, so the check passes with KP %.2f TI %.2f' % (act['why'], kp, ti))
                end_note = act['note']
                break
            if refines >= MAX_REFINES:
                self.decide('Not applied: %s, but at most %d refinements at one setpoint' % (act['why'], MAX_REFINES))
                end_note = 'still %s after %d refinements' % (act['why'], MAX_REFINES)
                break
            self.decide('Rule: %s, so %s x %.2f: KP %.2f -> %.2f, TI %.2f -> %.2f' % (
                act['why'], 'KP' if act['kind'] == 'kp' else 'TI', act['f'], kp, act['kp'], ti, act['ti']))
            self.check_why = 'rule: %s, %s x %.2f' % (act['why'], 'KP' if act['kind'] == 'kp' else 'TI', act['f'])
            self.say('%g mbar: %s, %s x %.2f' % (sp, act['why'], 'KP' if act['kind'] == 'kp' else 'TI', act['f']))
            prev = {'kp': kp, 'ti': ti, 'fig': fig, 'k': k}
            kp, ti = act['kp'], act['ti']
            refines += 1
            steps.append('%s x%.2f' % ('KP' if act['kind'] == 'kp' else 'TI', act['f']))
        trims = self.cuts + steps
        self.calc('Result at %g mbar: KP %.2f, TI %.2f min%s' % (sp, kp, ti, ' (changed ' + ', '.join(trims) + ')'
                                                                 if trims else ''), 'result')
        self.now('')
        notes = []
        if end_note:
            notes.append(end_note)
        if not r['steady']:
            notes.append('relay not steady')
        if reach_cuts:
            notes.append('reach oscillated, its KP cut ' + ' '.join(reach_cuts))
        if first:
            notes.append(first)
        if not fit:
            notes.append('no good fit (R2 under %g)' % R2_MIN)
        fig = fig or {}
        return {'sp_mbar': sp, 'kp': kp, 'ti_min': ti, 'Ku': round(ku, 3), 'Pu_s': round(pu, 1),
                'kp_rule': kp0, 'd_pct': r['d_pct'], 'a_mbar': round(r['a_mbar'], 3), 'eps_mbar': round(r['eps_mbar'], 3),
                'cycles': r['cycles'], 'asym': None if r['asym'] is None else round(r['asym'], 2), 'valve_pct': x0,
                'trims': ', '.join(trims), 'overshoot_pct': fig.get('overshoot_pct'),
                'settle_s': None if fig.get('settle_s') is None else round(fig['settle_s']),
                'reversals': fig.get('reversals'), 't63_s': fig.get('t63_s'), 't90_s': fig.get('t90_s'),
                'backoff_pct': fig.get('backoff_pct'), 'sag_s': fig.get('sag_s'),
                'K': round(fit['K'], 3) if fit else None, 'L_s': fit['L'] if fit else None,
                'tau_s': fit['tau'] if fit else None, 'note': '; '.join(notes)}


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
                    for k in LEARNED_NUM:
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

